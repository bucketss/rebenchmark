#include "report.h"

#include "clock.h"
#include "sysinfo.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <system_error>

#ifndef REBENCH_VERSION
#define REBENCH_VERSION "dev"
#endif

namespace report {
namespace {

std::string format(const char* fmt, double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), fmt, value);
    return buffer;
}

std::string ms(double ns) {
    return format("%.3f", ns / 1e6);
}

std::tm toTm(std::time_t t, bool utc) {
    std::tm result = {};
#ifdef _WIN32
    if (utc) gmtime_s(&result, &t); else localtime_s(&result, &t);
#else
    if (utc) gmtime_r(&t, &result); else localtime_r(&t, &result);
#endif
    return result;
}

std::string formatTime(std::time_t t, const char* fmt, bool utc) {
    const std::tm parts = toTm(t, utc);
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), fmt, &parts);
    return buffer;
}

std::string fileSafe(const std::string& text) {
    std::string result;
    for (const char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        result += ok ? c : '_';
    }
    return result.empty() ? "_" : result;
}

class Json {
public:
    void beginObject(const char* key = nullptr) { open(key, '{'); }
    void endObject() { close('}'); }
    void beginArray(const char* key = nullptr) { open(key, '['); }
    void endArray() { close(']'); }

    void string(const char* key, const std::string& value) {
        prefix(key);
        quote(value);
    }
    void number(const char* key, double value, int decimals) {
        prefix(key);
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
        out_ += buffer;
    }
    void integer(const char* key, long long value) {
        prefix(key);
        out_ += std::to_string(value);
    }
    void boolean(const char* key, bool value) {
        prefix(key);
        out_ += value ? "true" : "false";
    }
    // An already formatted value.
    void raw(const char* key, const std::string& text) {
        prefix(key);
        out_ += text;
    }

    const std::string& text() const { return out_; }

private:
    void open(const char* key, char bracket) {
        prefix(key);
        out_ += bracket;
        empty_.push_back(true);
    }
    void close(char bracket) {
        const bool empty = empty_.back();
        empty_.pop_back();
        if (!empty) newline();
        out_ += bracket;
    }
    void prefix(const char* key) {
        if (!empty_.empty()) {
            if (!empty_.back()) out_ += ',';
            empty_.back() = false;
            newline();
        }
        if (key != nullptr) {
            quote(key);
            out_ += ": ";
        }
    }
    void newline() {
        out_ += '\n';
        out_.append(empty_.size() * 2, ' ');
    }
    static std::size_t utf8Length(const std::string& value, std::size_t at) {
        const auto lead = static_cast<unsigned char>(value[at]);
        std::size_t length = 0;
        std::uint32_t lowest = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            length = 2;
            lowest = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            length = 3;
            lowest = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            length = 4;
            lowest = 0x10000;
        } else {
            return 0;
        }
        if (at + length > value.size()) return 0;
        std::uint32_t code = lead & (0x7f >> length);
        for (std::size_t i = 1; i < length; ++i) {
            const auto next = static_cast<unsigned char>(value[at + i]);
            if ((next & 0xc0) != 0x80) return 0;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < lowest || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return 0;
        return length;
    }

    void quote(const std::string& value) {
        out_ += '"';
        for (std::size_t at = 0; at < value.size();) {
            const char c = value[at];
            const auto byte = static_cast<unsigned char>(c);
            if (c == '"' || c == '\\') {
                out_ += '\\';
                out_ += c;
                ++at;
            } else if (byte < 0x80) {
                if (byte < 0x20) {
                    char escape[8];
                    std::snprintf(escape, sizeof(escape), "\\u%04x", byte);
                    out_ += escape;
                } else {
                    out_ += c;
                }
                ++at;
            } else if (const std::size_t length = utf8Length(value, at)) {
                out_.append(value, at, length);
                at += length;
            } else {
                char escape[8];
                std::snprintf(escape, sizeof(escape), "\\u%04x", byte);
                out_ += escape;
                ++at;
            }
        }
        out_ += '"';
    }

    std::string out_;
    std::vector<bool> empty_;
};

void msStats(Json& json, const char* key, const Histogram& h) {
    json.beginObject(key);
    json.number("mean", h.mean() / 1e6, 4);
    json.number("stddev", h.stddev() / 1e6, 4);
    json.number("min", static_cast<double>(h.min()) / 1e6, 4);
    json.number("p50", h.percentile(50.0) / 1e6, 4);
    json.number("p90", h.percentile(90.0) / 1e6, 4);
    json.number("p99", h.percentile(99.0) / 1e6, 4);
    json.number("p99_9", h.percentile(99.9) / 1e6, 4);
    json.number("max", static_cast<double>(h.max()) / 1e6, 4);
    json.endObject();
}

void range(Json& json, const char* key, const probe::Range& r, int samples) {
    json.beginObject(key);
    json.integer("min", samples != 0 ? r.min : 0);
    json.number("avg", samples != 0 ? static_cast<double>(r.sum) / samples : 0.0, 2);
    json.integer("max", r.max);
    json.endObject();
}

void buckets(Json& json, const char* key, const Histogram& h) {
    json.beginArray(key);
    for (int i = 0; i < Histogram::kBuckets; ++i) {
        if (h.bucket(i) == 0) continue;
        json.raw(nullptr, "[" + std::to_string(Histogram::bucketLow(i)) + ", " +
            std::to_string(Histogram::bucketHigh(i)) + ", " + std::to_string(h.bucket(i)) + "]");
    }
    json.endArray();
}

double fps(const Histogram& period) {
    return period.sum() != 0
        ? static_cast<double>(period.count()) * 1e9 / static_cast<double>(period.sum())
        : 0.0;
}

double busyPercent(const probe::FrameStats& frames) {
    return frames.period.sum() != 0
        ? 100.0 * static_cast<double>(frames.busy.sum()) / static_cast<double>(frames.period.sum())
        : 0.0;
}

double wallSeconds(const probe::Run& run) {
    return run.stopTicks > run.startTicks ? clk::toSeconds(run.stopTicks - run.startTicks) : 0.0;
}

int overIndex(int ms) {
    for (int i = 0; i < probe::kOverCount; ++i) {
        if (probe::kOverMs[i] == ms) return i;
    }
    return -1;
}

std::string line(const char* fmt, ...) {
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return buffer;
}

gamedll::FuncStat delta(const gamedll::FuncStat& from, const gamedll::FuncStat& to) {
    gamedll::FuncStat result;
    result.calls = to.calls - from.calls;
    result.selfNs = to.selfNs - from.selfNs;
    result.totalNs = to.totalNs - from.totalNs;
    result.maxTotalNs = to.maxTotalNs;   // a high water mark, not a running sum
    return result;
}

