#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct RebenchAmxxFrameEntry;

// Time spent running Pawn, broken down by plugin and public function. The
// measuring is done by rebench_amxx, a separate AMXX module; this side finds
// it and reads it. Without the module everything here reports unavailable and
// the report simply has no amxx section.
//
// This overlaps the game DLL table rather than adding to it: plugins run
// inside the game DLL calls that the level 1 wrappers already time, so a
// plugin's StartFrame forward is counted both here and in StartFrame. That is
// the point -- it says how much of StartFrame was Pawn.
namespace amxx {

struct Entry {
    std::string plugin;
    std::string function;
    std::uint64_t calls = 0;
    std::uint64_t selfNs = 0;
    std::uint64_t totalNs = 0;
    std::uint64_t maxNs = 0;
};

struct Stats {
    bool present = false;
    std::uint64_t totalNs = 0;
    std::uint64_t totalCalls = 0;
    std::uint64_t unattributed = 0;
    std::vector<Entry> entries;
};

// Looks for the module. Cheap to call repeatedly: the result is cached once
// the module is found. Load order puts the module first, since AMXX attaches
// its modules while starting up and rebench_mm loads later, but this stays
// lazy so the two can load in either order.
bool available();

// What the module says about its detour, or why there is nothing to read.
const char* status();

bool active();
void setActive(bool on);

// Keeps the module on the same clock as this side, since it has no engine
// access to be told any other way. One of REBENCH_CLOCK_* from amxxbridge.h.
void setClock(int clock);

// Called from the SV_Frame hook: how much of this frame was spent in Pawn.
// Counts only outermost calls, so a plugin calling a plugin is counted once.
void beginFrame();
std::uint64_t frameNs();

// This frame's heaviest plugin functions, for a spike. Names belong to the
// module; copy them.
int frameTop(RebenchAmxxFrameEntry* out, int max);

// Outermost Pawn time since the module loaded.
std::uint64_t totalNs();

// What the module's timing adds to one Pawn call, measured now. 0 without it.
double overheadNsPerCall();

// Loaded plugin file names, in load order.
std::vector<std::string> scripts();

// maxNs is a high water mark, so it is reset at the start of a run rather
// than differenced.
void resetMax();

void snapshot(Stats& out);

// to minus from, dropping entries that did not run. Entries are keyed by
// plugin and function, so one missing at the start counts as zero.
Stats diff(const Stats& from, const Stats& to);

} // namespace amxx
