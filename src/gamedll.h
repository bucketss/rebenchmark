#pragma once

#include <cstdint>
#include <string>

// Wraps the DLL_FUNCTIONS table the engine calls, so every entry point into the
// game is timed. The table comes from ReHLDS itself, so with Metamod loaded the
// measured cost is Metamod's dispatch plus every plugin hook plus the real game
// DLL: everything the engine asks the game to do.
namespace gamedll {

// Every member of DLL_FUNCTIONS, in declaration order.
#define REBENCH_DLL_FUNCTIONS(X)                                              \
    X(pfnGameInit) X(pfnSpawn) X(pfnThink) X(pfnUse) X(pfnTouch)              \
    X(pfnBlocked) X(pfnKeyValue) X(pfnSave) X(pfnRestore) X(pfnSetAbsBox)     \
    X(pfnSaveWriteFields) X(pfnSaveReadFields) X(pfnSaveGlobalState)          \
    X(pfnRestoreGlobalState) X(pfnResetGlobalState) X(pfnClientConnect)       \
    X(pfnClientDisconnect) X(pfnClientKill) X(pfnClientPutInServer)           \
    X(pfnClientCommand) X(pfnClientUserInfoChanged) X(pfnServerActivate)      \
    X(pfnServerDeactivate) X(pfnPlayerPreThink) X(pfnPlayerPostThink)         \
    X(pfnStartFrame) X(pfnParmsNewLevel) X(pfnParmsChangeLevel)               \
    X(pfnGetGameDescription) X(pfnPlayerCustomization)                        \
    X(pfnSpectatorConnect) X(pfnSpectatorDisconnect) X(pfnSpectatorThink)     \
    X(pfnSys_Error) X(pfnPM_Move) X(pfnPM_Init) X(pfnPM_FindTextureType)      \
    X(pfnSetupVisibility) X(pfnUpdateClientData) X(pfnAddToFullPack)          \
    X(pfnCreateBaseline) X(pfnRegisterEncoders) X(pfnGetWeaponData)           \
    X(pfnCmdStart) X(pfnCmdEnd) X(pfnConnectionlessPacket)                    \
    X(pfnGetHullBounds) X(pfnCreateInstancedBaselines)                        \
    X(pfnInconsistentFile) X(pfnAllowLagCompensation)

enum Function {
#define REBENCH_ENUM(name) k_##name,
    REBENCH_DLL_FUNCTIONS(REBENCH_ENUM)
#undef REBENCH_ENUM
    kFunctions
};

// Name without the "pfn" prefix.
const char* name(int function);

// total is entry to exit, self is total less the time spent in nested wrapped
// calls. Work the engine does on the game's behalf through enginefuncs counts
// as self time, because enginefuncs is not wrapped.
struct FuncStat {
    std::uint64_t calls = 0;
    std::uint64_t selfNs = 0;
    std::uint64_t totalNs = 0;
    std::uint64_t maxTotalNs = 0;
};

// Counters for every function, to subtract one point in time from another.
struct Totals {
    FuncStat f[kFunctions];
    // Calls skipped because nesting outran the stack.
    std::uint64_t overflows = 0;
};

constexpr int kSpikes = 32;
constexpr int kSpikeTop = 8;
constexpr int kSpikePawn = 4;

struct SpikeEntry {
    int function = -1;
    std::uint32_t calls = 0;
    std::uint64_t selfNs = 0;
    std::uint64_t totalNs = 0;
};

constexpr int kSpikePhase = 6;

struct SpikePhase {
    int phase = -1;
    std::uint32_t calls = 0;
    std::uint64_t selfNs = 0;
    std::uint64_t totalNs = 0;
};

// A plugin function's share of a spike.
struct SpikePawn {
    char plugin[40] = {};
    char function[40] = {};
    std::uint32_t calls = 0;
    std::uint64_t selfNs = 0;
};

// What else the frame did, beside the game DLL functions.
struct FrameExtra {
    std::uint64_t amxxNs = 0;
    bool hasAmxx = false;
    std::uint64_t deltaNs = 0;
    int deltaCalls = 0;
    int edictChurn = 0;   // allocs plus frees
    int hltv = 0;
    int pawnEntries = 0;
    SpikePawn pawn[kSpikePawn];
    bool hasPhases = false;
    int phaseEntries = 0;
    SpikePhase phase[kSpikePhase];
};

// One expensive frame and the functions that made it expensive.
struct Spike {
    std::uint64_t periodNs = 0;
    std::uint64_t busyNs = 0;
    std::uint64_t gamedllNs = 0;
    std::uint64_t cpuNs = 0;
    bool hasCpu = false;
    double atSeconds = 0.0;
    float mapTime = 0.0f;
    char map[32] = {};
    int humans = 0;
    int bots = 0;
    int entries = 0;
    SpikeEntry top[kSpikeTop];
    FrameExtra extra;
};

// What the probe knows about the frame being staged.
struct Context {
    std::uint64_t busyNs = 0;
    std::uint64_t gamedllNs = 0;
    double atSeconds = 0.0;
    float mapTime = 0.0f;
    const char* map = nullptr;
    int humans = 0;
    int bots = 0;
    FrameExtra extra;
};

// Patches the engine's table, or fails with the reason in error. Counting only
// happens between these and setEnabled(true).
bool install(std::string& error);

// Restores the original pointers. Fails if another module patched over ours,
// in which case unloading would crash and the wrappers stay in place.
bool uninstall(std::string& error);

bool installed();
bool enabled();
void setEnabled(bool on);

// Slots that no longer point at our wrappers.
int foreignSlots();

bool busy();

// Times a wrapped call was skipped because the call stack was too deep, since
// load. Totals carries the same count for differencing.
std::uint64_t depthOverflows();

// Time in top-level game DLL calls since load, frames or not. Differenced
// across a map change, it is what the load spent in the game.
std::uint64_t topLevelTotalNs();

// Frame boundaries, called from the SV_Frame hook.
void beginFrame();

// Sum of the time spent in top-level game calls this frame.
std::uint64_t frameGamedllNs();

// Whether this frame is worth keeping in the spike log.
bool spikeWanted(std::uint64_t busyNs);

// Copies the frame's counters into the staging slot. The period is not known
// until the next frame, so commitSpike() finishes the record.
void stageSpike(const Context& context);

// A staged record lives exactly one frame: the next frame either supplies the
// period it was missing or throws it away.
void commitSpike(std::uint64_t periodNs, bool hasCpu, std::uint64_t cpuNs);
void dropSpike();

void clearFrame();

// maxTotalNs is a high water mark, so it cannot be differenced like the sums.
// Each of the two readers keeps its own: snapshot() reports the worst call
// since resetTopMax(), snapshotRun() the worst since resetRunMax().
// Metamod dispatches StartFrame as: plugin pre-hooks in load order, the real
// game DLL, then plugin post-hooks in load order. Our own Metamod hooks bracket
// the inner part of it. With rebench loaded LAST in plugins.ini the bracket
// opens immediately before the game DLL, so StartFrame's total time less this
// is what the other plugins spend in their pre-hooks.
struct Inner {
    std::uint64_t calls = 0;
    std::uint64_t ns = 0;
};

void markStartFrameEnter();
void markStartFrameLeave();
void snapshotInner(Inner& out);

// Cost of the wrapper itself over `iterations` calls, in ns, with no game DLL
// underneath. Restores the counters it touches, so it leaves no trace in the
// totals. sink keeps the loop from being optimised away.
std::uint64_t measureWrapperNs(int iterations, std::uint64_t& sink);

void snapshot(Totals& out);
void snapshotRun(Totals& out);
void resetTopMax();
void resetRunMax();

void resetSpikes();
int spikeCount();
const Spike& spike(int index);

// Frames under this are never staged. Zero keeps the worst ones whatever they
// cost.
void setSpikeFloorNs(std::uint64_t ns);
std::uint64_t spikeFloorNs();

} // namespace gamedll
