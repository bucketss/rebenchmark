#include "probe.h"

#include "amxxbridge.h"
#include "clock.h"
#include "engine.h"
#include "gamedll.h"
#include "regame.h"
#include "rehlds.h"

#include <cstring>

extern globalvars_t* gpGlobals;
extern enginefuncs_t g_engfuncs;

namespace probe {
namespace {

constexpr int kSlices = 10;

struct Slice {
    FrameStats frames;
    std::uint64_t startTicks = 0;
};

// Completed one-second slices, oldest overwritten first, plus the one filling.
Slice g_slices[kSlices];
int g_sliceNext = 0;
Slice g_current;
std::uint64_t g_sliceTicks = 0;

// The same second, but only frames that belong to the run, for its series.
FrameStats g_runSlice;
// Two ticks of sys_ticrate, refreshed once a second.
std::uint64_t g_twoTicksNs = 0;
std::uint32_t g_clockGeneration = 0;

void refreshTwoTicks() {
    const float ticrate = g_engfuncs.pfnCVarGetFloat("sys_ticrate");
    g_twoTicksNs = ticrate > 0.0f ? static_cast<std::uint64_t>(2e9 / ticrate) : 0;
}

// A frame is recorded one frame late: its period, and the CPU time it used,
// are only known once the next frame starts.
struct Pending {
    bool valid = false;
    std::uint64_t startTicks = 0;
    std::uint64_t cpuTicks = 0;
    Sample sample;
};
Pending g_pending;

// The cross-check is looking for a counter that shifted, not timing anything,
// so it samples every 100 frames rather than every frame. A jump is not
// going to un-happen while it waits, and comparing a span that long makes a
// wrong tick rate easier to see, not harder.
constexpr int kCheckEvery = 100;

struct CheckPoint {
    bool valid = false;
    std::uint64_t sourceTicks = 0;
    std::uint64_t osTicks = 0;
};
CheckPoint g_lastCheck;
int g_sinceCheck = 0;

int g_depth = 0;
bool g_hooked = false;
bool g_stalls = true;
std::string g_map;

bool g_recording = false;
Run g_run;
RunFinishedFn g_onLimitReached = nullptr;
ClockRevertedFn g_onClockReverted = nullptr;

// A map change in progress: set at ServerDeactivate, finished by the first
// frame of the next map.
struct Load {
    bool pending = false;
    bool inFrame = false;   // began inside a frame (rcon changelevel)
    std::uint64_t startTicks = 0;
    std::string from;
    std::uint64_t gamedllNs = 0;
    std::uint64_t amxxNs = 0;
    std::uint64_t edictAllocs = 0;
};
Load g_load;

double runSeconds(std::uint64_t ticks) {
    return ticks > g_run.startTicks ? clk::toSeconds(ticks - g_run.startTicks) : 0.0;
}

// Falls back to the OS clock when the forced TSC has disagreed with it too
// often to be worth trusting. Called between frames, with nothing in flight.
void revertClock() {
    // A run's start is in the old source's ticks; carry its elapsed time over.
    const double elapsed = g_recording ? runSeconds(clk::now()) : 0.0;
    clk::g_check.reverted = true;
    clk::useOs();
    amxx::setClock(REBENCH_CLOCK_OS);
    clockChanged();
    if (g_recording) {
        const std::uint64_t now = clk::now();
        const std::uint64_t back = clk::fromSeconds(elapsed);
        g_run.startTicks = now > back ? now - back : 0;
        g_run.clockRevertedInRun = true;
    }
    if (g_onClockReverted != nullptr) g_onClockReverted();
    g_engfuncs.pfnServerPrint("[ReBench] clock: the TSC disagreed with the OS clock too often, "
        "falling back to it\n");
}

void recordFrame(std::uint64_t startTicks, const Sample& sample) {
    g_current.frames.record(sample);

    // A frame already under way when the run began straddles its start.
    if (!g_recording || startTicks < g_run.startTicks) return;
    g_run.frames.record(sample);
    g_runSlice.record(sample);
    if (sample.periodNs > g_run.worst.periodNs) {
        WorstFrame& worst = g_run.worst;
        worst.periodNs = sample.periodNs;
        worst.busyNs = sample.busyNs;
        worst.gameNs = sample.gameNs;
        worst.cpuNs = sample.cpuNs;
        worst.hasCpu = sample.hasCpu;
        worst.amxxNs = sample.amxxNs;
        worst.hasAmxx = sample.hasAmxx;
        worst.deltaNs = sample.deltaNs;
        worst.edictChurn = sample.edictChurn;
        worst.atSeconds = runSeconds(startTicks);
        worst.mapTime = gpGlobals->time;
        worst.map = g_map;
        worst.players = countPlayers();
    }
}

int mapIndex() {
    if (g_run.maps.empty() || g_run.maps.back() != g_map) return -1;
    return static_cast<int>(g_run.maps.size()) - 1;
}

void pushSeriesRow(std::uint64_t now, const Players& players, int edicts) {
    const FrameStats& s = g_runSlice;
    if (s.period.count() == 0) return;
    SeriesRow row;
    row.atSeconds = runSeconds(now);
    row.map = mapIndex();
    row.frames = static_cast<std::uint32_t>(s.period.count());
    row.periodNs = static_cast<double>(s.period.sum());
    row.busyNs = static_cast<double>(s.busy.sum());
    row.hasGame = s.game.count() != 0;
    row.gameNs = static_cast<double>(s.game.sum());
    row.hasAmxx = s.amxxFrame.count() != 0;
    row.amxxNs = static_cast<double>(s.amxxFrame.sum());
    row.deltaNs = static_cast<double>(s.delta.sum());
    row.hasCpu = s.cpu.count() != 0;
    row.cpuNs = static_cast<double>(s.cpu.sum());
    row.overTwoTicks = static_cast<std::uint32_t>(s.overTwoTicks);
    row.periodMaxNs = s.period.max();
    row.busyP99Ns = static_cast<std::uint64_t>(s.busy.percentile(99.0));
    row.busyMaxNs = s.busy.max();
    row.stalled = static_cast<std::uint32_t>(s.stalled);
    row.edictChurn = static_cast<std::uint32_t>(s.edictChurn);
    row.players = players;
    row.edicts = edicts;
    g_run.series.push_back(row);
    g_runSlice.clear();
}

void samplePopulation(const Players& players, int edicts) {
    ++g_run.playerSamples;
    g_run.humans.add(players.humans);
    g_run.bots.add(players.bots);
    g_run.hltv.add(players.hltv);
    g_run.edicts.add(edicts);
    if (gpGlobals->maxEntities > g_run.edictLimit) g_run.edictLimit = gpGlobals->maxEntities;
}

void rotate(std::uint64_t now) {
    if (g_recording) {
        const Players players = countPlayers();
        const int edicts = g_engfuncs.pfnNumberOfEntities();
        samplePopulation(players, edicts);
        pushSeriesRow(now, players, edicts);
    }
    refreshTwoTicks();
    g_slices[g_sliceNext] = g_current;
    g_sliceNext = (g_sliceNext + 1) % kSlices;
    g_current.frames.clear();
    g_current.startTicks = now;
}

void stageSpike(std::uint64_t startTicks, std::uint64_t busyNs, std::uint64_t gameNs) {
    const Players players = countPlayers();
    gamedll::Context context;
    context.busyNs = busyNs;
    context.gamedllNs = gameNs;
    // A frame already under way when the run began straddles its start, and
    // the subtraction would wrap: bench start runs inside SV_Frame, so the
    // frame it was typed in always starts before the run does.
    context.atSeconds = g_recording ? runSeconds(startTicks) : 0.0;
    context.mapTime = gpGlobals->time;
    context.map = g_map.c_str();
    context.humans = players.humans;
    context.bots = players.bots;

    gamedll::FrameExtra& extra = context.extra;
    extra.hasAmxx = amxx::active();
    extra.amxxNs = amxx::frameNs();
    extra.deltaNs = engine::frameDeltaNs();
    extra.deltaCalls = engine::frameDeltaCalls();
    extra.edictChurn = engine::frameEdictChurn();
    extra.hltv = players.hltv;
    extra.hasPhases = phases::active();
    if (extra.hasPhases) {
        extra.phaseEntries = phases::frameTop(extra.phase, gamedll::kSpikePhase);
    }
    if (extra.hasAmxx) {
        RebenchAmxxFrameEntry top[gamedll::kSpikePawn];
        extra.pawnEntries = amxx::frameTop(top, gamedll::kSpikePawn);
        for (int i = 0; i < extra.pawnEntries; ++i) {
            gamedll::SpikePawn& pawn = extra.pawn[i];
            std::strncpy(pawn.plugin, top[i].plugin ? top[i].plugin : "?", sizeof(pawn.plugin) - 1);
            std::strncpy(pawn.function, top[i].function ? top[i].function : "?",
                sizeof(pawn.function) - 1);
            pawn.calls = top[i].calls;
            pawn.selfNs = top[i].selfNs;
        }
    }
    gamedll::stageSpike(context);
}

void finishLoad(std::uint64_t endTicks) {
    if (!g_load.pending) return;
    g_load.pending = false;
    if (!g_recording || g_load.startTicks < g_run.startTicks) return;

    MapChange change;
    change.atSeconds = runSeconds(g_load.startTicks);
    change.from = g_load.from;
    change.to = g_map;
    change.loadNs = endTicks > g_load.startTicks ? clk::toNs(endTicks - g_load.startTicks) : 0;
    change.hasGame = gamedll::enabled();
    if (change.hasGame) change.gamedllNs = gamedll::topLevelTotalNs() - g_load.gamedllNs;
    change.hasAmxx = amxx::active();
    if (change.hasAmxx) change.amxxNs = amxx::totalNs() - g_load.amxxNs;
    engine::Stats stats;
    engine::snapshot(stats);
    change.edictAllocs = stats.edictAllocs - g_load.edictAllocs;
    g_run.mapChanges.push_back(change);
}

void frameHook(IRehldsHook_SV_Frame* chain) {
    // Both of these are syscalls on a host whose clocksource has no vDSO path,
    // and both are read before the wall clock so their cost falls outside the
    // busy window rather than being charged to the frame.
    const bool wantCpu = g_stalls && clk::g_cpu.available;
    const std::uint64_t cpu = wantCpu ? clk::cpuTicks() : 0;
    const bool doCheck = clk::checking() && ++g_sinceCheck >= kCheckEvery;
    const std::uint64_t checkOs = doCheck ? clk::osTicks() : 0;
    const std::uint32_t generation = g_clockGeneration;
    const std::uint64_t start = clk::now();
    gamedll::beginFrame();
    engine::beginFrame();
    regame::beginFrame();
    amxx::beginFrame();
    phases::beginFrame();
    ++g_depth;
    chain->callNext();
    --g_depth;
    const std::uint64_t end = clk::now();

    if (generation != g_clockGeneration) {
        g_pending.valid = false;
        gamedll::dropSpike();
        gamedll::clearFrame();
        g_current.startTicks = end;
        return;
    }

    // SV_Frame_Internal returns at once with no map loaded.
    if (!g_rehldsSv->IsActive()) {
        g_pending.valid = false;
        gamedll::dropSpike();
        gamedll::clearFrame();
        return;
    }

    // The map changed inside this frame, so the frame is mostly the load.
    if (g_load.pending && g_load.inFrame) {
        finishLoad(end);
        g_pending.valid = false;
        gamedll::dropSpike();
        gamedll::clearFrame();
        return;
    }
    // The load ran between frames and ended when this one began.
    if (g_load.pending) finishLoad(start);

    // A TSC that is not synchronised across cores can step back on migration.
    bool revert = false;
    if (doCheck) {
        g_sinceCheck = 0;
        if (g_lastCheck.valid && checkOs > g_lastCheck.osTicks) {
            const std::uint64_t sourceTicks =
                start > g_lastCheck.sourceTicks ? start - g_lastCheck.sourceTicks : 0;
            const bool agreed = clk::crossCheck(sourceTicks, checkOs - g_lastCheck.osTicks);
            if (!agreed && g_recording && clk::g_check.lastGapNs > g_run.clockWorstGapNs) {
                g_run.clockWorstGapNs = clk::g_check.lastGapNs;
            }
            revert = clk::shouldRevert();
        }
        g_lastCheck.valid = true;
        g_lastCheck.sourceTicks = start;
        g_lastCheck.osTicks = checkOs;
    }

    if (g_pending.valid && start > g_pending.startTicks) {
        Sample sample = g_pending.sample;
        sample.periodNs = clk::toNs(start - g_pending.startTicks);
        if (sample.hasCpu && cpu >= g_pending.cpuTicks) {
            sample.cpuNs = clk::cpuToNs(cpu - g_pending.cpuTicks);
        } else {
            sample.hasCpu = false;
        }
        recordFrame(g_pending.startTicks, sample);
        gamedll::commitSpike(sample.periodNs, sample.hasCpu, sample.cpuNs);
    } else {
        // No period for the frame that staged it.
        gamedll::dropSpike();
    }

    const std::uint64_t busyNs = end > start ? clk::toNs(end - start) : 0;
    const std::uint64_t gameNs = gamedll::frameGamedllNs();
    // Not the frame bench start ran in, which also paid for pricing the
    // instrumentation.
    const bool beforeRun = g_recording && start < g_run.startTicks;
    if (!beforeRun && gamedll::spikeWanted(busyNs)) stageSpike(start, busyNs, gameNs);
    gamedll::clearFrame();

    g_pending.valid = true;
    g_pending.startTicks = start;
    g_pending.cpuTicks = cpu;
    g_pending.sample = Sample();
    g_pending.sample.busyNs = busyNs;
    g_pending.sample.gameNs = gameNs;
    g_pending.sample.hasGame = gamedll::enabled();
    g_pending.sample.hasCpu = wantCpu;
    g_pending.sample.deltaNs = engine::frameDeltaNs();
    g_pending.sample.deltaCalls = engine::frameDeltaCalls();
    g_pending.sample.edictChurn = engine::frameEdictChurn();
    g_pending.sample.amxxNs = amxx::frameNs();
    g_pending.sample.hasAmxx = amxx::active();

    // Both spans are guarded against a clock that steps back: unsigned
    // subtraction would wrap to an enormous number and read as "long enough",
    // rotating the window or ending a run seconds after it started.
    if (g_current.startTicks == 0) {
        g_current.startTicks = start;
    } else if (end > g_current.startTicks && end - g_current.startTicks >= g_sliceTicks) {
        rotate(end);
    }

    if (g_recording && g_run.limitSeconds > 0.0 && end > g_run.startTicks &&
        end - g_run.startTicks >= clk::fromSeconds(g_run.limitSeconds)) {
        const Run finished = endRun("limit");
        if (g_onLimitReached != nullptr) g_onLimitReached(finished);
        return;
    }

    // Last, so the frame it fires on is fully accounted for on the old clock.
    if (revert) revertClock();
}

double measureWrapperNs() {
    // The cheapest of a few batches: interference only ever adds.
    constexpr int kIterations = 20000;
    std::uint64_t best = UINT64_MAX;
    std::uint64_t sink = 0;
    for (int i = 0; i < 5; ++i) {
        const std::uint64_t ns = gamedll::measureWrapperNs(kIterations, sink);
        if (ns < best) best = ns;
    }
    return static_cast<double>(best) / kIterations;
}

} // namespace

double measureCpuReadNs() {
    if (!clk::g_cpu.available) return 0.0;
    constexpr int kReads = 1000;
    volatile std::uint64_t sink = 0;
    const std::uint64_t start = clk::now();
    for (int i = 0; i < kReads; ++i) sink += clk::cpuTicks();
    return static_cast<double>(clk::toNs(clk::now() - start)) / kReads;
}

void FrameStats::record(const Sample& sample) {
    period.record(sample.periodNs);
    busy.record(sample.busyNs);
    if (g_twoTicksNs != 0 && sample.periodNs > g_twoTicksNs) ++overTwoTicks;
    for (int i = 0; i < kOverCount; ++i) {
        if (sample.periodNs > static_cast<std::uint64_t>(kOverMs[i]) * 1000000) ++overMs[i];
    }
    if (sample.hasGame) {
        game.record(sample.gameNs);
        engine.record(sample.busyNs > sample.gameNs ? sample.busyNs - sample.gameNs : 0);
    }
    if (sample.hasCpu) {
        cpu.record(sample.cpuNs);
        if (isStall(sample.busyNs, sample.cpuNs)) ++stalled;
    }
    if (sample.deltaCalls > 0) delta.record(sample.deltaNs);
    // Every frame, including the ones that ran no Pawn at all: a plugin that
    // is idle most frames should read as cheap.
    if (sample.hasAmxx) amxxFrame.record(sample.amxxNs);
    edictChurn += static_cast<std::uint64_t>(sample.edictChurn);
}

void FrameStats::clear() {
    period.clear();
    busy.clear();
    game.clear();
    engine.clear();
    cpu.clear();
    delta.clear();
    amxxFrame.clear();
    stalled = 0;
    edictChurn = 0;
    overTwoTicks = 0;
    for (std::uint64_t& count : overMs) count = 0;
}

void FrameStats::merge(const FrameStats& other) {
    period.merge(other.period);
    busy.merge(other.busy);
    game.merge(other.game);
    engine.merge(other.engine);
    cpu.merge(other.cpu);
    delta.merge(other.delta);
    amxxFrame.merge(other.amxxFrame);
    stalled += other.stalled;
    edictChurn += other.edictChurn;
    overTwoTicks += other.overTwoTicks;
    for (int i = 0; i < kOverCount; ++i) overMs[i] += other.overMs[i];
}

bool isStall(std::uint64_t busyNs, std::uint64_t cpuNs) {
    if (busyNs <= cpuNs) return false;
    // A sixteenth of the frame, never less than a millisecond: the floor keeps
    // ordinary sub-millisecond frames from tripping on jitter.
    constexpr std::uint64_t kFloorNs = 1000000;
    const std::uint64_t margin = busyNs / 16;
    return busyNs - cpuNs > (margin > kFloorNs ? margin : kFloorNs);
}

void Range::add(int value) {
    if (value < min) min = value;
    if (value > max) max = value;
    sum += value;
}

void start(RunFinishedFn onLimitReached, ClockRevertedFn onClockReverted) {
    g_onLimitReached = onLimitReached;
    g_onClockReverted = onClockReverted;
    g_sliceTicks = clk::fromSeconds(1.0);
    refreshTwoTicks();
    if (!g_hooked) {
        // Outermost, so the time of every other SV_Frame hook is inside ours.
        g_rehldsHooks->SV_Frame()->registerHook(&frameHook, HC_PRIORITY_UNINTERRUPTABLE);
        g_hooked = true;
    }
}

void stop() {
    if (g_hooked) {
        g_rehldsHooks->SV_Frame()->unregisterHook(&frameHook);
        g_hooked = false;
    }
}

bool inFrame() {
    return g_depth > 0;
}

bool stallDetection() {
    return g_stalls;
}

void setStallDetection(bool on) {
    g_stalls = on;
}

void mapStarted(const char* name) {
    g_map = name != nullptr ? name : "";
    if (g_recording && (g_run.maps.empty() || g_run.maps.back() != g_map)) {
        g_run.maps.push_back(g_map);
    }
    // Loading the map ran game DLL calls outside any frame of ours.
    gamedll::clearFrame();
}

void mapEnded() {
    if (g_recording) pushSeriesRow(clk::now(), countPlayers(), g_engfuncs.pfnNumberOfEntities());

    // The next period would span the map load.
    g_pending.valid = false;
    gamedll::dropSpike();

    g_load.pending = true;
    g_load.inFrame = g_depth > 0;
    g_load.startTicks = clk::now();
    g_load.from = g_map;
    g_load.gamedllNs = gamedll::topLevelTotalNs();
    g_load.amxxNs = amxx::totalNs();
    engine::Stats stats;
    engine::snapshot(stats);
    g_load.edictAllocs = stats.edictAllocs;
}

void clockChanged() {
    // The timestamps already in hand are in the old source's ticks. Differencing
    // them against the new source's would invent a period out of two unrelated
    // clock domains, and because both are absolute counts the result comes out
    // timestamp-sized -- days, not milliseconds -- rather than merely wrong.
    ++g_clockGeneration;
    g_pending.valid = false;
    g_lastCheck = CheckPoint();
    g_sinceCheck = 0;
    gamedll::dropSpike();
    // The slice clock too, or the window would never rotate again, and the
    // length of a second in ticks with it.
    g_sliceTicks = clk::fromSeconds(1.0);
    g_current.startTicks = 0;
    for (Slice& slice : g_slices) slice.startTicks = 0;
    g_load.pending = false;
}

const std::string& currentMap() {
    return g_map;
}

bool recording() {
    return g_recording;
}

const Run& currentRun() {
    return g_run;
}

void beginRun(const std::string& tag, double limitSeconds) {
    g_run = Run();
    g_run.tag = tag;
    g_run.limitSeconds = limitSeconds;
    g_run.levelActive = gamedll::enabled();
    g_run.amxxActive = amxx::active();
    // Priced before the start, so the loops land outside the run.
    g_run.wrapperNs = g_run.levelActive ? measureWrapperNs() : 0.0;
    g_run.pawnTimingNs = g_run.amxxActive ? amxx::overheadNsPerCall() : 0.0;
    g_run.phasesActive = phases::active();
    g_run.phaseMarkNs = g_run.phasesActive ? phases::overheadNsPerMark() : 0.0;
    g_run.cpuReadNs = g_stalls ? measureCpuReadNs() : 0.0;
    g_run.clockReadNs = clk::g_info.nsPerRead;
    g_run.contentionStart = sys::contention();
    g_run.clockStart = clk::g_check;
    g_run.startTicks = clk::now();
    g_run.startedAt = std::time(nullptr);
    if (!g_map.empty()) g_run.maps.push_back(g_map);
    gamedll::snapshot(g_run.gameStart);
    engine::snapshot(g_run.engineStart);
    gamedll::snapshotInner(g_run.innerStart);
    regame::snapshot(g_run.regameStart);
    amxx::resetMax();
    amxx::snapshot(g_run.amxxStart);
    phases::resetMax();
    phases::snapshot(g_run.phasesStart);
    g_run.phasesDropped = phases::dropped();
    gamedll::resetRunMax();
    engine::resetRunMax();
    regame::resetRunMax();
    gamedll::resetSpikes();
    g_runSlice.clear();
    refreshTwoTicks();
    g_recording = true;
}

Run endRun(const char* reason) {
    const std::uint64_t now = clk::now();
    if (g_recording) {
        // The part-second since the last rotation.
        const Players players = countPlayers();
        const int edicts = g_engfuncs.pfnNumberOfEntities();
        if (g_run.playerSamples == 0) samplePopulation(players, edicts);
        pushSeriesRow(now, players, edicts);
    }
    g_recording = false;
    g_run.endReason = reason;
    g_run.stopTicks = now;
    g_run.stoppedAt = std::time(nullptr);
    gamedll::snapshotRun(g_run.gameStop);
    engine::snapshotRun(g_run.engineStop);
    gamedll::snapshotInner(g_run.innerStop);
    regame::snapshotRun(g_run.regameStop);
    amxx::snapshot(g_run.amxxStop);
    phases::snapshot(g_run.phasesStop);
    g_run.phasesDropped = phases::dropped() - g_run.phasesDropped;
    g_run.clockStop = clk::g_check;
    g_run.contentionStop = sys::contention();
    g_run.loadAverage = sys::loadAverage();
    g_run.gameLevel = g_run.frames.game.count() != 0;
    return g_run;
}

Window window() {
    Window result;
    std::uint64_t oldest = g_current.startTicks;
    for (const Slice& slice : g_slices) {
        if (slice.startTicks == 0) continue;
        result.frames.merge(slice.frames);
        if (slice.startTicks < oldest) oldest = slice.startTicks;
    }
    result.frames.merge(g_current.frames);
    const std::uint64_t now = clk::now();
    if (oldest != 0 && now > oldest) result.seconds = clk::toSeconds(now - oldest);
    return result;
}

Players countPlayers() {
    Players players;
    const int maxClients = g_rehldsSvs->GetMaxClients();
    for (int i = 0; i < maxClients; ++i) {
        IGameClient* client = g_rehldsSvs->GetClient(i);
        if (client == nullptr || !client->IsActive()) continue;
        const edict_t* edict = client->GetEdict();
        const int flags = edict != nullptr ? edict->v.flags : 0;
        if (flags & FL_PROXY) {
            ++players.hltv;
        } else if (flags & FL_FAKECLIENT) {
            ++players.bots;
        } else {
            ++players.humans;
        }
    }
    return players;
}

} // namespace probe
