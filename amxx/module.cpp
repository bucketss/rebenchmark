// rebench_amxx: an AMX Mod X module that times how long the server spends
// running Pawn.
//
// Everything a plugin does starts at amx_Exec: AMXX registers forwards with
// the game, and every forward, native callback and callfunc ends up there.
// Detouring it turns "plugins" from one line in the game DLL table into a
// table of plugins and public functions.
//
// This is a separate binary from rebench_mm only because AMXX will not load a
// module from anywhere but modules/<name>_amxx.dll. rebench_mm finds it at run
// time and folds its numbers into the same report; see src/amxxbridge.h.
//
// Why a detour rather than a hook: AMXX has no API for this. amx_Exec is
// called through a function pointer the core hands to modules, so the address
// is exact and no signature scan is involved, but the call sites are inside
// the core and cannot be redirected. The JIT does not move the entry point --
// amx_Exec branches to the compiled code internally -- so JIT and interpreted
// plugins are both covered.

#include "amxxabi.h"
#include "CDetour/detours.h"

#include "../src/amxxbridge.h"
#include "../src/clock.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define REBENCH_EXPORT extern "C" __declspec(dllexport)
#else
#define REBENCH_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

constexpr int kMaxEntries = 4096;
// Pawn recurses through natives that call forwards, but not deeply. Anything
// past this is still counted in the totals, just not attributed.
constexpr int kMaxDepth = 32;
constexpr int kCacheSlots = 8192;
constexpr int kNameSlots = 8192;
constexpr int kStringSlots = 16384;
constexpr std::size_t kPoolBytes = 256 * 1024;

typedef void (*PFN_PRINT_SRVCONSOLE)(const char* fmt, ...);

PFN_AMX_EXEC g_amxExec = nullptr;
PFN_FIND_AMXSCRIPT_BYAMX g_findScript = nullptr;
PFN_GET_AMXSCRIPTNAME g_scriptName = nullptr;
PFN_PRINT_SRVCONSOLE g_print = nullptr;

CDetour* g_detour = nullptr;
bool g_active = false;
char g_status[160] = "not attached";

RebenchAmxxEntry g_entries[kMaxEntries];
int g_entryCount = 0;

// (amx, index) is what the call gives us; resolving it to names costs a
// lookup and some string work, so it is done once and cached. Cleared when
// plugins reload, because an AMX at the same address is then a different
// plugin.
struct CacheSlot {
    const AMX* amx;
    int index;
    int entry;
};
CacheSlot g_cache[kCacheSlots];

struct Active {
    int entry;
    std::uint64_t start;
    std::uint64_t childTicks;
};
Active g_stack[kMaxDepth];
int g_depth = 0;

// Per-entry share of the current frame, so a spike can name the plugin behind
// it. Only the entries touched this frame are cleared.
struct FrameAcc {
    std::uint32_t calls;
    std::uint64_t selfNs;
};
FrameAcc g_frameAcc[kMaxEntries];
bool g_frameListed[kMaxEntries];
int g_frameTouched[kMaxEntries];
int g_frameTouchedCount = 0;

std::uint64_t g_frameNs = 0;

// Written by the overhead probe so its loop cannot be optimised away.
volatile std::uint64_t g_overheadSink = 0;
std::uint64_t g_totalNs = 0;
std::uint64_t g_totalCalls = 0;
std::uint64_t g_unattributed = 0;

char g_pool[kPoolBytes];
std::size_t g_poolUsed = 0;
std::uint32_t g_strings[kStringSlots];
int g_byName[kNameSlots];
bool g_warnedFull = false;

std::uint32_t fnv(const char* text, std::uint32_t hash = 2166136261u) {
    for (; *text != '\0'; ++text) {
        hash ^= static_cast<unsigned char>(*text);
        hash *= 16777619u;
    }
    return hash;
}

