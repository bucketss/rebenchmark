#pragma once

#include <cstdint>

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace clk {

enum class Source { Tsc, Os };

struct Info {
    Source source = Source::Os;
    double hz = 1.0;
    double nsPerTick = 1.0;
    // What one now() costs, measured at init. A TSC or vDSO read is 5-30 ns; a
    // read the kernel has to service, because the clocksource has no vDSO path,
    // is 300-1000. Every wrapped call spends two of these, so this is what
    // decides whether level 1 is affordable on a given host.
    double nsPerRead = 0.0;
    // The rate the TSC ran at during calibration, kept even when the TSC is not
    // the chosen source. A hypervisor that merely masks the invariant bit still
    // leaves a counter ticking at the nominal clock, which is worth being able
    // to tell from one that is genuinely unusable.
    double tscHz = 0.0;
    double osNsPerTick = 1.0;
    // What each source costs to read, priced at init through the same call
    // shape so the two are comparable. auto weighs them against each other.
    double osReadNs = 0.0;
    double tscReadNs = 0.0;
    // The TSC is in use despite the CPU not advertising it invariant, because
    // it was asked for. The cross-check runs only in this state.
    bool tscForced = false;
};

extern Info g_info;

// Time this thread was actually running on a CPU, as opposed to wall time. A
// frame whose wall time far exceeds its CPU time was not computing: the process
// was descheduled, blocked, or the host took the CPU away.
struct CpuInfo {
    bool available = false;
    double hz = 1.0;
    double nsPerTick = 1.0;
};

extern CpuInfo g_cpu;

// QueryPerformanceCounter on Windows, CLOCK_MONOTONIC (ns) on Linux.
std::uint64_t osTicks();
double osHz();

// QueryThreadCycleTime on Windows, CLOCK_THREAD_CPUTIME_ID on Linux. Zero when
// unavailable. Costs about 900 ns on 32-bit Windows (the WOW64 thunk) against
// about 40 ns on Linux, so it is read once per frame, not twice.
std::uint64_t cpuTicks();
const char* cpuSourceName();

// Picks the TSC when the CPU reports it invariant, calibrated against the OS
// clock over 50 ms of spinning; otherwise the OS clock. Call once, before now().
void init();

// Switch source after init. useTsc takes the rate measured during calibration
// whether or not the CPU advertised the counter invariant, which is what a
// hypervisor masking that bit calls for; it returns false when no sane rate was
// seen. All three re-measure nsPerRead. Only safe with no measurement in
// flight: a start timestamp taken from one source and an end from the other is
// meaningless.
bool useTsc();
void useOs();

// The TSC when the CPU vouches for it, and also when it does not but the
// counter calibrated sanely and reading it is much cheaper than the OS clock --
// a hypervisor masking the invariant bit on hardware whose TSC is fine looks
// exactly like that, and the cross-check covers the case where it is not.
// Otherwise the OS clock, which needs no caveat.
void useAuto();

// How far the TSC and the OS clock disagreed about wall time. Only filled while
// the TSC is forced: the counter is then uncorrected, so a live migration or a
// move to a socket with an unsynchronised TSC would show up here rather than
// quietly skewing a run.
struct Check {
    std::uint64_t checks = 0;
    std::uint64_t divergences = 0;
    std::uint64_t worstNs = 0;
    // The most recent sample's disagreement, diverged or not.
    std::uint64_t lastGapNs = 0;
    bool reverted = false;
};

extern Check g_check;

// True while the cross-check is worth running.
bool checking();

// One span, measured both ways. False when they disagree by more than the
// sampling skew and the calibration error together can explain.
bool crossCheck(std::uint64_t sourceTicks, std::uint64_t osDelta);

// Enough disagreements that the counter should not be trusted any further.
bool shouldRevert();

const char* sourceName();

inline std::uint64_t now() {
    return g_info.source == Source::Tsc ? __rdtsc() : osTicks();
}

inline std::uint64_t toNs(std::uint64_t ticks) {
    return static_cast<std::uint64_t>(static_cast<double>(ticks) * g_info.nsPerTick);
}

inline std::uint64_t fromSeconds(double seconds) {
    return static_cast<std::uint64_t>(seconds * g_info.hz);
}

inline double toSeconds(std::uint64_t ticks) {
    return static_cast<double>(ticks) / g_info.hz;
}

inline std::uint64_t cpuToNs(std::uint64_t ticks) {
    return static_cast<std::uint64_t>(static_cast<double>(ticks) * g_cpu.nsPerTick);
}

inline std::uint64_t osToNs(std::uint64_t ticks) {
    return static_cast<std::uint64_t>(static_cast<double>(ticks) * g_info.osNsPerTick);
}

} // namespace clk