// Function indices with any calls in the span, heaviest self time first.
std::vector<int> bySelfTime(const gamedll::Totals& from, const gamedll::Totals& to) {
    std::vector<int> order;
    for (int i = 0; i < gamedll::kFunctions; ++i) {
        if (delta(from.f[i], to.f[i]).calls != 0) order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return delta(from.f[a], to.f[a]).selfNs > delta(from.f[b], to.f[b]).selfNs;
    });
    return order;
}

std::uint64_t sumSelfNs(const gamedll::Totals& from, const gamedll::Totals& to) {
    std::uint64_t total = 0;
    for (int i = 0; i < gamedll::kFunctions; ++i) total += delta(from.f[i], to.f[i]).selfNs;
    return total;
}

std::uint64_t sumCalls(const gamedll::Totals& from, const gamedll::Totals& to) {
    std::uint64_t total = 0;
    for (int i = 0; i < gamedll::kFunctions; ++i) total += delta(from.f[i], to.f[i]).calls;
    return total;
}

// rebench's own cost over a run: each measured per-call price times how often
// it was paid. Hookchain dispatch and the detour's trampoline jump are not
// priced, so this is a floor.
struct Overhead {
    std::uint64_t wrappedCalls = 0;
    double wrapperMs = 0.0;
    std::uint64_t pawnCalls = 0;
    double pawnMs = 0.0;
    std::uint64_t phaseMarks = 0;
    double phaseMs = 0.0;
    std::uint64_t frames = 0;
    double frameMs = 0.0;
    std::uint64_t hookCalls = 0;
    double hooksMs = 0.0;
    double totalMs() const { return wrapperMs + pawnMs + phaseMs + frameMs + hooksMs; }
};

// Clock reads per frame outside any hook counted below: two in the SV_Frame
// hook, two bracketing StartFrame.
constexpr int kReadsPerFrame = 4;

Overhead overhead(const probe::Run& run) {
    Overhead o;
    if (run.gameLevel && run.wrapperNs > 0.0) {
        o.wrappedCalls = sumCalls(run.gameStart, run.gameStop);
        o.wrapperMs = static_cast<double>(o.wrappedCalls) * run.wrapperNs / 1e6;
    }
    if (run.pawnTimingNs > 0.0) {
        o.pawnCalls = run.amxxStop.totalCalls - run.amxxStart.totalCalls;
        o.pawnMs = static_cast<double>(o.pawnCalls) * run.pawnTimingNs / 1e6;
    }
    if (run.phaseMarkNs > 0.0) {
        for (const phases::Entry& entry : phases::diff(run.phasesStart, run.phasesStop).entries) {
            o.phaseMarks += entry.calls;
        }
        o.phaseMs = static_cast<double>(o.phaseMarks) * run.phaseMarkNs / 1e6;
    }
    o.frames = run.frames.period.count();
    o.frameMs = static_cast<double>(o.frames) *
        (kReadsPerFrame * run.clockReadNs + (run.frames.cpu.count() != 0 ? run.cpuReadNs : 0.0)) / 1e6;
    const engine::Stats& es = run.engineStart;
    const engine::Stats& ee = run.engineStop;
    o.hookCalls = (ee.deltaCalls - es.deltaCalls) + (ee.edictAllocs - es.edictAllocs) +
        (ee.edictFrees - es.edictFrees) +
        (run.regameStop.thinkCalls - run.regameStart.thinkCalls) +
        (run.regameStop.restarts - run.regameStart.restarts);
    o.hooksMs = static_cast<double>(o.hookCalls) * 2.0 * run.clockReadNs / 1e6;
    return o;
}

// A per-second value, or null when nothing was measuring it.
std::string nullable(bool present, double value, int decimals) {
    if (!present) return "null";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

} // namespace

std::vector<std::string> summarize(const probe::FrameStats& frames, float sysTicrate) {
    const Histogram& period = frames.period;
    const Histogram& busy = frames.busy;
    if (period.count() == 0) return {"no frames recorded"};

    std::vector<std::string> lines;
    lines.push_back("frames " + std::to_string(period.count()) + ", " +
        format("%.1f", fps(period)) + " fps (sys_ticrate " + format("%g", sysTicrate) +
        "), inside SV_Frame " + format("%.1f", busyPercent(frames)) + "% of the time");
    lines.push_back("period ms  p50 " + ms(period.percentile(50.0)) +
        "  p99 " + ms(period.percentile(99.0)) + "  p99.9 " + ms(period.percentile(99.9)) +
        "  max " + ms(static_cast<double>(period.max())) + "  sd " + ms(period.stddev()));
    lines.push_back("busy   ms  p50 " + ms(busy.percentile(50.0)) +
        "  p99 " + ms(busy.percentile(99.0)) + "  p99.9 " + ms(busy.percentile(99.9)) +
        "  max " + ms(static_cast<double>(busy.max())) + "  sd " + ms(busy.stddev()));

    if (frames.cpu.count() != 0) {
        // Against the period, not busy. The counter is read once a frame, so it
        // spans frame start to frame start and takes in whatever the engine
        // burns between frames as well as SV_Frame itself -- at a high
        // sys_ticrate that gap is spun through rather than slept, which is why
        // this can be several times the busy figure. Over the period it is a
        // duty cycle, bounded by 100% for one thread and needing no clamp.
        const double onCpu = frames.period.sum() != 0
            ? 100.0 * static_cast<double>(frames.cpu.sum()) /
                static_cast<double>(frames.period.sum())
            : 0.0;
        std::string line = "cpu    ms  p50 " + ms(frames.cpu.percentile(50.0)) +
            "  p99 " + ms(frames.cpu.percentile(99.0)) +
            "  max " + ms(static_cast<double>(frames.cpu.max())) +
            "   on cpu " + format("%.1f", onCpu) + "% of wall";
        if (frames.stalled != 0) {
            line += ", " + std::to_string(frames.stalled) + " stalled";
        }
        lines.push_back(line);
    }

    if (frames.game.count() != 0) {
        lines.push_back("game   ms  p50 " + ms(frames.game.percentile(50.0)) +
            "  p99 " + ms(frames.game.percentile(99.0)) +
            "  max " + ms(static_cast<double>(frames.game.max())) +
            "   engine ms  p50 " + ms(frames.engine.percentile(50.0)) +
            "  p99 " + ms(frames.engine.percentile(99.0)) +
            "  max " + ms(static_cast<double>(frames.engine.max())));
    }

    if (frames.delta.count() != 0) {
        lines.push_back("delta  ms  p50 " + ms(frames.delta.percentile(50.0)) +
            "  p99 " + ms(frames.delta.percentile(99.0)) +
            "  max " + ms(static_cast<double>(frames.delta.max())) +
            "   entity delta in " + std::to_string(frames.delta.count()) + " of " +
            std::to_string(period.count()) + " frames");
    }

    if (frames.amxxFrame.count() != 0) {
        lines.push_back("pawn   ms  p50 " + ms(frames.amxxFrame.percentile(50.0)) +
            "  p99 " + ms(frames.amxxFrame.percentile(99.0)) +
            "  max " + ms(static_cast<double>(frames.amxxFrame.max())) +
            "   part of game, not extra");
    }

    std::string over;
    if (sysTicrate > 0.0f) {
        over = "over 2 ticks " + std::to_string(frames.overTwoTicks) + ", ";
    }
    over += "over 10 ms " + std::to_string(frames.overMs[overIndex(10)]) +
        ", over 50 ms " + std::to_string(frames.overMs[overIndex(50)]);
    lines.push_back(over);
    return lines;
}