void warnFull(const char* what) {
    if (g_warnedFull) return;
    g_warnedFull = true;
    if (g_print) {
        g_print("[ReBench] plugin timing %s is full; new plugin functions are counted as "
                "unattributed until the server restarts\n", what);
    }
}

// Names live as long as the module. Interning also means the plugin name is
// stored once however many functions it has.
const char* intern(const char* text) {
    std::uint32_t slot = fnv(text) & (kStringSlots - 1);
    while (g_strings[slot] != 0) {
        const char* existing = g_pool + (g_strings[slot] - 1);
        if (std::strcmp(existing, text) == 0) return existing;
        slot = (slot + 1) & (kStringSlots - 1);
    }
    const std::size_t len = std::strlen(text);
    if (g_poolUsed + len + 1 > kPoolBytes) return nullptr;
    char* out = g_pool + g_poolUsed;
    std::memcpy(out, text, len + 1);
    g_strings[slot] = static_cast<std::uint32_t>(g_poolUsed + 1);
    g_poolUsed += len + 1;
    return out;
}

// The publics table, walked the way amx_GetPublic walks it. amx_GetPublic
// itself is not one of the functions the core exports to modules.
const char* publicName(const AMX* amx, int index) {
    if (index == -1) return "(main)";
    if (index < 0) return "(entry)";

    const AMX_HEADER* hdr = reinterpret_cast<const AMX_HEADER*>(amx->base);
    if (!hdr || hdr->magic != AMX_MAGIC || hdr->defsize <= 0) return nullptr;
    if (static_cast<unsigned>(index) >= AMXX_NUMENTRIES(hdr, publics, natives)) return nullptr;

    return AMXX_GETENTRYNAME(hdr, AMXX_GETENTRY(hdr, publics, index));
}

const char* pluginName(const AMX* amx) {
    if (!g_findScript || !g_scriptName) return nullptr;
    const char* path = g_scriptName(g_findScript(amx));
    if (!path || !*path) return nullptr;

    // getAmxScriptName returns a path; the file name is what people know.
    const char* slash = std::strrchr(path, '/');
    const char* back = std::strrchr(path, '\\');
    if (back > slash) slash = back;
    return slash ? slash + 1 : path;
}

inline unsigned hashKey(const AMX* amx, int index) {
    // The low bits of a heap pointer are mostly constant, so mix before
    // masking or every plugin lands in the same few slots.
    unsigned key = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(amx));
    key ^= static_cast<unsigned>(index) * 2654435761u;
    key ^= key >> 13;
    return key & (kCacheSlots - 1);
}

// -1 when the call cannot be attributed: an unnamed public, a full table, or
// a full string pool.
int resolve(const AMX* amx, int index) {
    const unsigned slot = hashKey(amx, index);
    CacheSlot& cached = g_cache[slot];
    if (cached.amx == amx && cached.index == index) return cached.entry;

    const char* function = publicName(amx, index);
    const char* plugin = pluginName(amx);
    if (!function || !plugin) return -1;

    std::uint32_t nameSlot = fnv(function, fnv(plugin) * 16777619u) & (kNameSlots - 1);
    int found = -1;
    while (g_byName[nameSlot] != 0) {
        const int candidate = g_byName[nameSlot] - 1;
        if (std::strcmp(g_entries[candidate].plugin, plugin) == 0 &&
            std::strcmp(g_entries[candidate].function, function) == 0) {
            found = candidate;
            break;
        }
        nameSlot = (nameSlot + 1) & (kNameSlots - 1);
    }

    if (found < 0) {
        if (g_entryCount >= kMaxEntries) {
            warnFull("table");
            return -1;
        }
        const char* ownedPlugin = intern(plugin);
        const char* ownedFunction = intern(function);
        if (!ownedPlugin || !ownedFunction) {
            warnFull("name pool");
            return -1;
        }

        found = g_entryCount++;
        g_byName[nameSlot] = found + 1;
        RebenchAmxxEntry& entry = g_entries[found];
        entry.plugin = ownedPlugin;
        entry.function = ownedFunction;
        entry.calls = 0;
        entry.selfNs = 0;
        entry.totalNs = 0;
        entry.maxNs = 0;
    }

    // One slot per key: a collision replaces the previous occupant rather than
    // probing, so a miss costs a resolve and never a long walk.
    cached.amx = amx;
    cached.index = index;
    cached.entry = found;
    return found;
}

} // namespace

