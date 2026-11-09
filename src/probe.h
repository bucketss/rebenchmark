#pragma once

#include "amxx.h"
#include "clock.h"
#include "engine.h"
#include "gamedll.h"
#include "histogram.h"
#include "phases.h"
#include "regame.h"
#include "sysinfo.h"

#include <climits>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace probe {

struct Players {
    int humans = 0;
    int bots = 0;
    int hltv = 0;
};

// A frame whose wall time ran well past the CPU time the thread actually got
// was not computing: descheduled, blocked, or the host took the CPU away.
//
// The margin scales with the frame, because a fixed one means opposite things
// at the two ends. A millisecond is the whole story in a 2 ms frame and noise
// in a 400 ms one -- a flat threshold called a 457 ms frame stalled on the
// strength of 1.7 ms of scheduling jitter, when it had spent 99.6% of itself
// genuinely computing.
bool isStall(std::uint64_t busyNs, std::uint64_t cpuNs);

struct Sample {
    std::uint64_t periodNs = 0;
    std::uint64_t busyNs = 0;
    std::uint64_t gameNs = 0;
    std::uint64_t cpuNs = 0;
    std::uint64_t deltaNs = 0;
    std::uint64_t amxxNs = 0;
    int deltaCalls = 0;
    int edictChurn = 0;
    bool hasGame = false;
    bool hasCpu = false;
    bool hasAmxx = false;
};

constexpr int kOverMs[] = {5, 10, 20, 50, 100};
constexpr int kOverCount = static_cast<int>(sizeof(kOverMs) / sizeof(kOverMs[0]));

// Frame timing over a span. period runs from one SV_Frame start to the next, so
// it includes the host's sleep and anything it runs between frames; busy is the
// time inside SV_Frame. game and engine split busy between the game DLL table
// and the rest of the engine, and are only filled while the game DLL wrappers
// are counting.
struct FrameStats {
    Histogram period;
    Histogram busy;
    Histogram game;
    Histogram engine;
    Histogram cpu;
    // SV_CreatePacketEntities, over only the frames that sent a client update.
    Histogram delta;
    // Time in Pawn per frame, over every frame while plugin timing was on.
    Histogram amxxFrame;
    // Frames that spent a meaningful part of their busy time not running.
    std::uint64_t stalled = 0;
    // Edict allocs plus frees inside frames.
    std::uint64_t edictChurn = 0;
    std::uint64_t overTwoTicks = 0;
    std::uint64_t overMs[kOverCount] = {};

    void record(const Sample& sample);
    void clear();
    void merge(const FrameStats& other);
};

struct Range {
    int min = INT_MAX;
    int max = 0;
    long long sum = 0;

    void add(int value);
};

struct WorstFrame {
    std::uint64_t periodNs = 0;
    std::uint64_t busyNs = 0;
    std::uint64_t gameNs = 0;
    std::uint64_t cpuNs = 0;
    std::uint64_t amxxNs = 0;
    std::uint64_t deltaNs = 0;
    int edictChurn = 0;
    bool hasCpu = false;
    bool hasAmxx = false;
    double atSeconds = 0.0;   // from the start of the run
    float mapTime = 0.0f;
    std::string map;
    Players players;
};

// One second of a run.
struct SeriesRow {
    double atSeconds = 0.0;   // end of the second, from the start of the run
    int map = -1;             // index into Run::maps
    std::uint32_t frames = 0;
    double periodNs = 0.0;    // sums
    double busyNs = 0.0;
    double gameNs = 0.0;
    double amxxNs = 0.0;
    double deltaNs = 0.0;
    double cpuNs = 0.0;
    std::uint32_t overTwoTicks = 0;   // periods longer than 2 / sys_ticrate
    std::uint64_t periodMaxNs = 0;
    std::uint64_t busyP99Ns = 0;
    std::uint64_t busyMaxNs = 0;
    std::uint32_t stalled = 0;
    std::uint32_t edictChurn = 0;
    Players players;
    int edicts = 0;
    bool hasGame = false;
    bool hasAmxx = false;
    bool hasCpu = false;
};