std::vector<std::string> functionTable(const gamedll::Totals& from, const gamedll::Totals& to,
    double seconds, int rows) {
    const std::vector<int> order = bySelfTime(from, to);
    if (order.empty()) return {"no game DLL calls counted"};

    const double selfMs = static_cast<double>(sumSelfNs(from, to)) / 1e6;
    std::vector<std::string> lines;
    lines.push_back(line("%-22s %10s %9s %9s %9s %8s", "function", "calls", "self ms",
        "total ms", "us/call", "max ms"));
    for (std::size_t i = 0; i < order.size() && static_cast<int>(i) < rows; ++i) {
        const gamedll::FuncStat stat = delta(from.f[order[i]], to.f[order[i]]);
        lines.push_back(line("%-22s %10llu %9.1f %9.1f %9.2f %8.3f",
            gamedll::name(order[i]),
            static_cast<unsigned long long>(stat.calls),
            static_cast<double>(stat.selfNs) / 1e6,
            static_cast<double>(stat.totalNs) / 1e6,
            stat.calls != 0 ? static_cast<double>(stat.selfNs) / stat.calls / 1e3 : 0.0,
            static_cast<double>(stat.maxTotalNs) / 1e6));
    }
    if (static_cast<int>(order.size()) > rows) {
        lines.push_back(line("%d more functions not shown", static_cast<int>(order.size()) - rows));
    }
    lines.push_back(line("total self %.1f ms over %.1f s, %.2f%% of wall time",
        selfMs, seconds, seconds > 0.0 ? selfMs / (seconds * 10.0) : 0.0));
    if (const std::uint64_t overflows = to.overflows - from.overflows) {
        lines.push_back(line("%llu calls missed: nesting deeper than the stack",
            static_cast<unsigned long long>(overflows)));
    }
    return lines;
}

std::vector<std::string> engineTable(const engine::Stats& from, const engine::Stats& to) {
    const std::uint64_t allocs = to.edictAllocs - from.edictAllocs;
    const std::uint64_t frees = to.edictFrees - from.edictFrees;
    const std::uint64_t deltas = to.deltaCalls - from.deltaCalls;
    if (allocs == 0 && frees == 0 && deltas == 0) return {};

    std::vector<std::string> lines;
    if (deltas != 0) {
        const std::uint64_t ns = to.deltaNs - from.deltaNs;
        lines.push_back(line("entity delta %llu calls, %.1f ms, %.2f us/call, worst %.3f ms, "
            "%.0f KB written",
            static_cast<unsigned long long>(deltas), static_cast<double>(ns) / 1e6,
            static_cast<double>(ns) / deltas / 1e3,
            static_cast<double>(to.maxDeltaNs) / 1e6,
            static_cast<double>(to.deltaBytes - from.deltaBytes) / 1024.0));
    }
    if (allocs != 0 || frees != 0) {
        const std::uint64_t allocNs = to.edictAllocNs - from.edictAllocNs;
        const std::uint64_t freeNs = to.edictFreeNs - from.edictFreeNs;
        lines.push_back(line("edicts %llu allocs %.2f us/call worst %.3f ms, "
            "%llu frees %.2f us/call worst %.3f ms",
            static_cast<unsigned long long>(allocs),
            allocs != 0 ? static_cast<double>(allocNs) / allocs / 1e3 : 0.0,
            static_cast<double>(to.maxEdictAllocNs) / 1e6,
            static_cast<unsigned long long>(frees),
            frees != 0 ? static_cast<double>(freeNs) / frees / 1e3 : 0.0,
            static_cast<double>(to.maxEdictFreeNs) / 1e6));
        // ED_Alloc scans the edict array for a slot it may reuse, so its cost
        // rises with entity count and churn rather than being flat.
        if (const std::uint64_t failures = to.edictAllocFailures - from.edictAllocFailures) {
            lines.push_back(line("  %llu allocation(s) FAILED: the server ran out of edicts",
                static_cast<unsigned long long>(failures)));
        }
    }
    return lines;
}

std::vector<std::string> amxxTable(const amxx::Stats& stats, double seconds, int rows) {
    if (!stats.present) return {};
    if (stats.entries.empty() && stats.totalCalls == 0) return {};

    const double totalMs = static_cast<double>(stats.totalNs) / 1e6;
    std::vector<std::string> lines;
    // Truncated rather than allowed to run over: a long plugin name would push
    // every column right and only on the rows that have one.
    lines.push_back(line("%-24s %-20s %9s %9s %9s %8s", "plugin", "function", "calls",
        "self ms", "total ms", "us/call"));
    for (std::size_t i = 0; i < stats.entries.size() && static_cast<int>(i) < rows; ++i) {
        const amxx::Entry& entry = stats.entries[i];
        lines.push_back(line("%-24.24s %-20.20s %9llu %9.1f %9.1f %8.2f",
            entry.plugin.c_str(), entry.function.c_str(),
            static_cast<unsigned long long>(entry.calls),
            static_cast<double>(entry.selfNs) / 1e6,
            static_cast<double>(entry.totalNs) / 1e6,
            entry.calls != 0 ? static_cast<double>(entry.selfNs) / entry.calls / 1e3 : 0.0));
    }
    if (static_cast<int>(stats.entries.size()) > rows) {
        lines.push_back(line("%d more not shown",
            static_cast<int>(stats.entries.size()) - rows));
    }
    // Pawn runs inside the game DLL calls level 1 already times, so this is a
    // share of that time rather than something to add to it.
    lines.push_back(line("total %.1f ms over %llu calls, %.2f%% of wall time, inside the game "
        "DLL table not beside it",
        totalMs, static_cast<unsigned long long>(stats.totalCalls),
        seconds > 0.0 ? totalMs / (seconds * 10.0) : 0.0));
    if (stats.unattributed != 0) {
        lines.push_back(line("  %llu call(s) counted but not attributed to a plugin",
            static_cast<unsigned long long>(stats.unattributed)));
    }
    return lines;
}