// DETOUR_DECL_STATIC3 declares Detour_AmxExec plus Detour_AmxExec_Actual, the
// trampoline that reaches the real amx_Exec. AMXAPI is empty in the AMXX
// build, so this is cdecl on both sides.
DETOUR_DECL_STATIC3(Detour_AmxExec, int, AMX*, amx, cell*, retval, int, index) {
    // AMX_FLAG_BROWSE means the core is walking the opcode table with a
    // throwaway AMX, not running a plugin.
    if (!g_active || !amx || (amx->flags & AMX_FLAG_BROWSE) != 0) {
        return DETOUR_STATIC_CALL(Detour_AmxExec)(amx, retval, index);
    }

    if (g_depth >= kMaxDepth) {
        ++g_unattributed;
        return DETOUR_STATIC_CALL(Detour_AmxExec)(amx, retval, index);
    }

    Active& active = g_stack[g_depth++];
    active.entry = resolve(amx, index);
    active.childTicks = 0;
    active.start = clk::now();

    const int result = DETOUR_STATIC_CALL(Detour_AmxExec)(amx, retval, index);

    const std::uint64_t end = clk::now();
    --g_depth;

    // The TSC can step back when a thread migrates between unsynchronised
    // cores.
    std::uint64_t total = end > active.start ? end - active.start : 0;
    if (active.childTicks > total) active.childTicks = total;
    const std::uint64_t self = total - active.childTicks;

    if (g_depth > 0) {
        g_stack[g_depth - 1].childTicks += total;
    } else {
        // Only the outermost call counts towards the frame and the run, so a
        // plugin calling a plugin is not counted twice.
        const std::uint64_t totalNs = clk::toNs(total);
        g_frameNs += totalNs;
        g_totalNs += totalNs;
    }

    ++g_totalCalls;

    if (active.entry < 0) {
        ++g_unattributed;
        return result;
    }

    RebenchAmxxEntry& entry = g_entries[active.entry];
    const std::uint64_t totalNs = clk::toNs(total);
    const std::uint64_t selfNs = clk::toNs(self);
    ++entry.calls;
    entry.selfNs += selfNs;
    entry.totalNs += totalNs;
    if (totalNs > entry.maxNs) entry.maxNs = totalNs;

    FrameAcc& acc = g_frameAcc[active.entry];
    if (!g_frameListed[active.entry]) {
        g_frameListed[active.entry] = true;
        g_frameTouched[g_frameTouchedCount++] = active.entry;
    }
    ++acc.calls;
    acc.selfNs += selfNs;

    return result;
}

