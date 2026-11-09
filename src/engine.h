#pragma once

#include <cstdint>

// Engine work the game DLL table cannot see: building the per-client entity
// snapshot, and edict churn. Hooked through ReHLDS hookchains, which cost far
// less per call than the game DLL wrappers do, so these stay on at every level.
namespace engine {

struct Stats {
    // SV_CreatePacketEntities, one call per client update. It only delta
    // encodes an entity list that SV_WriteEntitiesToClient has already built by
    // calling AddToFullPack for every entity. That build is the expensive half
    // and has no hookchain, so it is only visible through AddToFullPack at
    // level 1.
    std::uint64_t deltaCalls = 0;
    std::uint64_t deltaNs = 0;
    std::uint64_t maxDeltaNs = 0;
    std::uint64_t deltaBytes = 0;

    // ED_Alloc / ED_Free. Churn here is entities being created and destroyed,
    // which is a common source of hitches and of running out of edicts.
    //
    // ED_Alloc is timed because it degrades: it linearly scans the edict array
    // for a slot it is allowed to reuse, and an edict only becomes reusable
    // half a second of server time after it was freed. The more entities are in
    // play, and the harder they churn, the longer that scan runs. ED_Free is
    // flat by comparison, and is timed to have something to read it against.
    std::uint64_t edictAllocs = 0;
    std::uint64_t edictAllocNs = 0;
    std::uint64_t maxEdictAllocNs = 0;
    std::uint64_t edictFrees = 0;
    std::uint64_t edictFreeNs = 0;
    std::uint64_t maxEdictFreeNs = 0;
    std::uint64_t edictAllocFailures = 0;
};

void install();
void uninstall();
bool installed();

// Frame boundaries, called from the SV_Frame hook.
void beginFrame();
std::uint64_t frameDeltaNs();
int frameDeltaCalls();
int frameEdictChurn();

// The max fields are high water marks, so they cannot be differenced like the
// sums. snapshot() reports the worst call since the plugin loaded, snapshotRun()
// the worst since resetRunMax().
void snapshot(Stats& out);
void snapshotRun(Stats& out);
void resetRunMax();

} // namespace engine
