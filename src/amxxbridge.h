#pragma once

// The contract between rebench_mm and rebench_amxx, which are separate
// binaries because they are loaded by different things: Metamod loads plugins
// from plugins.ini, AMXX loads modules from modules.ini and rewrites the name
// to modules/<name>_amxx.dll. One file cannot be both.
//
// rebench_mm resolves the module, never the other way round. AMXX attaches its
// modules while it is itself starting up, which is before rebench_mm loads, so
// the module cannot find rebench_mm at attach time; rebench_mm can always find
// the module.
//
// Strings handed out here belong to the module and live as long as it does, so
// the caller may hold them for the length of a call but should copy anything
// it keeps.

#include <cstdint>

#define REBENCH_AMXX_ABI 3u

// Which clock the module times with. It has no engine access of its own, so
// rebench_mm decides and pushes the choice across before enabling timing;
// otherwise the two halves could convert ticks at different rates and report
// the same work as two different numbers.
#define REBENCH_CLOCK_AUTO 0
#define REBENCH_CLOCK_TSC  1
#define REBENCH_CLOCK_OS   2

// Exported from the module with C linkage, returning a RebenchAmxxApi*.
#define REBENCH_AMXX_EXPORT "rebench_amxx_api"

extern "C" {

struct RebenchAmxxEntry {
    const char* plugin;
    const char* function;
    std::uint64_t calls;
    // total is entry to exit; self is total less time spent in nested
    // amx_Exec calls, so a forward that calls another plugin is not charged
    // twice.
    std::uint64_t selfNs;
    std::uint64_t totalNs;
    std::uint64_t maxNs;
};

// One entry's share of the current frame, for the spike log.
struct RebenchAmxxFrameEntry {
    const char* plugin;
    const char* function;
    std::uint32_t calls;
    std::uint64_t selfNs;
};

struct RebenchAmxxApi {
    std::uint32_t abi;

    // Why the detour is or is not in place, for bench info.
    const char* (*status)();

    // Timing is off until asked for. The detour itself stays installed for the
    // life of the module: removing it means writing over the first bytes of
    // amx_Exec, which is not safe to do from a console command, since that
    // runs inside SV_Frame and a plugin may be on the stack.
    int (*active)();
    void (*setActive)(int on);

    // One of REBENCH_CLOCK_*. Only safe with no plugin on the stack, which
    // means between frames: a start timestamp from one source and an end from
    // the other is meaningless.
    void (*setClock)(int clock);

    // Called from the SV_Frame hook. beginFrame clears the per-frame total.
    void (*beginFrame)();
    std::uint64_t (*frameNs)();

    // Monotonic since the module loaded.
    std::uint64_t (*totalNs)();
    std::uint64_t (*totalCalls)();

    // Calls that were counted in the totals but not attributed to an entry,
    // because the table filled or the call stack was deeper than the module
    // tracks. Nonzero means the per-plugin table understates.
    std::uint64_t (*unattributed)();

    // The per-plugin table. Entries are added as they are first seen and never
    // removed, so an entry absent at the start of a run counts as zero.
    int (*count)();
    int (*entry)(int index, RebenchAmxxEntry* out);

    // maxNs is a high water mark and cannot be differenced between two
    // snapshots, so it is reset at the start of a run.
    void (*resetMax)();

    // This frame's heaviest entries by self time, into out, at most max of
    // them. Returns how many were written. Cleared by beginFrame.
    int (*frameTop)(RebenchAmxxFrameEntry* out, int max);

    // The detour's own bookkeeping over `iterations` simulated calls, in ns,
    // with no plugin under it. Leaves the counters untouched.
    std::uint64_t (*overheadNs)(int iterations);

    // Loaded plugin file names by load order, null past the last one.
    const char* (*script)(int index);
};

typedef const RebenchAmxxApi* (*RebenchAmxxApiFn)();

} // extern "C"