std::vector<std::string> phasesTable(const phases::Stats& stats, double seconds, int rows) {
    if (!stats.present || stats.entries.empty()) return {};

    std::vector<const phases::Entry*> order;
    order.reserve(stats.entries.size());
    for (const phases::Entry& entry : stats.entries) {
        if (entry.calls != 0) order.push_back(&entry);
    }
    if (order.empty()) return {};
    std::sort(order.begin(), order.end(),
        [](const phases::Entry* a, const phases::Entry* b) { return a->selfNs > b->selfNs; });

    std::vector<std::string> lines;
    lines.push_back(line("%-22s %10s %9s %9s %8s %8s", "phase", "calls", "self ms", "total ms",
        "us/call", "max ms"));
    double selfMs = 0.0;
    for (const phases::Entry* entry : order) selfMs += static_cast<double>(entry->selfNs) / 1e6;
    for (std::size_t i = 0; i < order.size() && static_cast<int>(i) < rows; ++i) {
        const phases::Entry& entry = *order[i];
        lines.push_back(line("%-22.22s %10llu %9.1f %9.1f %8.2f %8.3f", entry.name.c_str(),
            static_cast<unsigned long long>(entry.calls),
            static_cast<double>(entry.selfNs) / 1e6,
            static_cast<double>(entry.totalNs) / 1e6,
            entry.calls != 0 ? static_cast<double>(entry.selfNs) / entry.calls / 1e3 : 0.0,
            static_cast<double>(entry.maxTotalNs) / 1e6));
    }
    if (static_cast<int>(order.size()) > rows) {
        lines.push_back(line("%d more not shown", static_cast<int>(order.size()) - rows));
    }
    lines.push_back(line("self %.1f ms over the run, %.2f%% of wall time", selfMs,
        seconds > 0.0 ? selfMs / (seconds * 10.0) : 0.0));
    return lines;
}

std::vector<std::string> startFrameBreakdown(const probe::Run& run) {
    const gamedll::FuncStat frame =
        delta(run.gameStart.f[gamedll::k_pfnStartFrame], run.gameStop.f[gamedll::k_pfnStartFrame]);
    if (frame.calls == 0) return {};

    const double totalMs = static_cast<double>(frame.totalNs) / 1e6;
    const double innerMs = static_cast<double>(run.innerStop.ns - run.innerStart.ns) / 1e6;
    const std::uint64_t innerCalls = run.innerStop.calls - run.innerStart.calls;

    std::vector<std::string> lines;
    lines.push_back(line("StartFrame %.1f ms over %llu calls, everything the engine asks of "
        "the game each frame", totalMs, static_cast<unsigned long long>(frame.calls)));
    if (innerCalls != 0) {
        const double pluginsMs = totalMs > innerMs ? totalMs - innerMs : 0.0;
        lines.push_back(line("  game DLL %.1f ms (%.0f%%), other Metamod plugins %.1f ms (%.0f%%)",
            innerMs, totalMs > 0.0 ? 100.0 * innerMs / totalMs : 0.0,
            pluginsMs, totalMs > 0.0 ? 100.0 * pluginsMs / totalMs : 0.0));
        lines.push_back("  (the split needs rebench last in plugins.ini; otherwise the plugins "
            "loaded after it count as game DLL)");
    }

    const regame::Stats& from = run.regameStart;
    const regame::Stats& to = run.regameStop;
    const std::uint64_t thinkCalls = to.thinkCalls - from.thinkCalls;
    if (thinkCalls == 0) {
        lines.push_back("  no ReGameDLL API, so the game DLL's own time is not broken down");
        return lines;
    }
    const double thinkMs = static_cast<double>(to.thinkNs - from.thinkNs) / 1e6;
    lines.push_back(line("  of the game DLL, CSGameRules::Think %.1f ms (%.0f%% of StartFrame), "
        "worst call %.2f ms", thinkMs, totalMs > 0.0 ? 100.0 * thinkMs / totalMs : 0.0,
        static_cast<double>(to.maxThinkNs) / 1e6));
    const std::uint64_t restarts = to.restarts - from.restarts;
    lines.push_back(line("    of which %llu round restarts, %.1f ms, worst %.2f ms",
        static_cast<unsigned long long>(restarts),
        static_cast<double>(to.restartNs - from.restartNs) / 1e6,
        static_cast<double>(to.maxRestartNs) / 1e6));
    lines.push_back("  the rest of the game DLL's StartFrame is the bot manager and local "
        "navigation, which have no hookchain");
    return lines;
}

std::vector<std::string> spikeTable(int rows) {
    const int count = gamedll::spikeCount();
    if (count == 0) return {"no spikes recorded"};

    // Held worst first, so take the worst rows and then read them oldest first.
    const int shown = count < rows ? count : rows;
    std::vector<int> order(shown);
    for (int i = 0; i < shown; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [](int a, int b) {
        return gamedll::spike(a).atSeconds < gamedll::spike(b).atSeconds;
    });

    std::vector<std::string> lines;
    for (int n = 0; n < shown; ++n) {
        const gamedll::Spike& spike = gamedll::spike(order[n]);
        std::string head = line("%8.1fs  %s  %d+%d  period %.2f  busy %.2f  game %.2f ms",
            spike.atSeconds, spike.map, spike.humans, spike.bots,
            static_cast<double>(spike.periodNs) / 1e6,
            static_cast<double>(spike.busyNs) / 1e6,
            static_cast<double>(spike.gamedllNs) / 1e6);
        if (spike.hasCpu) {
            head += line("  cpu %.2f", static_cast<double>(spike.cpuNs) / 1e6);
            if (probe::isStall(spike.busyNs, spike.cpuNs)) head += " STALLED";
        }
        const gamedll::FrameExtra& extra = spike.extra;
        if (extra.hasAmxx) head += line("  pawn %.2f", static_cast<double>(extra.amxxNs) / 1e6);
        if (extra.deltaCalls != 0) {
            head += line("  delta %.2f(%d)", static_cast<double>(extra.deltaNs) / 1e6, extra.deltaCalls);
        }
        if (extra.edictChurn != 0) head += line("  edicts +-%d", extra.edictChurn);
        lines.push_back(head);
        std::string top;
        for (int i = 0; i < spike.entries; ++i) {
            const gamedll::SpikeEntry& entry = spike.top[i];
            if (i != 0) top += "  ";
            top += line("%s %.2f(%u)", gamedll::name(entry.function),
                static_cast<double>(entry.selfNs) / 1e6, entry.calls);
        }
        if (!top.empty()) lines.push_back("           " + top);
        std::string pawn;
        for (int i = 0; i < extra.pawnEntries; ++i) {
            const gamedll::SpikePawn& entry = extra.pawn[i];
            if (i != 0) pawn += "  ";
            pawn += line("%s:%s %.2f(%u)", entry.plugin, entry.function,
                static_cast<double>(entry.selfNs) / 1e6, entry.calls);
        }
        if (!pawn.empty()) lines.push_back("           " + pawn);
        std::string phaseLine;
        for (int i = 0; i < extra.phaseEntries; ++i) {
            const gamedll::SpikePhase& entry = extra.phase[i];
            if (i != 0) phaseLine += "  ";
            phaseLine += line("%s %.2f(%u)", phases::name(entry.phase),
                static_cast<double>(entry.selfNs) / 1e6, entry.calls);
        }
        if (!phaseLine.empty()) lines.push_back("           " + phaseLine);
    }
    if (count > shown) lines.push_back(line("%d lesser spikes not shown", count - shown));
    return lines;
}

