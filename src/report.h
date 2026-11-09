#pragma once

#include "gamedll.h"
#include "probe.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace report {

// Server state read at the time the report is written.
struct Server {
    std::string hostname;
    int maxPlayers = 0;
    float sysTicrate = 0.0f;
    float fpsMax = 0.0f;
    std::string pingboost;
    std::string rehldsApi;
    int rehldsBuild = 0;
    // Other cvars that change what a frame costs, by name.
    std::vector<std::pair<std::string, std::string>> cvars;
    // rebench's own settings when the report was written.
    std::string levelMode;
    std::string amxxMode;
    std::string phasesMode;
    bool stalls = false;
    double spikeFloorMs = 0.0;
    // Binaries mapped under the HLDS directory, and AMXX plugins by load order.
    std::vector<std::string> binaries;
    std::vector<std::string> plugins;
};

// Console summary of a span of frames, a few lines, no trailing newlines.
std::vector<std::string> summarize(const probe::FrameStats& frames, float sysTicrate);

// Engine work the game DLL table cannot see: the entity delta encode, and
// edict churn with what it cost.
std::vector<std::string> engineTable(const engine::Stats& from, const engine::Stats& to);

// Time spent running Pawn, by plugin and public function, heaviest self time
// first. Empty when the AMXX module is not loaded or timed nothing.
std::vector<std::string> amxxTable(const amxx::Stats& stats, double seconds, int rows);

std::vector<std::string> phasesTable(const phases::Stats& stats, double seconds, int rows);

// Game DLL functions between two sets of counters, heaviest self time first.
std::vector<std::string> functionTable(const gamedll::Totals& from, const gamedll::Totals& to,
    double seconds, int rows);

// Where StartFrame's time went: the real game DLL against everything Metamod
// ran around it.
std::vector<std::string> startFrameBreakdown(const probe::Run& run);

// The spike log, oldest first.
std::vector<std::string> spikeTable(int rows);

// Map changes inside the run and what each load cost.
std::vector<std::string> mapChangeTable(const probe::Run& run);

// What rebench itself added to the run, estimated from measured per-call costs.
std::vector<std::string> instrumentationLines(const probe::Run& run);

// Writes <dir>/<tag>_<map>_<local time>.json. Returns the path, or an empty
// path with error set.
std::filesystem::path write(const probe::Run& run, const Server& server,
    const std::filesystem::path& dir, std::string& error);

} // namespace report
