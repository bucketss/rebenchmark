#include "amxx.h"

#include "amxxbridge.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace amxx {
namespace {

const RebenchAmxxApi* g_api = nullptr;
bool g_looked = false;
const char* g_status = "rebench_amxx is not loaded";

// The module is found by file name, which is fixed: AMXX rewrites a
// modules.ini entry to modules/<name>_amxx<arch>.<ext>, and both halves ship
// together, so there is nothing to search for.
//
// The reference taken here is never released. AMXX frees a module's library
// the moment it detaches it (CModule::detach, right after AMXX_Detach), so
// "meta unload amxmodx" would otherwise unmap the image while the pointers
// below still point into it -- and active()/beginFrame() are on the frame
// path, so the next frame would jump into freed address space. Holding the
// image mapped makes that case read stale counters instead of crashing: the
// module's own detach stops the timing and destroys the detour first.
void* findExport() {
#ifdef _WIN32
#ifndef GET_MODULE_HANDLE_EX_FLAG_PIN
#define GET_MODULE_HANDLE_EX_FLAG_PIN 0x00000001
#endif
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, "rebench_amxx.dll", &module)) {
        return nullptr;
    }
    return reinterpret_cast<void*>(GetProcAddress(module, REBENCH_AMXX_EXPORT));
#else
    // RTLD_NOLOAD so this finds the module AMXX already loaded rather than
    // loading a second copy with its own counters. No dlclose: the reference
    // is what keeps the image mapped once AMXX drops its own.
    void* handle = dlopen("rebench_amxx_i386.so", RTLD_NOW | RTLD_NOLOAD);
    if (!handle) return nullptr;
    return dlsym(handle, REBENCH_AMXX_EXPORT);
#endif
}

void look() {
    if (g_looked && g_api) return;
    g_looked = true;

    RebenchAmxxApiFn get = reinterpret_cast<RebenchAmxxApiFn>(findExport());
    if (!get) {
        g_status = "rebench_amxx is not loaded";
        return;
    }

    const RebenchAmxxApi* api = get();
    if (!api) {
        g_status = "rebench_amxx returned no interface";
        return;
    }
    if (api->abi != REBENCH_AMXX_ABI) {
        // Two halves from different builds. Reading the struct anyway would
        // be reading whatever the other layout happens to put there.
        g_status = "rebench_amxx was built against a different bridge version";
        return;
    }

    g_api = api;
}

} // namespace

bool available() {
    look();
    return g_api != nullptr;
}

const char* status() {
    look();
    return g_api ? g_api->status() : g_status;
}

bool active() {
    // No look() here either: this is read once per frame, and a failed look
    // retries the search, which would put a module lookup in the frame path on
    // every server that does not have the module.
    return g_api && g_api->active() != 0;
}

void setActive(bool on) {
    look();
    if (g_api) g_api->setActive(on ? 1 : 0);
}

void setClock(int clock) {
    look();
    if (g_api) g_api->setClock(clock);
}

void beginFrame() {
    // No look() here: this is on the frame path, and the module cannot appear
    // part way through a frame.
    if (g_api) g_api->beginFrame();
}

std::uint64_t frameNs() {
    return g_api ? g_api->frameNs() : 0;
}

int frameTop(RebenchAmxxFrameEntry* out, int max) {
    return g_api ? g_api->frameTop(out, max) : 0;
}

std::uint64_t totalNs() {
    return g_api ? g_api->totalNs() : 0;
}

double overheadNsPerCall() {
    look();
    if (!g_api) return 0.0;
    // The cheapest of a few batches: interference only ever adds.
    constexpr int kIterations = 20000;
    std::uint64_t best = UINT64_MAX;
    for (int i = 0; i < 5; ++i) {
        const std::uint64_t ns = g_api->overheadNs(kIterations);
        if (ns < best) best = ns;
    }
    return static_cast<double>(best) / kIterations;
}

std::vector<std::string> scripts() {
    std::vector<std::string> out;
    look();
    if (!g_api) return out;
    for (int i = 0;; ++i) {
        const char* name = g_api->script(i);
        if (!name) break;
        out.push_back(name);
    }
    return out;
}

void resetMax() {
    look();
    if (g_api) g_api->resetMax();
}

void snapshot(Stats& out) {
    out = Stats();
    look();
    if (!g_api) return;

    out.present = true;
    out.totalNs = g_api->totalNs();
    out.totalCalls = g_api->totalCalls();
    out.unattributed = g_api->unattributed();

    const int count = g_api->count();
    out.entries.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        RebenchAmxxEntry raw = {};
        if (!g_api->entry(i, &raw)) continue;

        Entry entry;
        // The module owns these and keeps them for its lifetime, but copying
        // means a snapshot outlives an unload.
        entry.plugin = raw.plugin ? raw.plugin : "?";
        entry.function = raw.function ? raw.function : "?";
        entry.calls = raw.calls;
        entry.selfNs = raw.selfNs;
        entry.totalNs = raw.totalNs;
        entry.maxNs = raw.maxNs;
        out.entries.push_back(entry);
    }
}

Stats diff(const Stats& from, const Stats& to) {
    Stats out;
    out.present = to.present;
    if (!to.present) return out;

    out.totalNs = to.totalNs - from.totalNs;
    out.totalCalls = to.totalCalls - from.totalCalls;
    out.unattributed = to.unattributed - from.unattributed;

    for (const Entry& late : to.entries) {
        Entry entry = late;

        for (const Entry& early : from.entries) {
            if (early.plugin == late.plugin && early.function == late.function) {
                entry.calls -= early.calls;
                entry.selfNs -= early.selfNs;
                entry.totalNs -= early.totalNs;
                break;
            }
        }

        // maxNs is not differenced; it was reset when the run began.
        if (entry.calls == 0) continue;
        out.entries.push_back(entry);
    }

    std::sort(out.entries.begin(), out.entries.end(), [](const Entry& a, const Entry& b) {
        return a.selfNs > b.selfNs;
    });
    return out;
}

} // namespace amxx