namespace {

const char* apiStatus() { return g_status; }
int apiActive() { return g_active ? 1 : 0; }

void apiSetActive(int on) {
    // g_depth is deliberately not touched here. Every push in the callback is
    // matched by a pop in the same invocation, so the stack is self-balancing
    // even if this flips mid-call; resetting it from outside is what would
    // corrupt it, by leaving pops without their pushes and driving the index
    // negative on the next frame.
    g_frameNs = 0;
    g_active = on != 0 && g_detour != nullptr;
}

// rebench_mm calls this between frames, with timing off or no plugin running,
// so no measurement is in flight when the source changes under it.
void apiSetClock(int clock) {
    if (clock == REBENCH_CLOCK_TSC) {
        clk::useTsc();
    } else if (clock == REBENCH_CLOCK_OS) {
        clk::useOs();
    } else {
        clk::useAuto();
    }
}

void apiBeginFrame() {
    g_frameNs = 0;
    for (int n = 0; n < g_frameTouchedCount; ++n) {
        const int e = g_frameTouched[n];
        g_frameAcc[e] = FrameAcc{};
        g_frameListed[e] = false;
    }
    g_frameTouchedCount = 0;
}

int apiFrameTop(RebenchAmxxFrameEntry* out, int max) {
    if (!out || max <= 0) return 0;
    int written = 0;
    for (int n = 0; n < g_frameTouchedCount; ++n) {
        const int e = g_frameTouched[n];
        const FrameAcc& acc = g_frameAcc[e];
        if (written == max && acc.selfNs <= out[max - 1].selfNs) continue;
        int at = written < max ? written : max - 1;
        while (at > 0 && out[at - 1].selfNs < acc.selfNs) {
            out[at] = out[at - 1];
            --at;
        }
        out[at].plugin = g_entries[e].plugin;
        out[at].function = g_entries[e].function;
        out[at].calls = acc.calls;
        out[at].selfNs = acc.selfNs;
        if (written < max) ++written;
    }
    return written;
}

// The detour's bookkeeping, done against scratch state: a cache probe, the
// stack push and pop, two clock reads and the counter updates. The trampoline
// jump itself is not in it.
std::uint64_t apiOverheadNs(int iterations) {
    if (iterations <= 0) return 0;
    RebenchAmxxEntry entry = {};
    FrameAcc acc = {};
    Active stack[1];

    const std::uint64_t start = clk::now();
    for (int i = 0; i < iterations; ++i) {
        const CacheSlot& cached = g_cache[hashKey(nullptr, i)];
        g_overheadSink += static_cast<std::uint64_t>(cached.index);

        Active& active = stack[0];
        active.entry = 0;
        active.childTicks = 0;
        active.start = clk::now();
        const std::uint64_t end = clk::now();

        std::uint64_t total = end > active.start ? end - active.start : 0;
        if (active.childTicks > total) active.childTicks = total;
        const std::uint64_t totalNs = clk::toNs(total);
        const std::uint64_t selfNs = clk::toNs(total - active.childTicks);
        ++entry.calls;
        entry.selfNs += selfNs;
        entry.totalNs += totalNs;
        if (totalNs > entry.maxNs) entry.maxNs = totalNs;
        ++acc.calls;
        acc.selfNs += selfNs;
    }
    g_overheadSink += entry.totalNs + acc.selfNs;
    return clk::toNs(clk::now() - start);
}

const char* apiScript(int index) {
    if (!g_scriptName || index < 0) return nullptr;
    const char* path = g_scriptName(index);
    if (!path) return nullptr;
    const char* slash = std::strrchr(path, '/');
    const char* back = std::strrchr(path, '\\');
    if (back > slash) slash = back;
    return slash ? slash + 1 : path;
}
std::uint64_t apiFrameNs() { return g_frameNs; }
std::uint64_t apiTotalNs() { return g_totalNs; }
std::uint64_t apiTotalCalls() { return g_totalCalls; }
std::uint64_t apiUnattributed() { return g_unattributed; }
int apiCount() { return g_entryCount; }

int apiEntry(int index, RebenchAmxxEntry* out) {
    if (!out || index < 0 || index >= g_entryCount) return 0;
    *out = g_entries[index];
    return 1;
}

void apiResetMax() {
    for (int i = 0; i < g_entryCount; ++i) g_entries[i].maxNs = 0;
}

const RebenchAmxxApi g_api = {
    REBENCH_AMXX_ABI,
    &apiStatus,
    &apiActive,
    &apiSetActive,
    &apiSetClock,
    &apiBeginFrame,
    &apiFrameNs,
    &apiTotalNs,
    &apiTotalCalls,
    &apiUnattributed,
    &apiCount,
    &apiEntry,
    &apiResetMax,
    &apiFrameTop,
    &apiOverheadNs,
    &apiScript,
};

amxx_module_info_s g_moduleInfo = {
    "rebench",
    "arc",
    REBENCH_VERSION,
    0,          // survives a map change, so a run can span one
    "REBENCH",
    "rebench",  // library name, so plugins can require it
    "",         // no libclass; the core itself substitutes "" rather than null
};

} // namespace