std::vector<std::string> mapChangeTable(const probe::Run& run) {
    std::vector<std::string> lines;
    for (const probe::MapChange& change : run.mapChanges) {
        std::string text = line("map change at %.1fs  %s -> %s  load %.1f ms", change.atSeconds,
            change.from.c_str(), change.to.c_str(), static_cast<double>(change.loadNs) / 1e6);
        if (change.hasGame) text += line("  game %.1f", static_cast<double>(change.gamedllNs) / 1e6);
        if (change.hasAmxx) text += line("  pawn %.1f", static_cast<double>(change.amxxNs) / 1e6);
        text += line("  edict allocs %llu", static_cast<unsigned long long>(change.edictAllocs));
        lines.push_back(text);
    }
    return lines;
}

std::vector<std::string> instrumentationLines(const probe::Run& run) {
    const Overhead o = overhead(run);
    const double seconds = wallSeconds(run);
    return {line("rebench overhead ~%.1f ms (%.3f%% of wall): wrappers %.1f, pawn timing %.1f, "
        "phase marks %.1f, frame hooks %.1f, engine hooks %.1f", o.totalMs(),
        seconds > 0.0 ? o.totalMs() / (seconds * 10.0) : 0.0,
        o.wrapperMs, o.pawnMs, o.phaseMs, o.frameMs, o.hooksMs)};
}

