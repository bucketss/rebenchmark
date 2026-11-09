#include "clock.h"

#include "sysinfo.h"

#include <cmath>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace clk {

Info g_info;
CpuInfo g_cpu;

Check g_check;

namespace {

// Written by the read-cost probe so the loop cannot be optimised away.
volatile std::uint64_t g_probeSink = 0;

// The two clocks are read back to back, so the spans they report differ only by
// the jitter between those two reads plus whatever the TSC calibration got
// wrong. A millisecond covers the first and a tenth of a percent the second,
// which together are far narrower than any real jump: a migration or a move to
// an unsynchronised socket shifts the counter by milliseconds at least.
constexpr std::uint64_t kSlackNs = 1000000;
constexpr std::uint64_t kRevertAfter = 8;

// The TSC has to be this much cheaper than the OS clock before auto will read
// it uncorrected. A vDSO OS clock is already a few tens of nanoseconds and
// carries no caveat, so there is nothing to buy; a clock read the kernel has to
// service is two orders of magnitude dearer, which is worth a cross-check.
constexpr double kTscMustBeatOsBy = 4.0;

constexpr int kReadProbe = 1000;

void measureReadCost() {
    const std::uint64_t probeStart = now();
    for (int i = 0; i < kReadProbe; ++i) g_probeSink += now();
    g_info.nsPerRead = static_cast<double>(toNs(now() - probeStart)) / kReadProbe;
}

std::uint64_t readTsc() { return __rdtsc(); }

// Both sources timed against the OS clock and through a function pointer, so
// neither gets inlined and the two numbers are comparable. Only the ratio is
// used; nsPerRead is measured separately through the real inline path.
double probeReadNs(std::uint64_t (*read)(), double referenceHz) {
    const std::uint64_t t0 = osTicks();
    for (int i = 0; i < kReadProbe; ++i) g_probeSink += read();
    const std::uint64_t t1 = osTicks();
    return static_cast<double>(t1 - t0) * (1e9 / referenceHz) / kReadProbe;
}

bool tscCalibrated() { return g_info.tscHz > 1e8; }

}  // namespace

std::uint64_t cpuTicks() {
#ifdef _WIN32
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(GetCurrentThread(), &cycles)) return 0;
    return static_cast<std::uint64_t>(cycles);
#else
    timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
        static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

const char* cpuSourceName() {
    if (!g_cpu.available) return "none";
#ifdef _WIN32
    return "thread cycles";
#else
    return "thread cputime";
#endif
}

std::uint64_t osTicks() {
#ifdef _WIN32
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
        static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

double osHz() {
#ifdef _WIN32
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(frequency.QuadPart);
#else
    return 1e9;
#endif
}

void init() {
    const double referenceHz = osHz();

    constexpr int kSpins = 5;
    const auto span = static_cast<std::uint64_t>(referenceHz * 0.01);
    const std::uint64_t reference0 = osTicks();
    const std::uint64_t tsc0 = __rdtsc();
    std::uint64_t reference1 = reference0;
    double cpuHz = 0.0;
    std::uint64_t cpuFirst = 0;
    std::uint64_t cpuLast = 0;
    for (int i = 0; i < kSpins; ++i) {
        const std::uint64_t cpu0 = cpuTicks();
        const std::uint64_t from = osTicks();
        do {
            reference1 = osTicks();
        } while (reference1 - from < span);
        const std::uint64_t cpu1 = cpuTicks();
        if (i == 0) cpuFirst = cpu0;
        cpuLast = cpu1;
        const double spinSeconds = static_cast<double>(reference1 - from) / referenceHz;
        if (cpu1 > cpu0 && spinSeconds > 0.0) {
            const double hz = static_cast<double>(cpu1 - cpu0) / spinSeconds;
            if (hz > cpuHz) cpuHz = hz;
        }
    }
    const std::uint64_t tsc1 = __rdtsc();
    const double seconds = static_cast<double>(reference1 - reference0) / referenceHz;

    g_info.tscHz = seconds > 0.0 ? static_cast<double>(tsc1 - tsc0) / seconds : 0.0;
    g_info.osNsPerTick = 1e9 / referenceHz;

    // Price both before choosing between them.
    g_info.osReadNs = probeReadNs(&osTicks, referenceHz);
    g_info.tscReadNs = probeReadNs(&readTsc, referenceHz);

#ifdef _WIN32
    if (sys::cpu().invariantTsc && tscCalibrated() && cpuHz > 0.0 &&
        std::fabs(cpuHz / g_info.tscHz - 1.0) < 0.02) {
        cpuHz = g_info.tscHz;
    }
    g_cpu.available = cpuLast > cpuFirst && cpuHz > 1e8 && cpuHz < 2e10;
    g_cpu.hz = g_cpu.available ? cpuHz : 1.0;
#else
    // Linux counts nanoseconds.
    (void)cpuHz;
    g_cpu.available = cpuLast >= cpuFirst && cpuLast != 0;
    g_cpu.hz = 1e9;
#endif
    g_cpu.nsPerTick = 1e9 / g_cpu.hz;

    useAuto();
}

bool useTsc() {
    // Below this the calibration spin saw nothing that looks like a CPU clock,
    // so there is no rate to convert with.
    if (!tscCalibrated()) return false;
    // Only a switch onto the counter starts the cross-check tally afresh.
    if (g_info.source != Source::Tsc) g_check = Check();
    g_info.source = Source::Tsc;
    g_info.hz = g_info.tscHz;
    g_info.nsPerTick = 1e9 / g_info.hz;
    g_info.tscForced = !sys::cpu().invariantTsc;
    measureReadCost();
    return true;
}

void useOs() {
    g_info.source = Source::Os;
    g_info.hz = osHz();
    g_info.nsPerTick = 1e9 / g_info.hz;
    g_info.tscForced = false;
    measureReadCost();
}

void useAuto() {
    // The counter the CPU vouches for, or the one it does not vouch for but
    // which calibrated sanely and is far cheaper to read. A hypervisor masking
    // the invariant bit on hardware whose TSC is fine looks exactly like the
    // second case, and a span is cross-checked against the OS clock every 100
    // frames in it, so the risk is bounded and visible rather than assumed away.
    //
    // Once the cross-check has given up on the counter, auto stays off it; only
    // an explicit useTsc() goes back.
    if (g_check.reverted) {
        useOs();
        return;
    }
    if (tscCalibrated() &&
        (sys::cpu().invariantTsc || g_info.tscReadNs * kTscMustBeatOsBy < g_info.osReadNs)) {
        useTsc();
        return;
    }
    useOs();
}

bool checking() {
    return g_info.tscForced && !g_check.reverted;
}

bool crossCheck(std::uint64_t sourceTicks, std::uint64_t osDelta) {
    ++g_check.checks;
    const std::uint64_t mine = toNs(sourceTicks);
    const std::uint64_t theirs = osToNs(osDelta);
    const std::uint64_t gap = mine > theirs ? mine - theirs : theirs - mine;
    g_check.lastGapNs = gap;
    if (gap <= kSlackNs + theirs / 1000) return true;
    ++g_check.divergences;
    if (gap > g_check.worstNs) g_check.worstNs = gap;
    return false;
}

bool shouldRevert() {
    return !g_check.reverted && g_check.divergences >= kRevertAfter;
}

const char* sourceName() {
    return g_info.source == Source::Tsc ? (g_info.tscForced ? "tsc (forced)" : "tsc") : "os";
}

} // namespace clk