REBENCH_EXPORT const RebenchAmxxApi* rebench_amxx_api() { return &g_api; }

REBENCH_EXPORT int AMXX_Query(int* interfaceVersion, amxx_module_info_s* moduleInfo) {
    if (!interfaceVersion || !moduleInfo) return AMXX_PARAM;
    if (*interfaceVersion != AMXX_INTERFACE_VERSION) {
        *interfaceVersion = AMXX_INTERFACE_VERSION;
        return AMXX_IFVERS;
    }
    std::memcpy(moduleInfo, &g_moduleInfo, sizeof(amxx_module_info_s));
    return AMXX_OK;
}

REBENCH_EXPORT int AMXX_CheckGame(const char*) { return AMXX_GAME_OK; }

REBENCH_EXPORT int AMXX_Attach(PFN_REQ_FNPTR reqFnptrFunc) {
    if (!reqFnptrFunc) return AMXX_PARAM;

    g_amxExec = reinterpret_cast<PFN_AMX_EXEC>(reqFnptrFunc("amx_Exec"));
    g_findScript =
        reinterpret_cast<PFN_FIND_AMXSCRIPT_BYAMX>(reqFnptrFunc("FindAmxScriptByAmx"));
    g_scriptName =
        reinterpret_cast<PFN_GET_AMXSCRIPTNAME>(reqFnptrFunc("GetAmxScriptName"));
    g_print = reinterpret_cast<PFN_PRINT_SRVCONSOLE>(reqFnptrFunc("PrintSrvConsole"));

    if (!g_amxExec || !g_findScript || !g_scriptName) {
        std::snprintf(g_status, sizeof(g_status),
            "the AMXX core did not supply amx_Exec or the script name functions");
        // Returning AMXX_OK: without the detour the module measures nothing,
        // but refusing to load would take AMXX down with it.
        return AMXX_OK;
    }

    clk::init();
    CDetourManager::Init();

    // The detour is installed once and left in place. Removing it means
    // writing over the first bytes of amx_Exec, and the only thing that would
    // ask for that is a console command, which runs inside SV_Frame with a
    // plugin possibly on the stack. Timing is gated by g_active instead, which
    // costs a predictable few nanoseconds per call while off.
    g_detour = DETOUR_CREATE_STATIC_FIXED(Detour_AmxExec, reinterpret_cast<void*>(g_amxExec));
    if (!g_detour) {
        std::snprintf(g_status, sizeof(g_status), "could not build a trampoline for amx_Exec");
        return AMXX_OK;
    }

    g_detour->EnableDetour();
    std::snprintf(g_status, sizeof(g_status), "amx_Exec detoured at %p, clock %s",
        reinterpret_cast<void*>(g_amxExec), clk::sourceName());
    return AMXX_OK;
}

// Plugins are reloaded on a map change, so an AMX at a remembered address is
// now a different plugin. The name table itself is kept: the same plugin and
// function on the next map is the same row.
REBENCH_EXPORT void AMXX_PluginsUnloaded() {
    std::memset(g_cache, 0, sizeof(g_cache));
}

REBENCH_EXPORT int AMXX_Detach() {
    g_active = false;
    if (g_detour) {
        // Safe here in a way it is not from a console command: AMXX detaches
        // its modules while shutting down, with no plugin running.
        g_detour->Destroy();
        g_detour = nullptr;
    }
    g_amxExec = nullptr;
    g_findScript = nullptr;
    g_scriptName = nullptr;
    g_print = nullptr;
    std::snprintf(g_status, sizeof(g_status), "detached");
    return AMXX_OK;
}