// From ServerDeactivate to the first frame of the next map. None of it is in the
// frame numbers, since periods that span a load are dropped.
struct MapChange {
    double atSeconds = 0.0;
    std::string from;
    std::string to;
    std::uint64_t loadNs = 0;
    std::uint64_t gamedllNs = 0;
    std::uint64_t amxxNs = 0;
    std::uint64_t edictAllocs = 0;
    bool hasGame = false;
    bool hasAmxx = false;
};

struct Run {
    std::string tag;
    std::string endReason;
    std::uint64_t startTicks = 0;
    std::uint64_t stopTicks = 0;
    std::time_t startedAt = 0;
    std::time_t stoppedAt = 0;
    double limitSeconds = 0.0;
    bool levelActive = false;
    bool amxxActive = false;
    bool phasesActive = false;
    FrameStats frames;
    WorstFrame worst;
    int playerSamples = 0;   // one per second
    Range humans;
    Range bots;
    Range hltv;
    Range edicts;
    int edictLimit = 0;
    std::vector<std::string> maps;
    std::vector<SeriesRow> series;
    std::vector<MapChange> mapChanges;
    // The forced-TSC cross-check at both ends, and the worst disagreement
    // inside the run.
    clk::Check clockStart;
    clk::Check clockStop;
    std::uint64_t clockWorstGapNs = 0;
    bool clockRevertedInRun = false;
    sys::Contention contentionStart;
    sys::Contention contentionStop;
    double loadAverage = -1.0;
    // Measured at the start, per call, for the instrumentation estimate. Zero
    // when the thing was not on.
    double wrapperNs = 0.0;
    double pawnTimingNs = 0.0;
    double phaseMarkNs = 0.0;
    double cpuReadNs = 0.0;
    double clockReadNs = 0.0;
    engine::Stats engineStart;
    engine::Stats engineStop;
    // Only filled when the AMXX module is loaded and timing was on.
    amxx::Stats amxxStart;
    amxx::Stats amxxStop;
    phases::Stats phasesStart;
    phases::Stats phasesStop;
    std::uint64_t phasesDropped = 0;
    gamedll::Inner innerStart;
    gamedll::Inner innerStop;
    regame::Stats regameStart;
    regame::Stats regameStop;
    // Game DLL counters at both ends of the run; the report is the difference.
    bool gameLevel = false;
    gamedll::Totals gameStart;
    gamedll::Totals gameStop;
};

struct Window {
    double seconds = 0.0;
    FrameStats frames;
};

using RunFinishedFn = void (*)(const Run&);
using ClockRevertedFn = void (*)();

// Registers the SV_Frame hook. rehldsInit() and clk::init() come first.
// onClockReverted runs after the probe falls back to the OS clock by itself, so
// the caller can rebase any tick counts of its own.
void start(RunFinishedFn onLimitReached, ClockRevertedFn onClockReverted);
void stop();

// True while SV_Frame is on the stack, which is where rcon commands run.
bool inFrame();

void mapStarted(const char* name);
void mapEnded();

// Drops timestamps taken on the previous clock. Call after any source switch.
void clockChanged();

// The per-frame thread CPU time read, which is what tells a frame the host
// descheduled from one the game really spent. On by default: it is the only way
// to tell those apart, and it costs one read a frame. Worth turning off only on
// a host where that read is expensive and the last of the interference matters
// more than knowing why a frame was slow.
bool stallDetection();
void setStallDetection(bool on);
double measureCpuReadNs();
const std::string& currentMap();

bool recording();
const Run& currentRun();
void beginRun(const std::string& tag, double limitSeconds);
Run endRun(const char* reason);

// The last ten seconds or so.
Window window();

Players countPlayers();

} // namespace probe
