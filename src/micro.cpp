#include "micro.h"

#include <extdll.h>
#include <meta_api.h>

// extdll.h pulls in Valve's minmax.h, which defines min/max/clamp as macros and
// breaks the std:: versions at their call sites.
#undef min
#undef max
#undef clamp

#include "clock.h"
#include "gamedll.h"
#include "rehlds.h"

#include <algorithm>
#include <cstring>

namespace micro {
namespace {

// Batches, not one long loop: a single descheduling event would otherwise wreck
// the whole measurement, and the median of several batches survives it.
constexpr int kBatches = 9;
// Per batch. Short enough that the server never loses a frame to a bench.
constexpr double kBatchMs = 1.0;
// The calibration pass, to size a batch before timing one.
constexpr int kProbeOps = 200;

// Written by every loop so the optimiser cannot delete it.
volatile std::uint64_t g_sink = 0;

// A bench runs `ops` operations and returns the nanoseconds they took.
//
// EVERY BENCH MUST BE FREE OF SIDE EFFECTS. The loop count is not chosen by the
// author: a calibration pass sizes a batch to fit kBatchMs and then runs
// kBatches of them, so whatever one operation does happens an unbounded number
// of times. A create/remove entity pair looked harmless and killed the server
// with "ED_Alloc_internal: no free edicts", because ED_Alloc refuses to reuse
// an edict until half a second of *server* time has passed (pr_edict.cpp) and
// server time does not advance inside the frame this runs in. Nothing that
// allocates, frees, writes a cvar, sends a message or touches an entity belongs
// here. Read-only calls only.
using Loop = std::uint64_t (*)(int ops);

struct Bench {
    const char* name;
    const char* what;
    Loop loop;
    // Empty when it can run, otherwise why it cannot.
    const char* (*unavailable)();
};

const char* needMap() {
    return g_rehldsSv->IsActive() ? nullptr : "no map running";
}

const char* needCpuClock() {
    return clk::g_cpu.available ? nullptr : "no cpu clock on this platform";
}

// --- the benches -----------------------------------------------------------

std::uint64_t loopClock(int ops) {
    const std::uint64_t start = clk::now();
    for (int i = 0; i < ops; ++i) g_sink += clk::now();
    return clk::toNs(clk::now() - start);
}

// Timed whether or not this build uses the TSC. A host can report the counter
// as not invariant while still ticking it at the nominal rate -- a hypervisor
// masking the CPUID bit looks exactly like that -- and the question that
// answers is whether there is a cheap clock here going unused. A hypervisor
// that traps rdtsc instead shows up as thousands of ns, which is the answer
// too. Reading it never changes which clock the plugin actually uses.
std::uint64_t loopRdtsc(int ops) {
    const std::uint64_t start = clk::now();
    for (int i = 0; i < ops; ++i) g_sink += __rdtsc();
    return clk::toNs(clk::now() - start);
}

std::uint64_t loopCpuClock(int ops) {
    const std::uint64_t start = clk::now();
    for (int i = 0; i < ops; ++i) g_sink += clk::cpuTicks();
    return clk::toNs(clk::now() - start);
}

std::uint64_t loopWrapper(int ops) {
    std::uint64_t sink = 0;
    const std::uint64_t ns = gamedll::measureWrapperNs(ops, sink);
    g_sink += sink;
    return ns;
}

std::uint64_t loopCvar(int ops) {
    const std::uint64_t start = clk::now();
    for (int i = 0; i < ops; ++i) {
        g_sink += static_cast<std::uint64_t>(g_engfuncs.pfnCVarGetFloat("sv_gravity"));
    }
    return clk::toNs(clk::now() - start);
}

std::uint64_t loopEdictOfIndex(int ops) {
    const std::uint64_t start = clk::now();
    for (int i = 0; i < ops; ++i) {
        g_sink += reinterpret_cast<std::uintptr_t>(g_engfuncs.pfnPEntityOfEntIndex(1));
    }
    return clk::toNs(clk::now() - start);
}

std::uint64_t loopTraceLine(int ops) {
    // Straight down from well above the world origin: a trace that actually
    // walks the BSP rather than leaving in the first node.
    const Vector from(0.0f, 0.0f, 4096.0f);
    const Vector to(0.0f, 0.0f, -4096.0f);
    TraceResult trace = {};
    const std::uint64_t start = clk::now();
    for (int i = 0; i < ops; ++i) {
        g_engfuncs.pfnTraceLine(from, to, ignore_monsters, nullptr, &trace);
        g_sink += static_cast<std::uint64_t>(trace.flFraction * 1000.0f);
    }
    return clk::toNs(clk::now() - start);
}

const Bench g_benches[] = {
    {"clock", "clk::now(), the timer behind every measurement", &loopClock, nullptr},
    {"rdtsc", "raw __rdtsc(), what the clock would cost if the TSC were usable",
     &loopRdtsc, nullptr},
    {"cpuclock", "the per-frame cpu time read, for stall detection", &loopCpuClock, &needCpuClock},
    {"wrapper", "one level 1 game DLL wrapper, with nothing under it", &loopWrapper, nullptr},
    {"cvar", "pfnCVarGetFloat, which plugins call constantly", &loopCvar, nullptr},
    {"edict", "pfnPEntityOfEntIndex, about the cheapest engine call there is",
     &loopEdictOfIndex, &needMap},
    {"traceline", "pfnTraceLine straight down through the world", &loopTraceLine, &needMap},
};

constexpr int kBenchCount = static_cast<int>(sizeof(g_benches) / sizeof(g_benches[0]));

Result measure(const Bench& bench) {
    Result result;
    result.name = bench.name;
    result.what = bench.what;
    if (bench.unavailable != nullptr) {
        if (const char* why = bench.unavailable()) {
            result.skipped = why;
            return result;
        }
    }

    // Size a batch from a short probe, so a cheap op gets enough iterations to
    // rise above the clock's own resolution and an expensive one does not stall
    // the server.
    const std::uint64_t probeNs = bench.loop(kProbeOps);
    const double perOp = probeNs > 0 ? static_cast<double>(probeNs) / kProbeOps : 1.0;
    int ops = static_cast<int>(kBatchMs * 1e6 / (perOp > 0.0 ? perOp : 1.0));
    ops = std::max(ops, 64);
    ops = std::min(ops, 2000000);

    double perBatch[kBatches];
    for (int i = 0; i < kBatches; ++i) {
        perBatch[i] = static_cast<double>(bench.loop(ops)) / ops;
    }
    std::sort(perBatch, perBatch + kBatches);

    result.nsPerOp = perBatch[kBatches / 2];
    result.ops = static_cast<std::uint64_t>(ops) * kBatches;
    if (result.nsPerOp > 0.0) {
        result.spread = (perBatch[kBatches - 1] - perBatch[0]) / result.nsPerOp;
    }
    return result;
}

} // namespace

std::vector<const char*> names() {
    std::vector<const char*> out;
    for (const Bench& bench : g_benches) out.push_back(bench.name);
    return out;
}

double budgetMs(const std::string& name) {
    const int count = name.empty() ? kBenchCount : 1;
    return count * kBatches * kBatchMs;
}

std::vector<Result> run(const std::string& name) {
    std::vector<Result> results;
    for (const Bench& bench : g_benches) {
        if (!name.empty() && name != bench.name) continue;
        results.push_back(measure(bench));
    }
    return results;
}

} // namespace micro