std::filesystem::path write(const probe::Run& run, const Server& server,
    const std::filesystem::path& dir, std::string& error) {
    const Histogram& period = run.frames.period;
    const Histogram& busy = run.frames.busy;
    const sys::Cpu& cpu = sys::cpu();

    Json json;
    json.beginObject();
    json.string("rebench", REBENCH_VERSION);
    json.string("tag", run.tag);
    json.string("end_reason", run.endReason);
    json.string("started", formatTime(run.startedAt, "%Y-%m-%dT%H:%M:%SZ", true));
    json.string("stopped", formatTime(run.stoppedAt, "%Y-%m-%dT%H:%M:%SZ", true));
    const double wall = wallSeconds(run);
    json.number("duration_s", wall, 3);
    json.number("measured_s", static_cast<double>(period.sum()) / 1e9, 3);
    json.beginArray("maps");
    for (const std::string& map : run.maps) json.string(nullptr, map);
    json.endArray();

    json.beginObject("server");
    json.string("hostname", server.hostname);
    json.integer("maxplayers", server.maxPlayers);
    json.number("sys_ticrate", server.sysTicrate, 1);
    json.number("fps_max", server.fpsMax, 1);
    json.string("pingboost", server.pingboost);
    json.string("rehlds_api", server.rehldsApi);
    json.integer("rehlds_build", server.rehldsBuild);
    json.beginObject("cvars");
    for (const auto& cvar : server.cvars) json.string(cvar.first.c_str(), cvar.second);
    json.endObject();
    json.beginArray("binaries");
    for (const std::string& binary : server.binaries) json.string(nullptr, binary);
    json.endArray();
    json.beginArray("amxx_plugins");
    for (const std::string& plugin : server.plugins) json.string(nullptr, plugin);
    json.endArray();
    json.endObject();

    json.beginObject("settings");
    json.string("level", server.levelMode);
    json.string("amxx", server.amxxMode);
    json.string("engine", server.phasesMode);
    json.boolean("level_active", run.levelActive);
    json.boolean("amxx_active", run.amxxActive);
    json.boolean("engine_active", run.phasesActive);
    json.boolean("stalls", server.stalls);
    json.number("spike_floor_ms", server.spikeFloorMs, 3);
    json.endObject();

    json.beginObject("host");
    json.string("os", sys::osName());
    json.string("cpu", cpu.brand);
    json.boolean("hypervisor", cpu.hypervisor);
    json.boolean("invariant_tsc", cpu.invariantTsc);
    json.string("clock", clk::sourceName());
    json.number("clock_hz", clk::g_info.hz, 0);
    json.number("clock_ns_per_read", clk::g_info.nsPerRead, 1);
    // How far a forced TSC drifted from the OS clock over the run. Without it
    // a report from an uncorrected counter is indistinguishable from one taken
    // on a clock the CPU vouches for.
    if (clk::g_info.tscForced || run.clockStop.reverted) {
        json.integer("clock_check_samples",
            static_cast<long long>(run.clockStop.checks - run.clockStart.checks));
        json.integer("clock_check_divergences",
            static_cast<long long>(run.clockStop.divergences - run.clockStart.divergences));
        json.number("clock_check_worst_ms", static_cast<double>(run.clockWorstGapNs) / 1e6, 4);
        json.boolean("clock_reverted", run.clockStop.reverted);
        json.boolean("clock_reverted_during_run", run.clockRevertedInRun);
    }
    const std::string clocksource = sys::clocksource();
    if (!clocksource.empty()) json.string("clocksource", clocksource);
    {
        const sys::Contention& from = run.contentionStart;
        const sys::Contention& to = run.contentionStop;
        if (from.available && to.available) {
            const std::uint64_t total = to.totalTicks - from.totalTicks;
            json.number("steal_pct", total != 0
                ? 100.0 * static_cast<double>(to.stealTicks - from.stealTicks) /
                    static_cast<double>(total)
                : 0.0, 3);
            json.integer("voluntary_switches",
                static_cast<long long>(to.voluntarySwitches - from.voluntarySwitches));
            json.integer("involuntary_switches",
                static_cast<long long>(to.involuntarySwitches - from.involuntarySwitches));
        }
        if (run.loadAverage >= 0.0) json.number("load_average_1m", run.loadAverage, 2);
    }
    json.endObject();

    json.beginObject("players");
    json.integer("samples", run.playerSamples);
    range(json, "humans", run.humans, run.playerSamples);
    range(json, "bots", run.bots, run.playerSamples);
    range(json, "hltv", run.hltv, run.playerSamples);
    range(json, "edicts", run.edicts, run.playerSamples);
    json.integer("edicts_limit", run.edictLimit);
    json.endObject();

    {
        const engine::Stats& from = run.engineStart;
        const engine::Stats& to = run.engineStop;
        const std::uint64_t deltaCalls = to.deltaCalls - from.deltaCalls;
        const std::uint64_t deltaNs = to.deltaNs - from.deltaNs;
        json.beginObject("engine");
        json.integer("delta_calls", static_cast<long long>(deltaCalls));
        json.number("delta_ms", static_cast<double>(deltaNs) / 1e6, 3);
        json.number("delta_us_per_call",
            deltaCalls != 0 ? static_cast<double>(deltaNs) / deltaCalls / 1e3 : 0.0, 3);
        json.number("delta_max_ms", static_cast<double>(to.maxDeltaNs) / 1e6, 4);
        json.integer("delta_bytes",
            static_cast<long long>(to.deltaBytes - from.deltaBytes));
        const std::uint64_t allocs = to.edictAllocs - from.edictAllocs;
        const std::uint64_t allocNs = to.edictAllocNs - from.edictAllocNs;
        const std::uint64_t frees = to.edictFrees - from.edictFrees;
        const std::uint64_t freeNs = to.edictFreeNs - from.edictFreeNs;
        json.integer("edict_allocs", static_cast<long long>(allocs));
        json.number("edict_alloc_ms", static_cast<double>(allocNs) / 1e6, 3);
        json.number("edict_alloc_us_per_call",
            allocs != 0 ? static_cast<double>(allocNs) / allocs / 1e3 : 0.0, 3);
        json.number("edict_alloc_max_ms", static_cast<double>(to.maxEdictAllocNs) / 1e6, 4);
        json.integer("edict_frees", static_cast<long long>(frees));
        json.number("edict_free_ms", static_cast<double>(freeNs) / 1e6, 3);
        json.number("edict_free_us_per_call",
            frees != 0 ? static_cast<double>(freeNs) / frees / 1e3 : 0.0, 3);
        json.number("edict_free_max_ms", static_cast<double>(to.maxEdictFreeNs) / 1e6, 4);
        json.integer("edict_alloc_failures",
            static_cast<long long>(to.edictAllocFailures - from.edictAllocFailures));
        json.endObject();
    }

    // Emitted only when timing was actually on. A zeroed section would read as
    // "the plugins cost nothing" rather than "nobody measured".
    const amxx::Stats amxxRun = amxx::diff(run.amxxStart, run.amxxStop);
    if (run.amxxStop.present && amxxRun.totalCalls != 0) {
        const amxx::Stats& stats = amxxRun;
        const double seconds = wall;
        const double totalMs = static_cast<double>(stats.totalNs) / 1e6;
        json.beginObject("amxx");
        json.number("total_ms", totalMs, 3);
        // A share of the game DLL time, not an addition to it: plugins run
        // inside the calls the level 1 wrappers time.
        json.number("total_pct", seconds > 0.0 ? totalMs / (seconds * 10.0) : 0.0, 3);
        json.integer("calls", static_cast<long long>(stats.totalCalls));
        json.integer("unattributed", static_cast<long long>(stats.unattributed));
        if (run.frames.amxxFrame.count() != 0) {
            json.number("frame_p50_ms", run.frames.amxxFrame.percentile(50.0) / 1e6, 4);
            json.number("frame_p99_ms", run.frames.amxxFrame.percentile(99.0) / 1e6, 4);
            json.number("frame_max_ms",
                static_cast<double>(run.frames.amxxFrame.max()) / 1e6, 4);
        }
        json.beginArray("functions");
        for (const amxx::Entry& entry : stats.entries) {
            json.beginObject();
            json.string("plugin", entry.plugin);
            json.string("function", entry.function);
            json.integer("calls", static_cast<long long>(entry.calls));
            json.number("self_ms", static_cast<double>(entry.selfNs) / 1e6, 3);
            json.number("total_ms", static_cast<double>(entry.totalNs) / 1e6, 3);
            json.number("us_per_call",
                entry.calls != 0 ? static_cast<double>(entry.selfNs) / entry.calls / 1e3 : 0.0, 3);
            json.number("max_ms", static_cast<double>(entry.maxNs) / 1e6, 4);
            json.endObject();
        }
        json.endArray();
        json.endObject();
    }

    const phases::Stats phasesRun = phases::diff(run.phasesStart, run.phasesStop);
    if (run.phasesActive && !phasesRun.entries.empty()) {
        json.beginObject("phases");
        json.integer("dropped", static_cast<long long>(run.phasesDropped));
        json.beginArray("list");
        for (const phases::Entry& entry : phasesRun.entries) {
            if (entry.calls == 0) continue;
            json.beginObject();
            json.string("phase", entry.name);
            json.integer("calls", static_cast<long long>(entry.calls));
            json.number("self_ms", static_cast<double>(entry.selfNs) / 1e6, 3);
            json.number("total_ms", static_cast<double>(entry.totalNs) / 1e6, 3);
            json.number("us_per_call",
                entry.calls != 0 ? static_cast<double>(entry.selfNs) / entry.calls / 1e3 : 0.0, 3);
            json.number("max_ms", static_cast<double>(entry.maxTotalNs) / 1e6, 4);
            json.endObject();
        }
        json.endArray();
        json.endObject();
    }

    json.beginObject("frames");
    json.integer("count", static_cast<long long>(period.count()));
    json.number("fps", fps(period), 2);
    json.number("busy_pct", busyPercent(run.frames), 3);
    msStats(json, "period_ms", period);
    msStats(json, "busy_ms", busy);
    if (run.frames.game.count() != 0) {
        msStats(json, "game_ms", run.frames.game);
        msStats(json, "engine_ms", run.frames.engine);
    }
    if (run.frames.cpu.count() != 0) {
        msStats(json, "cpu_ms", run.frames.cpu);
        // Share of wall time the thread was on a CPU; see summarize.
        json.number("cpu_pct_of_wall", period.sum() != 0
            ? 100.0 * static_cast<double>(run.frames.cpu.sum()) /
                static_cast<double>(period.sum())
            : 0.0, 3);
        json.integer("stalled", static_cast<long long>(run.frames.stalled));
    }
    if (run.frames.delta.count() != 0) msStats(json, "delta_ms", run.frames.delta);
    if (run.frames.amxxFrame.count() != 0) msStats(json, "amxx_ms", run.frames.amxxFrame);
    json.integer("edict_churn", static_cast<long long>(run.frames.edictChurn));
    json.beginObject("over");
    json.boolean("exact", true);
    json.integer("2_ticks", static_cast<long long>(run.frames.overTwoTicks));
    for (int i = 0; i < probe::kOverCount; ++i) {
        json.integer((std::to_string(probe::kOverMs[i]) + "ms").c_str(),
            static_cast<long long>(run.frames.overMs[i]));
    }
    json.endObject();
    json.beginObject("worst");
    json.number("period_ms", static_cast<double>(run.worst.periodNs) / 1e6, 4);
    json.number("busy_ms", static_cast<double>(run.worst.busyNs) / 1e6, 4);
    if (run.frames.game.count() != 0) {
        json.number("game_ms", static_cast<double>(run.worst.gameNs) / 1e6, 4);
    }
    if (run.worst.hasCpu) {
        json.number("cpu_ms", static_cast<double>(run.worst.cpuNs) / 1e6, 4);
        json.boolean("stalled", probe::isStall(run.worst.busyNs, run.worst.cpuNs));
    }
    if (run.worst.hasAmxx) json.number("amxx_ms", static_cast<double>(run.worst.amxxNs) / 1e6, 4);
    json.number("delta_ms", static_cast<double>(run.worst.deltaNs) / 1e6, 4);
    json.integer("edict_churn", run.worst.edictChurn);
    json.number("at_s", run.worst.atSeconds, 3);
    json.string("map", run.worst.map);
    json.number("map_time", run.worst.mapTime, 3);
    json.integer("humans", run.worst.players.humans);
    json.integer("bots", run.worst.players.bots);
    json.integer("hltv", run.worst.players.hltv);
    json.endObject();
    json.endObject();

    if (run.gameLevel) {
        const double seconds = wall;
        const std::uint64_t selfNs = sumSelfNs(run.gameStart, run.gameStop);
        json.beginObject("gamedll");
        json.number("self_ms", static_cast<double>(selfNs) / 1e6, 3);
        json.number("self_pct", seconds > 0.0
            ? 100.0 * static_cast<double>(selfNs) / (seconds * 1e9) : 0.0, 3);
        json.integer("depth_overflows",
            static_cast<long long>(run.gameStop.overflows - run.gameStart.overflows));
        const std::uint64_t innerNs = run.innerStop.ns - run.innerStart.ns;
        const std::uint64_t innerCalls = run.innerStop.calls - run.innerStart.calls;
        json.number("startframe_inner_ms", static_cast<double>(innerNs) / 1e6, 3);
        json.integer("startframe_inner_calls", static_cast<long long>(innerCalls));
        if (innerCalls != 0) {
            // What the other Metamod plugins spent around the game DLL's own
            // StartFrame; needs rebench last in plugins.ini.
            const gamedll::FuncStat frame = delta(run.gameStart.f[gamedll::k_pfnStartFrame],
                run.gameStop.f[gamedll::k_pfnStartFrame]);
            json.number("startframe_plugins_ms", frame.totalNs > innerNs
                ? static_cast<double>(frame.totalNs - innerNs) / 1e6 : 0.0, 3);
        }
        if (run.regameStop.thinkCalls != run.regameStart.thinkCalls) {
            json.beginObject("regamedll");
            json.string("version", regame::version());
            json.integer("gamerules_think_calls",
                static_cast<long long>(run.regameStop.thinkCalls - run.regameStart.thinkCalls));
            json.number("gamerules_think_ms",
                static_cast<double>(run.regameStop.thinkNs - run.regameStart.thinkNs) / 1e6, 3);
            json.number("gamerules_think_max_ms",
                static_cast<double>(run.regameStop.maxThinkNs) / 1e6, 4);
            json.integer("round_restarts",
                static_cast<long long>(run.regameStop.restarts - run.regameStart.restarts));
            json.number("round_restart_ms",
                static_cast<double>(run.regameStop.restartNs - run.regameStart.restartNs) / 1e6, 3);
            json.number("round_restart_max_ms",
                static_cast<double>(run.regameStop.maxRestartNs) / 1e6, 4);
            json.endObject();
        }
        json.beginArray("functions");
        for (const int function : bySelfTime(run.gameStart, run.gameStop)) {
            const gamedll::FuncStat stat = delta(run.gameStart.f[function], run.gameStop.f[function]);
            json.beginObject();
            json.string("name", gamedll::name(function));
            json.integer("calls", static_cast<long long>(stat.calls));
            json.number("self_ms", static_cast<double>(stat.selfNs) / 1e6, 3);
            json.number("total_ms", static_cast<double>(stat.totalNs) / 1e6, 3);
            json.number("self_us_per_call",
                stat.calls != 0 ? static_cast<double>(stat.selfNs) / stat.calls / 1e3 : 0.0, 3);
            json.number("max_total_ms", static_cast<double>(stat.maxTotalNs) / 1e6, 4);
            json.endObject();
        }
        json.endArray();
        json.endObject();

        json.beginArray("spikes");
        std::vector<int> order(gamedll::spikeCount());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
        std::sort(order.begin(), order.end(), [](int a, int b) {
            return gamedll::spike(a).atSeconds < gamedll::spike(b).atSeconds;
        });
        for (const int index : order) {
            const gamedll::Spike& spike = gamedll::spike(index);
            json.beginObject();
            json.number("at_s", spike.atSeconds, 3);
            json.string("map", spike.map);
            json.number("map_time", spike.mapTime, 3);
            json.integer("humans", spike.humans);
            json.integer("bots", spike.bots);
            json.number("period_ms", static_cast<double>(spike.periodNs) / 1e6, 4);
            json.number("busy_ms", static_cast<double>(spike.busyNs) / 1e6, 4);
            json.number("game_ms", static_cast<double>(spike.gamedllNs) / 1e6, 4);
            if (spike.hasCpu) {
                json.number("cpu_ms", static_cast<double>(spike.cpuNs) / 1e6, 4);
                json.boolean("stalled", probe::isStall(spike.busyNs, spike.cpuNs));
            }
            const gamedll::FrameExtra& extra = spike.extra;
            json.integer("hltv", extra.hltv);
            if (extra.hasAmxx) json.number("amxx_ms", static_cast<double>(extra.amxxNs) / 1e6, 4);
            json.number("delta_ms", static_cast<double>(extra.deltaNs) / 1e6, 4);
            json.integer("delta_calls", extra.deltaCalls);
            json.integer("edict_churn", extra.edictChurn);
            if (extra.hasAmxx) {
                json.beginArray("pawn");
                for (int i = 0; i < extra.pawnEntries; ++i) {
                    const gamedll::SpikePawn& entry = extra.pawn[i];
                    json.beginObject();
                    json.string("plugin", entry.plugin);
                    json.string("function", entry.function);
                    json.integer("calls", entry.calls);
                    json.number("self_ms", static_cast<double>(entry.selfNs) / 1e6, 4);
                    json.endObject();
                }
                json.endArray();
            }
            if (extra.hasPhases) {
                json.beginArray("phases");
                for (int i = 0; i < extra.phaseEntries; ++i) {
                    const gamedll::SpikePhase& entry = extra.phase[i];
                    json.beginObject();
                    json.string("phase", phases::name(entry.phase));
                    json.integer("calls", entry.calls);
                    json.number("self_ms", static_cast<double>(entry.selfNs) / 1e6, 4);
                    json.number("total_ms", static_cast<double>(entry.totalNs) / 1e6, 4);
                    json.endObject();
                }
                json.endArray();
            }
            json.beginArray("top");
            for (int i = 0; i < spike.entries; ++i) {
                const gamedll::SpikeEntry& entry = spike.top[i];
                json.beginObject();
                json.string("name", gamedll::name(entry.function));
                json.integer("calls", entry.calls);
                json.number("self_ms", static_cast<double>(entry.selfNs) / 1e6, 4);
                json.number("total_ms", static_cast<double>(entry.totalNs) / 1e6, 4);
                json.endObject();
            }
            json.endArray();
            json.endObject();
        }
        json.endArray();
    }

    json.beginArray("map_changes");
    for (const probe::MapChange& change : run.mapChanges) {
        json.beginObject();
        json.number("at_s", change.atSeconds, 3);
        json.string("from", change.from);
        json.string("to", change.to);
        json.number("load_ms", static_cast<double>(change.loadNs) / 1e6, 3);
        if (change.hasGame) json.number("gamedll_ms", static_cast<double>(change.gamedllNs) / 1e6, 3);
        if (change.hasAmxx) json.number("amxx_ms", static_cast<double>(change.amxxNs) / 1e6, 3);
        json.integer("edict_allocs", static_cast<long long>(change.edictAllocs));
        json.endObject();
    }
    json.endArray();

    {
        const Overhead o = overhead(run);
        const double seconds = wall;
        json.beginObject("instrumentation");
        json.number("wrapper_ns_per_call", run.wrapperNs, 2);
        json.integer("wrapped_calls", static_cast<long long>(o.wrappedCalls));
        json.number("wrapper_ms", o.wrapperMs, 3);
        json.number("pawn_timing_ns_per_call", run.pawnTimingNs, 2);
        json.integer("pawn_calls", static_cast<long long>(o.pawnCalls));
        json.number("pawn_timing_ms", o.pawnMs, 3);
        json.number("phase_mark_ns_per_pair", run.phaseMarkNs, 2);
        json.integer("phase_marks", static_cast<long long>(o.phaseMarks));
        json.number("phase_marks_ms", o.phaseMs, 3);
        json.number("clock_read_ns", run.clockReadNs, 2);
        json.number("cpu_read_ns", run.cpuReadNs, 2);
        json.number("frame_hooks_ms", o.frameMs, 3);
        json.integer("engine_hook_calls", static_cast<long long>(o.hookCalls));
        json.number("engine_hooks_ms", o.hooksMs, 3);
        json.number("total_ms", o.totalMs(), 3);
        json.number("total_pct", seconds > 0.0 ? o.totalMs() / (seconds * 10.0) : 0.0, 4);
        json.endObject();
    }

    // One row a second, as arrays under a column list to keep a long run's
    // report small. Nulls are values nothing was measuring.
    json.beginObject("series");
    json.raw("columns", "[\"t_s\", \"map\", \"frames\", \"fps\", \"busy_pct\", \"over_2_ticks\", "
        "\"period_max_ms\", \"busy_p99_ms\", \"busy_max_ms\", \"game_ms\", \"amxx_ms\", \"delta_ms\", "
        "\"cpu_pct\", \"stalled\", \"humans\", \"bots\", \"hltv\", \"edicts\", \"edict_churn\"]");
    json.beginArray("rows");
    for (const probe::SeriesRow& row : run.series) {
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer),
            "[%.3f, %d, %u, %.1f, %.2f, %u, %.4f, %.4f, %.4f, %s, %s, %.3f, %s, %u, %d, %d, %d, %d, %u]",
            row.atSeconds, row.map, row.frames,
            row.periodNs > 0.0 ? row.frames * 1e9 / row.periodNs : 0.0,
            row.periodNs > 0.0 ? 100.0 * row.busyNs / row.periodNs : 0.0,
            row.overTwoTicks, static_cast<double>(row.periodMaxNs) / 1e6,
            static_cast<double>(row.busyP99Ns) / 1e6, static_cast<double>(row.busyMaxNs) / 1e6,
            nullable(row.hasGame, row.gameNs / 1e6, 3).c_str(),
            nullable(row.hasAmxx, row.amxxNs / 1e6, 3).c_str(),
            row.deltaNs / 1e6,
            nullable(row.hasCpu && row.periodNs > 0.0, 100.0 * row.cpuNs / row.periodNs, 2).c_str(),
            row.stalled, row.players.humans, row.players.bots, row.players.hltv, row.edicts,
            row.edictChurn);
        json.raw(nullptr, buffer);
    }
    json.endArray();
    json.endObject();

    json.beginObject("histograms");
    json.string("unit", "ns, [low, high) per bucket");
    buckets(json, "period", period);
    buckets(json, "busy", busy);
    if (run.frames.game.count() != 0) {
        buckets(json, "game", run.frames.game);
        buckets(json, "engine", run.frames.engine);
    }
    if (run.frames.cpu.count() != 0) buckets(json, "cpu", run.frames.cpu);
    if (run.frames.delta.count() != 0) buckets(json, "delta", run.frames.delta);
    if (run.frames.amxxFrame.count() != 0) buckets(json, "amxx", run.frames.amxxFrame);
    json.endObject();
    json.endObject();

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        error = "cannot create " + dir.u8string() + ": " + ec.message();
        return {};
    }

    const std::string stem = fileSafe(run.tag) + "_" +
        fileSafe(run.maps.empty() ? "nomap" : run.maps.front()) + "_" +
        formatTime(run.startedAt, "%Y%m%d-%H%M%S", false);
    std::filesystem::path path = dir / (stem + ".json");
    for (int n = 2; std::filesystem::exists(path, ec); ++n) {
        path = dir / (stem + "-" + std::to_string(n) + ".json");
    }

    std::ofstream out(path, std::ios::binary);
    out << json.text() << '\n';
    out.close();
    if (!out) {
        error = "cannot write " + path.u8string();
        return {};
    }
    return path;
}

} // namespace report
