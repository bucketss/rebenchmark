#include "phases.h"

#include "clock.h"
#include "rehlds.h"

#include <rebench_probe.h>

#include <algorithm>
#include <cstring>

namespace phases {
namespace {

constexpr int kMaxPhases = 64;

constexpr int kMaxDepth = 32;

IRebenchProbe* g_probe = nullptr;
bool g_looked = false;
const char* g_status = "this engine has no rebench probe";
bool g_active = false;
bool g_settling = false;
int g_count = 0;

struct Stat {
    std::uint64_t calls = 0;
    std::uint64_t selfNs = 0;
    std::uint64_t totalNs = 0;
    std::uint64_t maxTotalNs = 0;
    std::uint64_t runMaxNs = 0;
};
Stat g_stat[kMaxPhases];

struct FrameAcc {
    std::uint32_t calls;
    std::uint64_t selfNs;
    std::uint64_t totalNs;
};
FrameAcc g_frame[kMaxPhases];
bool g_listed[kMaxPhases];
int g_touched[kMaxPhases];
int g_touchedCount = 0;

struct Active {
    int phase;
    std::uint64_t start;
    std::uint64_t childTicks;
};
Active g_stack[kMaxDepth];
int g_depth = 0;
std::uint64_t g_dropped = 0;

void probeLook() {
    if (g_looked) return;
    g_looked = true;

    const CreateInterfaceFn factory = engineFactory();
    if (factory == nullptr) {
        g_status = "engine module or its CreateInterface not found";
        return;
    }
    int code = 0;
    auto* probe = reinterpret_cast<IRebenchProbe*>(factory(VREBENCH_PROBE_VERSION, &code));
    if (probe == nullptr) {
        g_status = "this engine has no rebench probe";
        return;
    }
    if (probe->GetVersion() != 1) {
        g_status = "the engine's rebench probe is a different version";
        return;
    }
    g_probe = probe;
    g_count = probe->GetPhaseCount();
    if (g_count > kMaxPhases) g_count = kMaxPhases;
    g_status = "engine phase marks available";
}

void mark(int phase, int begin) {
    if (phase < 0 || phase >= g_count) {
        ++g_dropped;
        return;
    }

    if (begin != 0) {
        if (g_depth >= kMaxDepth) {
            ++g_dropped;
            return;
        }
        Active& active = g_stack[g_depth++];
        active.phase = phase;
        active.childTicks = 0;
        active.start = clk::now();
        return;
    }

    const std::uint64_t end = clk::now();
    if (g_depth == 0 && g_settling) return;
    if (g_depth == 0 || g_stack[g_depth - 1].phase != phase) {
        ++g_dropped;
        return;
    }

    Active& active = g_stack[--g_depth];
    std::uint64_t total = end > active.start ? end - active.start : 0;
    if (active.childTicks > total) active.childTicks = total;
    const std::uint64_t self = total - active.childTicks;

    if (g_depth > 0) g_stack[g_depth - 1].childTicks += total;

    const std::uint64_t totalNs = clk::toNs(total);
    const std::uint64_t selfNs = clk::toNs(self);

    Stat& stat = g_stat[phase];
    ++stat.calls;
    stat.selfNs += selfNs;
    stat.totalNs += totalNs;
    if (totalNs > stat.maxTotalNs) stat.maxTotalNs = totalNs;
    if (totalNs > stat.runMaxNs) stat.runMaxNs = totalNs;

    FrameAcc& frame = g_frame[phase];
    if (!g_listed[phase]) {
        g_listed[phase] = true;
        g_touched[g_touchedCount++] = phase;
    }
    ++frame.calls;
    frame.selfNs += selfNs;
    frame.totalNs += totalNs;
}

} // namespace

bool available() {
    probeLook();
    return g_probe != nullptr;
}

const char* status() {
    probeLook();
    return g_status;
}

const char* name(int phase) {
    if (g_probe == nullptr || phase < 0 || phase >= g_count) return "";
    const char* text = g_probe->GetPhaseName(phase);
    return text != nullptr ? text : "";
}

int count() {
    return g_count;
}

bool active() {
    return g_active;
}

bool foreign() {
    if (g_probe == nullptr) return false;
    const RebenchProbeMarkFn held = g_probe->GetMarkCallback();
    return held != nullptr && held != &mark;
}

bool setActive(bool on, std::string& error) {
    probeLook();
    if (g_probe == nullptr) {
        error = status();
        return false;
    }
    if (on == g_active) return true;

    const RebenchProbeMarkFn held = g_probe->GetMarkCallback();
    if (on) {
        if (held != nullptr && held != &mark) {
            error = "another module is already reading the engine's phase marks";
            return false;
        }
        g_depth = 0;
        g_settling = true;
        g_probe->SetMarkCallback(&mark);
        g_active = true;
        return true;
    }

    if (held != nullptr && held != &mark) {
        g_active = false;
        error = "the engine's phase marks now belong to another module";
        return false;
    }
    g_probe->SetMarkCallback(nullptr);
    g_active = false;
    g_depth = 0;
    return true;
}

void beginFrame() {
    for (int i = 0; i < g_touchedCount; ++i) {
        const int phase = g_touched[i];
        g_frame[phase] = FrameAcc{};
        g_listed[phase] = false;
    }
    g_touchedCount = 0;
    g_depth = 0;
    g_settling = false;
}

int frameTop(gamedll::SpikePhase* out, int max) {
    if (out == nullptr || max <= 0) return 0;

    int order[kMaxPhases];
    const int touched = g_touchedCount;
    for (int i = 0; i < touched; ++i) order[i] = g_touched[i];
    std::sort(order, order + touched, [](int a, int b) {
        return g_frame[a].selfNs > g_frame[b].selfNs;
    });

    const int kept = touched < max ? touched : max;
    for (int i = 0; i < kept; ++i) {
        const int phase = order[i];
        out[i].phase = phase;
        out[i].calls = g_frame[phase].calls;
        out[i].selfNs = g_frame[phase].selfNs;
        out[i].totalNs = g_frame[phase].totalNs;
    }
    return kept;
}

std::uint64_t dropped() {
    return g_dropped;
}

void resetMax() {
    for (int i = 0; i < kMaxPhases; ++i) g_stat[i].runMaxNs = 0;
}

void snapshot(Stats& out) {
    out.present = g_probe != nullptr;
    out.entries.clear();
    if (g_probe == nullptr) return;
    out.entries.reserve(static_cast<std::size_t>(g_count));
    for (int phase = 0; phase < g_count; ++phase) {
        Entry entry;
        entry.phase = phase;
        entry.name = name(phase);
        entry.calls = g_stat[phase].calls;
        entry.selfNs = g_stat[phase].selfNs;
        entry.totalNs = g_stat[phase].totalNs;
        entry.maxTotalNs = g_stat[phase].runMaxNs;
        out.entries.push_back(std::move(entry));
    }
}

Stats diff(const Stats& from, const Stats& to) {
    Stats out;
    out.present = to.present;
    for (const Entry& late : to.entries) {
        const Entry* early = nullptr;
        for (const Entry& candidate : from.entries) {
            if (candidate.phase == late.phase) {
                early = &candidate;
                break;
            }
        }
        const std::uint64_t calls = early != nullptr && early->calls <= late.calls
            ? late.calls - early->calls
            : late.calls;
        if (calls == 0) continue;

        Entry entry = late;
        entry.calls = calls;
        if (early != nullptr) {
            entry.selfNs = late.selfNs > early->selfNs ? late.selfNs - early->selfNs : 0;
            entry.totalNs = late.totalNs > early->totalNs ? late.totalNs - early->totalNs : 0;
        }
        out.entries.push_back(std::move(entry));
    }
    return out;
}

double overheadNsPerMark() {
    if (g_probe == nullptr || g_count <= 0) return 0.0;

    constexpr int kPhase = REBENCH_PHASE_FRAME;
    constexpr int kIterations = 2000;

    const Stat savedStat = g_stat[kPhase];
    const FrameAcc savedFrame = g_frame[kPhase];
    const bool savedListed = g_listed[kPhase];
    const int savedTouched = g_touchedCount;
    const std::uint64_t savedDropped = g_dropped;
    const int savedDepth = g_depth;
    const std::uint64_t savedParentChild = savedDepth > 0 ? g_stack[savedDepth - 1].childTicks : 0;

    const std::uint64_t start = clk::now();
    for (int i = 0; i < kIterations; ++i) {
        mark(kPhase, 1);
        mark(kPhase, 0);
    }
    const std::uint64_t end = clk::now();

    g_stat[kPhase] = savedStat;
    g_frame[kPhase] = savedFrame;
    g_listed[kPhase] = savedListed;
    g_touchedCount = savedTouched;
    g_dropped = savedDropped;
    g_depth = savedDepth;
    if (savedDepth > 0) g_stack[savedDepth - 1].childTicks = savedParentChild;

    const std::uint64_t ns = end > start ? clk::toNs(end - start) : 0;
    return static_cast<double>(ns) / kIterations;
}

} // namespace phases
