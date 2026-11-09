#include "engine.h"

#include <extdll.h>

#undef min
#undef max
#undef clamp

#include "clock.h"
#include "rehlds.h"

namespace engine {
namespace {

Stats g_stats;
bool g_installed = false;

// Worst single call since the run began; the sums in g_stats can be differenced
// between two snapshots, a high water mark cannot.
struct RunMax {
    std::uint64_t delta = 0;
    std::uint64_t edictAlloc = 0;
    std::uint64_t edictFree = 0;
};
RunMax g_runMax;

void keepMax(std::uint64_t ns, std::uint64_t& lifetime, std::uint64_t& run) {
    if (ns > lifetime) lifetime = ns;
    if (ns > run) run = ns;
}

std::uint64_t g_frameDeltaTicks = 0;
int g_frameDeltaCalls = 0;
int g_frameEdictChurn = 0;

// Delta encodes the entity list SV_WriteEntitiesToClient has already built.
// packet_entities_s is internal to the engine, so it stays an opaque pointer;
// entities per update comes from AddToFullPack's call count at level 1, over
// this call count.
int createPacketEntitiesHook(IRehldsHook_SV_CreatePacketEntities* chain, sv_delta_t type,
    IGameClient* client, struct packet_entities_s* to, sizebuf_t* msg) {
    const int before = msg != nullptr ? msg->cursize : 0;
    const std::uint64_t start = clk::now();
    const int result = chain->callNext(type, client, to, msg);
    const std::uint64_t end = clk::now();

    const std::uint64_t ticks = end > start ? end - start : 0;
    g_frameDeltaTicks += ticks;
    ++g_frameDeltaCalls;

    const std::uint64_t ns = clk::toNs(ticks);
    ++g_stats.deltaCalls;
    g_stats.deltaNs += ns;
    keepMax(ns, g_stats.maxDeltaNs, g_runMax.delta);
    if (msg != nullptr && msg->cursize > before) {
        g_stats.deltaBytes += static_cast<std::uint64_t>(msg->cursize - before);
    }
    return result;
}

edict_t* edictAllocHook(IRehldsHook_ED_Alloc* chain) {
    const std::uint64_t start = clk::now();
    edict_t* edict = chain->callNext();
    const std::uint64_t end = clk::now();

    const std::uint64_t ns = clk::toNs(end > start ? end - start : 0);
    ++g_stats.edictAllocs;
    g_stats.edictAllocNs += ns;
    keepMax(ns, g_stats.maxEdictAllocNs, g_runMax.edictAlloc);
    ++g_frameEdictChurn;
    if (edict == nullptr) ++g_stats.edictAllocFailures;
    return edict;
}

void edictFreeHook(IRehldsHook_ED_Free* chain, edict_t* edict) {
    const std::uint64_t start = clk::now();
    chain->callNext(edict);
    const std::uint64_t end = clk::now();

    const std::uint64_t ns = clk::toNs(end > start ? end - start : 0);
    ++g_stats.edictFrees;
    g_stats.edictFreeNs += ns;
    keepMax(ns, g_stats.maxEdictFreeNs, g_runMax.edictFree);
    ++g_frameEdictChurn;
}

} // namespace

void install() {
    if (g_installed) return;
    g_rehldsHooks->SV_CreatePacketEntities()->registerHook(&createPacketEntitiesHook,
        HC_PRIORITY_UNINTERRUPTABLE);
    g_rehldsHooks->ED_Alloc()->registerHook(&edictAllocHook, HC_PRIORITY_UNINTERRUPTABLE);
    g_rehldsHooks->ED_Free()->registerHook(&edictFreeHook, HC_PRIORITY_UNINTERRUPTABLE);
    g_installed = true;
}

void uninstall() {
    if (!g_installed) return;
    g_rehldsHooks->SV_CreatePacketEntities()->unregisterHook(&createPacketEntitiesHook);
    g_rehldsHooks->ED_Alloc()->unregisterHook(&edictAllocHook);
    g_rehldsHooks->ED_Free()->unregisterHook(&edictFreeHook);
    g_installed = false;
}

bool installed() {
    return g_installed;
}

void beginFrame() {
    g_frameDeltaTicks = 0;
    g_frameDeltaCalls = 0;
    g_frameEdictChurn = 0;
}

std::uint64_t frameDeltaNs() {
    return clk::toNs(g_frameDeltaTicks);
}

int frameDeltaCalls() {
    return g_frameDeltaCalls;
}

int frameEdictChurn() {
    return g_frameEdictChurn;
}

void snapshot(Stats& out) {
    out = g_stats;
}

void snapshotRun(Stats& out) {
    snapshot(out);
    out.maxDeltaNs = g_runMax.delta;
    out.maxEdictAllocNs = g_runMax.edictAlloc;
    out.maxEdictFreeNs = g_runMax.edictFree;
}

void resetRunMax() {
    g_runMax = RunMax();
}

} // namespace engine
