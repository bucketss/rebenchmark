#include <extdll.h>
#include <meta_api.h>

// extdll.h pulls in Valve's minmax.h, which defines min/max/clamp as macros and
// breaks the std:: versions at their call sites.
#undef min
#undef max
#undef clamp

#include "amxx.h"
#include "amxxbridge.h"
#include "clock.h"
#include "engine.h"
#include "gamedll.h"
#include "micro.h"
#include "phases.h"
#include "probe.h"
#include "regame.h"
#include "rehlds.h"
#include "report.h"
#include "sysinfo.h"

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#ifndef REBENCH_VERSION
#define REBENCH_VERSION "dev"
#endif

enginefuncs_t g_engfuncs;
globalvars_t* gpGlobals = nullptr;
meta_globals_t* gpMetaGlobals = nullptr;
gamedll_funcs_t* gpGamedllFuncs = nullptr;
mutil_funcs_t* gpMetaUtilFuncs = nullptr;

namespace {

constexpr std::size_t kMaxTagLength = 32;

// Frames cheaper than this are never staged as spikes. At level 1 on a busy
// server the worst 32 frames are otherwise all ordinary ones.
constexpr std::uint64_t kDefaultSpikeFloorNs = 5000000;  // 5 ms

// Level 1 and plugin timing are per call, so they are the plugin's whole idle
// cost. By default both are on only while a run records; bench level 0|1 and
// bench amxx on|off pin them either way, and "run" goes back to the default.
enum class Mode { Run, On, Off };
Mode g_levelMode = Mode::Run;
Mode g_amxxMode = Mode::Run;
Mode g_phasesMode = Mode::Run;
// What the current run switched on, and so switches off when it ends.
bool g_runLevel = false;
bool g_runAmxx = false;
bool g_runPhases = false;

const char* modeName(Mode mode) {
    return mode == Mode::Run ? "run" : (mode == Mode::On ? "on" : "off");
}

// Level 1 is only turned on for a run unasked when a clock read is cheaper than this.
// Every wrapped call spends two reads, and pfnAddToFullPack runs once per
// entity per client per frame, so the table's call volume is entity count
// times client count times fps: on a host where a read goes to the kernel that
// is hundreds of milliseconds of timing per second of wall time, and the
// instrumentation costs more than anything it would find. The threshold sits
// between a TSC or vDSO read (5-30 ns) and one the kernel services (300-1000),
// which is a wide enough gap that nothing lands near it. Typing bench level 1
// still works: this only decides what is switched on without being asked.
constexpr double kMaxClockNsForLevel1 = 100.0;

bool clockIsCheap() {
    return clk::g_info.nsPerRead > 0.0 && clk::g_info.nsPerRead <= kMaxClockNsForLevel1;
}

constexpr double kDearCpuReadNs = 200.0;

constexpr double kMaxNumber = 1e7;

void print(const std::string& text) {
    g_engfuncs.pfnServerPrint(("[ReBench] " + text + "\n").c_str());
}

void printIndented(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) g_engfuncs.pfnServerPrint(("  " + line + "\n").c_str());
}

std::filesystem::path pluginPath() {
    const char* path =
        gpMetaUtilFuncs != nullptr ? gpMetaUtilFuncs->pfnGetPluginPath(PLID) : nullptr;
    if (path == nullptr || *path == '\0') return {};
    std::error_code ec;
    auto absolute = std::filesystem::absolute(std::filesystem::u8path(path), ec);
    return ec ? std::filesystem::path() : absolute;
}

// Beside the plugin binary: the working directory belongs to whatever launched
// hlds, and a service wrapper is free to change it.
std::filesystem::path reportsDir() {
    const auto binary = pluginPath();
    return binary.empty() ? std::filesystem::u8path("rebench_reports")
                          : binary.parent_path() / "reports";
}

std::filesystem::path configPath() {
    const auto binary = pluginPath();
    return binary.empty() ? std::filesystem::u8path("rebench.cfg")
                          : binary.parent_path() / "rebench.cfg";
}

// Written with the defaults when missing, so there is always a file to edit.
void writeDefaultConfig() {
    const auto path = configPath();
    std::error_code ec;
    if (std::filesystem::exists(path, ec) || ec) return;
    std::ofstream out(path, std::ios::binary);
    if (!out) return;
    out << "// rebench " REBENCH_VERSION " config\n"
           "\n"
           "bench level run\n"
           "bench amxx run\n"
           "bench engine run\n"
           "bench spikes floor " << (kDefaultSpikeFloorNs / 1000000) << "\n"
           "bench stalls on\n"
           "bench clock auto\n";
}

// Only settings, so the file cannot run arbitrary console commands.
bool configLineAllowed(const std::string& text) {
    for (const char c : text) {
        if (c == ';' || c == '"' || static_cast<unsigned char>(c) < 0x20) return false;
    }
    std::istringstream words(text);
    std::string bench, sub, arg;
    words >> bench >> sub >> arg;
    if (bench != "bench") return false;
    if (sub == "spikes") return arg == "floor";
    return sub == "level" || sub == "amxx" || sub == "engine" || sub == "stalls" ||
        sub == "clock";
}

// What the config held when last applied, so an unchanged file is not applied
// again at a map change: that would undo console changes.
std::string g_configApplied;
bool g_configEverRead = false;
bool g_configWasFound = false;

bool g_configBatch = false;
bool g_configRunning = false;
bool g_configRefused = false;
std::string g_configQueued;

bool g_configCheckPending = false;

void refuseWhileRecording(const std::string& text) {
    print(text);
    if (g_configRunning) g_configRefused = true;
}

// Queued rather than run here, so each line goes through its own command's
// checks. The buffer runs between frames.
void execConfig() {
    if (g_configBatch) return;
    const auto path = configPath();
    std::ifstream in(path, std::ios::binary);
    const bool found = static_cast<bool>(in);
    std::string contents;
    if (found) contents.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (g_configEverRead && found == g_configWasFound && contents == g_configApplied) return;
    g_configEverRead = true;
    g_configWasFound = found;

    if (!found) {
        g_configApplied = contents;
        print("no config at " + path.u8string() + "; using defaults");
        return;
    }
    std::string begin = "bench configbegin\n";
    g_engfuncs.pfnServerCommand(&begin[0]);
    std::istringstream lines(contents);
    std::string text;
    int lineNumber = 0;
    while (std::getline(lines, text)) {
        ++lineNumber;
        if (!text.empty() && text.back() == '\r') text.pop_back();
        const auto comment = text.find("//");
        if (comment != std::string::npos) text.erase(comment);
        const auto first = text.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        text = text.substr(first, text.find_last_not_of(" \t") - first + 1);
        if (!configLineAllowed(text)) {
            print("rebench.cfg line " + std::to_string(lineNumber) + " ignored: " + text);
            continue;
        }
        std::string command = text + "\n";
        g_engfuncs.pfnServerCommand(&command[0]);
    }
    g_configQueued = contents;
    g_configBatch = true;
    g_configRefused = false;
    std::string done = "bench configdone\n";
    g_engfuncs.pfnServerCommand(&done[0]);
    print("config " + path.u8string());
}

void commandConfigBegin() {
    if (g_configBatch) g_configRunning = true;
}

void commandConfigDone() {
    if (!g_configBatch) return;
    g_configBatch = false;
    g_configRunning = false;
    if (g_configRefused) {
        g_configCheckPending = true;
        print("config applied in part while recording; the rest once the run ends");
        return;
    }
    g_configApplied = g_configQueued;
}

float cvarFloat(const char* name) {
    return g_engfuncs.pfnCVarGetFloat(name);
}

std::string fixed(double value, int decimals) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

std::string line(const char* fmt, ...) {
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return buffer;
}

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool parseSeconds(const char* text, double& seconds) {
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(value) || value <= 0.0 ||
        value > kMaxNumber) {
        return false;
    }
    seconds = value;
    return true;
}

bool parseRows(const char* text, int& rows) {
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value <= 0) return false;
    rows = static_cast<int>(value);
    return true;
}

std::string sanitizeTag(const std::string& text) {
    std::string tag;
    for (const char c : text) {
        if (tag.size() == kMaxTagLength) break;
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
        tag += ok ? c : '_';
    }
    return tag;
}

std::string duration(double seconds) {
    const long total = static_cast<long>(seconds);
    char buffer[32];
    if (total >= 3600) {
        std::snprintf(buffer, sizeof(buffer), "%ldh%02ldm%02lds", total / 3600, total / 60 % 60, total % 60);
    } else if (total >= 60) {
        std::snprintf(buffer, sizeof(buffer), "%ldm%02lds", total / 60, total % 60);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1fs", seconds);
    }
    return buffer;
}

std::string rehldsVersion() {
    return std::to_string(g_rehldsApi->GetMajorVersion()) + "." +
        std::to_string(g_rehldsApi->GetMinorVersion());
}

report::Server serverInfo() {
    report::Server server;
    server.levelMode = modeName(g_levelMode);
    server.amxxMode = modeName(g_amxxMode);
    server.phasesMode = modeName(g_phasesMode);
    server.stalls = probe::stallDetection();
    server.spikeFloorMs = static_cast<double>(gamedll::spikeFloorNs()) / 1e6;
    const char* hostname = g_engfuncs.pfnCVarGetString("hostname");
    server.hostname = hostname != nullptr ? hostname : "";
    server.maxPlayers = g_rehldsSvs->GetMaxClients();
    server.sysTicrate = cvarFloat("sys_ticrate");
    server.fpsMax = cvarFloat("fps_max");
    char* next = nullptr;
    if (g_engfuncs.pfnEngCheckParm("-pingboost", &next) != 0) {
        server.pingboost = next != nullptr ? next : "";
    }
    server.rehldsApi = rehldsVersion();
    server.rehldsBuild = g_rehldsFuncs->GetBuildNumber();

    for (const char* name : {"sv_maxrate", "sv_minrate", "sv_maxupdaterate", "sv_minupdaterate",
             "sv_unlag", "sv_maxunlag", "sys_timescale"}) {
        const char* value = g_engfuncs.pfnCVarGetString(name);
        if (value != nullptr && g_engfuncs.pfnCVarGetPointer(name) != nullptr) {
            server.cvars.emplace_back(name, value);
        }
    }

    const char* gamedir = gpMetaUtilFuncs != nullptr
        ? gpMetaUtilFuncs->pfnGetGameInfo(PLID, GINFO_GAMEDIR)
        : nullptr;
    if (gamedir != nullptr && *gamedir != '\0') {
        // The engine may hand back just "cstrike", relative to the HLDS root.
        std::error_code ec;
        const auto absolute = std::filesystem::absolute(std::filesystem::u8path(gamedir), ec);
        if (!ec) server.binaries = sys::loadedBinaries(absolute.parent_path().u8string());
    }
    server.plugins = amxx::scripts();
    return server;
}

// Counters when the game DLL level was turned on, for bench top.
gamedll::Totals g_levelBase;
std::uint64_t g_levelBaseTicks = 0;

bool levelOn(std::string& error) {
    if (gamedll::enabled()) return true;
    if (!gamedll::install(error)) return false;
    gamedll::setEnabled(true);
    gamedll::resetTopMax();
    gamedll::snapshot(g_levelBase);
    g_levelBaseTicks = clk::now();
    return true;
}

bool levelOff(std::string& error) {
    gamedll::setEnabled(false);
    return gamedll::uninstall(error);
}

void syncModuleClock() {
    amxx::setClock(clk::g_info.source == clk::Source::Tsc ? REBENCH_CLOCK_TSC : REBENCH_CLOCK_OS);
}

// amxx base for the live table, the way bench top works off g_levelBase.
amxx::Stats g_amxxBase;
std::uint64_t g_amxxBaseTicks = 0;

void amxxSet(bool on) {
    // Repeating the current state keeps the live table's counters.
    if (amxx::active() == on) return;
    amxx::setActive(on);
    // Timing that started mid-run cannot be differenced, so the base moves
    // with the switch.
    amxx::snapshot(g_amxxBase);
    g_amxxBaseTicks = clk::now();
}

phases::Stats g_phasesBase;
std::uint64_t g_phasesBaseTicks = 0;

bool phasesSet(bool on, std::string& error) {
    if (phases::active() == on) return true;
    if (!phases::setActive(on, error)) return false;
    phases::snapshot(g_phasesBase);
    g_phasesBaseTicks = clk::now();
    return true;
}

// Before beginRun, so the run's start snapshots see the counters already on.
void runTimingOn() {
    g_runLevel = false;
    g_runAmxx = false;

    if (g_levelMode == Mode::Run && !gamedll::enabled()) {
        std::string error;
        if (!clockIsCheap()) {
            print("level 1 left off: a clock read costs " + fixed(clk::g_info.nsPerRead, 0) +
                " ns here; bench level 1 forces it");
        } else if (!levelOn(error)) {
            print("level 1 unavailable: " + error);
        } else {
            g_runLevel = true;
            if (const int foreign = gamedll::foreignSlots()) {
                print(std::to_string(foreign) + " game DLL slots taken over by something else");
            }
        }
    }

    if (g_amxxMode == Mode::Run && amxx::available() && !amxx::active()) {
        amxxSet(true);
        g_runAmxx = true;
    }

    if (g_phasesMode == Mode::Run && phases::available() && !phases::active()) {
        std::string error;
        if (!phasesSet(true, error)) {
            print("engine phases left off: " + error);
        } else {
            g_runPhases = true;
        }
    }
}

// The probe fell back to the OS clock by itself; the live tables' base ticks
// are in the old source.
void clockReverted() {
    gamedll::snapshot(g_levelBase);
    g_levelBaseTicks = clk::now();
    amxx::snapshot(g_amxxBase);
    g_amxxBaseTicks = clk::now();
    phases::snapshot(g_phasesBase);
    g_phasesBaseTicks = clk::now();
}

void runTimingOff() {
    if (g_runLevel) {
        std::string error;
        if (!levelOff(error)) print("level 1 counting stopped, but the wrappers stay: " + error);
    }
    if (g_runAmxx) amxxSet(false);
    if (g_runPhases) {
        std::string error;
        if (!phasesSet(false, error)) print("engine phase marks left on: " + error);
    }
    g_runLevel = false;
    g_runAmxx = false;
    g_runPhases = false;
}

void finishRun(const probe::Run& run) {
    const double seconds = clk::toSeconds(run.stopTicks - run.startTicks);
    print("run \"" + run.tag + "\" ended (" + run.endReason + ") after " + duration(seconds));
    printIndented(report::summarize(run.frames, cvarFloat("sys_ticrate")));
    printIndented(report::engineTable(run.engineStart, run.engineStop));
    printIndented(report::amxxTable(amxx::diff(run.amxxStart, run.amxxStop), seconds, 10));
    if (run.phasesActive) {
        printIndented(report::phasesTable(phases::diff(run.phasesStart, run.phasesStop),
            seconds, 12));
    }
    if (run.gameLevel) {
        printIndented(report::functionTable(run.gameStart, run.gameStop, seconds, 10));
        printIndented(report::startFrameBreakdown(run));
        printIndented(report::spikeTable(5));
    }
    printIndented(report::mapChangeTable(run));
    printIndented(report::instrumentationLines(run));
    std::string error;
    const auto path = report::write(run, serverInfo(), reportsDir(), error);
    if (path.empty()) {
        print("report not written: " + error);
    } else {
        print("report written to " + path.u8string());
    }
    runTimingOff();
}

void commandStatus() {
    const probe::Window window = probe::window();
    const probe::Players players = probe::countPlayers();
    std::string header = "last " + duration(window.seconds) + " on " +
        (probe::currentMap().empty() ? std::string("no map") : probe::currentMap()) + ", " +
        std::to_string(players.humans) + " humans, " + std::to_string(players.bots) + " bots";
    if (players.hltv != 0) header += ", " + std::to_string(players.hltv) + " hltv";
    print(header);
    printIndented(report::summarize(window.frames, cvarFloat("sys_ticrate")));

    if (gamedll::installed()) {
        std::string line = std::string("game DLL wrappers ") +
            (gamedll::enabled() ? "counting" : "installed, not counting");
        if (const int foreign = gamedll::foreignSlots()) {
            line += ", " + std::to_string(foreign) + " slots taken over by something else";
        }
        print(line);
    }

    if (!probe::recording()) {
        print("not recording");
        return;
    }
    const probe::Run& run = probe::currentRun();
    const std::uint64_t now = clk::now();
    std::string line = "recording \"" + run.tag + "\" for " +
        duration(now > run.startTicks ? clk::toSeconds(now - run.startTicks) : 0.0);
    if (run.limitSeconds > 0.0) line += " of " + duration(run.limitSeconds);
    line += ", " + std::to_string(run.frames.period.count()) + " frames";
    print(line);
}

void commandStart() {
    if (probe::recording()) {
        print("already recording \"" + probe::currentRun().tag + "\"; bench stop first");
        return;
    }
    const int argc = g_engfuncs.pfnCmd_Argc();
    std::string tag = "run";
    double seconds = 0.0;
    // "bench start 300" is a duration, not a tag.
    const bool durationOnly = argc == 3 && parseSeconds(g_engfuncs.pfnCmd_Argv(2), seconds);
    if (!durationOnly) {
        if (argc >= 3) tag = sanitizeTag(g_engfuncs.pfnCmd_Argv(2));
        if (argc >= 4 && !parseSeconds(g_engfuncs.pfnCmd_Argv(3), seconds)) {
            print("seconds must be a positive number");
            return;
        }
    }
    if (tag.empty()) tag = "run";
    runTimingOn();
    probe::beginRun(tag, seconds);
    print("recording \"" + tag + "\"" +
        (seconds > 0.0 ? " for " + duration(seconds) : std::string(" until bench stop")));
    const double cpuReadNs = probe::currentRun().cpuReadNs;
    if (cpuReadNs > kDearCpuReadNs) {
        print("stall detection reads cpu time at " + fixed(cpuReadNs, 0) +
            " ns a frame here; bench stalls off drops it");
    }
}

void commandStop() {
    if (!probe::recording()) {
        print("not recording");
        return;
    }
    finishRun(probe::endRun("command"));
}

void setLevel(bool on) {
    std::string error;
    if (on) {
        if (gamedll::enabled()) {
            g_levelMode = Mode::On;
            print("level 1 pinned on; bench top reset restarts the counters");
            return;
        }
        if (!levelOn(error)) {
            print("cannot wrap the game DLL table: " + error);
            return;
        }
        g_levelMode = Mode::On;
        print("level 1 pinned on: timing " + std::to_string(static_cast<int>(gamedll::kFunctions)) +
            " game DLL entry points");
        if (!clockIsCheap()) {
            print("warning: a clock read costs " + fixed(clk::g_info.nsPerRead, 0) +
                " ns here and every wrapped call spends two");
            printIndented({"AddToFullPack alone runs once per entity per client per frame, "
                           "so this can cost the server more than it measures"});
        }
        return;
    }
    g_levelMode = Mode::Off;
    if (!levelOff(error)) {
        print("counting stopped, but the wrappers stay: " + error);
        return;
    }
    print("level 0 pinned: frame timing only, runs included");
}

void setLevelRun() {
    g_levelMode = Mode::Run;
    if (probe::recording()) {
        // Left as it is for the rest of this run, and off once it ends.
        g_runLevel = gamedll::enabled();
        print("level 1 during runs only, from the end of this one");
        return;
    }
    std::string error;
    if (gamedll::enabled() && !levelOff(error)) {
        print("counting stopped, but the wrappers stay: " + error);
        return;
    }
    print("level 1 during runs only");
}

void commandLevel() {
    if (g_engfuncs.pfnCmd_Argc() < 3) {
        if (!gamedll::enabled()) {
            print(g_levelMode == Mode::Run ? "level 0 until a run starts" : "level 0 pinned");
            return;
        }
        print(std::string("level 1 ") + (g_levelMode == Mode::On ? "pinned" : "for this run") +
            ": game DLL functions, " +
            duration(clk::toSeconds(clk::now() - g_levelBaseTicks)) + " of counters");
        return;
    }
    const std::string arg = lower(g_engfuncs.pfnCmd_Argv(2));
    if (arg == "0" || arg == "1") {
        if (probe::recording()) {
            refuseWhileRecording("not while recording: the run is timed at the level it started at");
            return;
        }
        setLevel(arg == "1");
    } else if (arg == "run") {
        setLevelRun();
    } else {
        print("level is 0 (frames only), 1 (plus game DLL functions) or run (1 while recording)");
    }
}

void commandTop() {
    if (!gamedll::enabled()) {
        print("level 0; bench level 1 pins it on, or start a run");
        return;
    }
    int rows = 15;
    if (g_engfuncs.pfnCmd_Argc() >= 3) {
        const std::string arg = lower(g_engfuncs.pfnCmd_Argv(2));
        if (arg == "reset") {
            gamedll::resetTopMax();
            gamedll::snapshot(g_levelBase);
            g_levelBaseTicks = clk::now();
            print("counters reset");
            return;
        }
        if (!parseRows(arg.c_str(), rows)) {
            print("usage: bench top [rows|reset]");
            return;
        }
    }
    gamedll::Totals now;
    gamedll::snapshot(now);
    const double seconds = clk::toSeconds(clk::now() - g_levelBaseTicks);
    print("game DLL over the last " + duration(seconds));
    printIndented(report::functionTable(g_levelBase, now, seconds, rows));
}

void commandSpikes() {
    const int argc = g_engfuncs.pfnCmd_Argc();
    int rows = 10;
    if (argc >= 3) {
        const std::string arg = lower(g_engfuncs.pfnCmd_Argv(2));
        if (arg == "reset") {
            gamedll::resetSpikes();
            print("spike log cleared");
            return;
        }
        if (arg == "floor") {
            double floorMs = 0.0;
            if (argc < 4 || !parseSeconds(g_engfuncs.pfnCmd_Argv(3), floorMs)) {
                print("usage: bench spikes floor <ms>");
                return;
            }
            gamedll::setSpikeFloorNs(static_cast<std::uint64_t>(floorMs * 1e6));
            print("only frames busier than " + fixed(floorMs, 2) + " ms are logged");
            return;
        }
        if (!parseRows(arg.c_str(), rows)) {
            print("usage: bench spikes [rows|reset|floor <ms>]");
            return;
        }
    }
    print("worst " + std::to_string(gamedll::kSpikes) + " frames by time inside SV_Frame");
    printIndented(report::spikeTable(rows));
}

void commandMicro() {
    // Rcon runs inside SV_Frame, so a bench costs the server that frame. Small
    // enough not to matter, but never on top of a run it would land in.
    if (probe::recording()) {
        print("not while recording: the benches would land in the run");
        return;
    }
    std::string name;
    if (g_engfuncs.pfnCmd_Argc() >= 3) {
        name = lower(g_engfuncs.pfnCmd_Argv(2));
        const std::vector<const char*> known = micro::names();
        bool found = false;
        for (const char* candidate : known) found = found || name == candidate;
        if (!found) {
            std::string usage = "usage: bench micro [";
            for (std::size_t i = 0; i < known.size(); ++i) {
                usage += (i != 0 ? "|" : "") + std::string(known[i]);
            }
            print(usage + "]");
            return;
        }
    }
    print("running micro benches, about " + fixed(micro::budgetMs(name), 0) +
        " ms of this frame");
    std::vector<std::string> lines;
    lines.push_back(line("%-11s %10s %8s  %s", "bench", "ns/op", "spread", "what"));
    for (const micro::Result& result : micro::run(name)) {
        if (!result.skipped.empty()) {
            lines.push_back(line("%-11s %10s %8s  %s", result.name, "-", "-",
                ("skipped: " + result.skipped).c_str()));
            continue;
        }
        lines.push_back(line("%-11s %10.1f %7.0f%%  %s", result.name, result.nsPerOp,
            result.spread * 100.0, result.what));
    }
    printIndented(lines);
}

void commandAmxx() {
    const int argc = g_engfuncs.pfnCmd_Argc();
    if (!amxx::available()) {
        print(std::string("no plugin timing: ") + amxx::status());
        printIndented({"add rebench to addons/amxmodx/configs/modules.ini and restart the "
                       "server, with rebench_amxx.dll in addons/amxmodx/modules"});
        return;
    }

    int rows = 15;
    if (argc >= 3) {
        const std::string arg = lower(g_engfuncs.pfnCmd_Argv(2));
        if (arg == "on" || arg == "off" || arg == "run") {
            if (probe::recording()) {
                refuseWhileRecording("not while recording: the run would be measured two different ways");
                return;
            }
            g_amxxMode = arg == "on" ? Mode::On : (arg == "off" ? Mode::Off : Mode::Run);
            amxxSet(g_amxxMode == Mode::On);
            print(g_amxxMode == Mode::Run
                ? std::string("plugin timing during runs only")
                : std::string("plugin timing pinned ") + (amxx::active() ? "on" : "off"));
            return;
        }
        if (arg == "reset") {
            amxx::snapshot(g_amxxBase);
            g_amxxBaseTicks = clk::now();
            print("plugin counters reset");
            return;
        }
        if (!parseRows(arg.c_str(), rows)) {
            print("usage: bench amxx [on|off|run|reset|rows]");
            return;
        }
    }

    if (!amxx::active()) {
        print(std::string("plugin timing is off (") + modeName(g_amxxMode) +
            "); bench amxx on pins it on");
        return;
    }

    amxx::Stats now;
    amxx::snapshot(now);
    const double seconds = clk::toSeconds(clk::now() - g_amxxBaseTicks);
    print("plugins over the last " + duration(seconds));
    printIndented(report::amxxTable(amxx::diff(g_amxxBase, now), seconds, rows));
}

void commandEngine() {
    const int argc = g_engfuncs.pfnCmd_Argc();
    if (!phases::available()) {
        print(std::string("no engine phases: ") + phases::status());
        printIndented({"needs a ReHLDS built with the rebench probe patch, which marks the "
                       "phase boundaries inside SV_Frame"});
        return;
    }

    int rows = 15;
    if (argc >= 3) {
        const std::string arg = lower(g_engfuncs.pfnCmd_Argv(2));
        if (arg == "on" || arg == "off" || arg == "run") {
            if (probe::recording()) {
                refuseWhileRecording("not while recording: the run would be measured two different ways");
                return;
            }
            const Mode mode = arg == "on" ? Mode::On : (arg == "off" ? Mode::Off : Mode::Run);
            std::string error;
            if (!phasesSet(mode == Mode::On, error)) {
                print("engine phases: " + error);
                return;
            }
            g_phasesMode = mode;
            print(g_phasesMode == Mode::Run
                ? std::string("engine phases during runs only")
                : std::string("engine phases pinned ") + (phases::active() ? "on" : "off"));
            return;
        }
        if (arg == "reset") {
            phases::snapshot(g_phasesBase);
            g_phasesBaseTicks = clk::now();
            print("engine phase counters reset");
            return;
        }
        if (!parseRows(arg.c_str(), rows)) {
            print("usage: bench engine [on|off|run|reset|rows]");
            return;
        }
    }

    if (!phases::active()) {
        print(std::string("engine phases are off (") + modeName(g_phasesMode) +
            "); bench engine on pins them on");
        if (phases::foreign()) {
            printIndented({"another module is reading the engine's phase marks"});
        }
        return;
    }

    phases::Stats now;
    phases::snapshot(now);
    const double seconds = clk::toSeconds(clk::now() - g_phasesBaseTicks);
    print("engine phases over the last " + duration(seconds));
    printIndented(report::phasesTable(phases::diff(g_phasesBase, now), seconds, rows));
    if (const std::uint64_t dropped = phases::dropped()) {
        printIndented({std::to_string(dropped) + " mark(s) dropped: nesting too deep, or the "
                       "engine's marks did not pair"});
    }
}

std::string checkLine() {
    const clk::Check& check = clk::g_check;
    if (check.checks == 0) return "cross-check: nothing sampled yet";
    std::string line = "cross-check: " + std::to_string(check.divergences) +
        " of " + std::to_string(check.checks) + " samples disagreed with the OS clock";
    if (check.divergences != 0) {
        line += ", worst by " + fixed(static_cast<double>(check.worstNs) / 1e6, 2) + " ms";
    }
    return line;
}

void commandClock() {
    if (g_engfuncs.pfnCmd_Argc() < 3) {
        print(std::string("clock ") + clk::sourceName() + ", " +
            fixed(clk::g_info.nsPerRead, 0) + " ns/read");
        if (clk::g_info.tscForced || clk::g_check.reverted) print(checkLine());
        return;
    }
    // Switching with a run open would leave its start timestamp in one
    // source's ticks and its end in another's.
    if (probe::recording()) {
        refuseWhileRecording("not while recording: the run is timed in the clock it started on");
        return;
    }
    const std::string arg = lower(g_engfuncs.pfnCmd_Argv(2));
    const clk::Source wasSource = clk::g_info.source;
    const double wasHz = clk::g_info.hz;
    if (arg == "tsc") {
        if (!clk::useTsc()) {
            print("no usable TSC rate was seen at load; staying on the OS clock");
            return;
        }
    } else if (arg == "os") {
        clk::useOs();
    } else if (arg == "auto") {
        clk::useAuto();
    } else {
        print("clock is auto (follow the CPU), tsc (use it anyway) or os");
        return;
    }
    syncModuleClock();
    // Same source at the same rate: every tick in hand is still good.
    if (clk::g_info.source == wasSource && clk::g_info.hz == wasHz) {
        print(std::string("clock ") + clk::sourceName() + ", " +
            fixed(clk::g_info.nsPerRead, 0) + " ns/read, unchanged");
        return;
    }
    // The frame timestamps in hand are in the old source's ticks, and the
    // counters either side of the switch were converted at the old rate.
    probe::clockChanged();
    gamedll::snapshot(g_levelBase);
    g_levelBaseTicks = clk::now();
    amxx::snapshot(g_amxxBase);
    g_amxxBaseTicks = clk::now();
    phases::snapshot(g_phasesBase);
    g_phasesBaseTicks = clk::now();
    print(std::string("clock ") + clk::sourceName() + ", " +
        fixed(clk::g_info.nsPerRead, 0) + " ns/read, counters reset");
    if (clk::g_info.tscForced) {
        printIndented({"a span is cross-checked against the OS clock every 100 frames"});
    }
    if (clockIsCheap() && g_levelMode == Mode::Run) {
        printIndented({"a read is cheap enough for runs to use level 1 now"});
    }
}

void commandStalls() {
    if (g_engfuncs.pfnCmd_Argc() < 3) {
        std::string line = std::string("stall detection ") +
            (probe::stallDetection() ? "on" : "off");
        if (!clk::g_cpu.available) line += ", but there is no cpu clock on this platform";
        print(line);
        return;
    }
    const std::string arg = lower(g_engfuncs.pfnCmd_Argv(2));
    if (arg != "on" && arg != "off") {
        print("stalls is on or off");
        return;
    }
    // Half a run with cpu time and half without would read as a run whose
    // frames mostly did not stall.
    if (probe::recording()) {
        refuseWhileRecording("not while recording: the run would have cpu time for only part of it");
        return;
    }
    probe::setStallDetection(arg == "on");
    print(std::string("stall detection ") + (probe::stallDetection() ? "on" : "off"));
    if (!probe::stallDetection()) {
        printIndented({"spikes and the worst frame can no longer say whether the host took "
                       "the cpu away or the game really was busy"});
    }
}

void commandInfo() {
    const sys::Cpu& cpu = sys::cpu();
    print("rebench " REBENCH_VERSION ", ReHLDS API " + rehldsVersion() + " build " +
        std::to_string(g_rehldsFuncs->GetBuildNumber()));
    std::string clock = std::string("clock ") + clk::sourceName() + " at " +
        std::to_string(static_cast<long long>(clk::g_info.hz)) + " Hz, " +
        fixed(clk::g_info.nsPerRead, 0) + " ns/read";
    const std::string clocksource = sys::clocksource();
    if (!clocksource.empty()) clock += ", kernel clocksource " + clocksource;
    print(clock);
    // A counter ticking at the nominal rate while the invariant bit is clear is
    // what a hypervisor masking that bit looks like, and is worth telling apart
    // from a counter that is genuinely unusable.
    if (clk::g_info.source != clk::Source::Tsc && clk::g_info.tscHz > 1e8) {
        print("tsc ran at " + fixed(clk::g_info.tscHz / 1e9, 2) +
            " GHz during calibration but is not advertised invariant and the OS clock is "
            "cheap enough not to need it; bench clock tsc forces it");
    }
    if (clk::g_info.tscForced || clk::g_check.reverted) print(checkLine());
    if (clk::g_cpu.available) {
        const double cpuReadNs = probe::measureCpuReadNs();
        print(std::string("cpu clock ") + clk::cpuSourceName() + " at " +
            std::to_string(static_cast<long long>(clk::g_cpu.hz)) + " Hz, " +
            fixed(cpuReadNs, 0) + " ns/read");
        if (probe::stallDetection() && cpuReadNs > kDearCpuReadNs) {
            printIndented({"stall detection spends one of these a frame, most of rebench's own "
                           "cost here; bench stalls off drops it"});
        }
    } else {
        print(std::string("cpu clock ") + clk::cpuSourceName() + " (no stall detection)");
    }
    print("cpu " + (cpu.brand.empty() ? std::string("unknown") : cpu.brand) +
        (cpu.invariantTsc ? ", invariant tsc" : ", no invariant tsc") +
        (cpu.hypervisor ? ", under a hypervisor" : ""));
    print("os " + sys::osName());
    print(std::string("level 1: ") + modeName(g_levelMode) + (gamedll::enabled() ? ", on" : ", off"));
    print(std::string("plugin timing: ") + amxx::status() +
        (amxx::available() ? std::string(", ") + modeName(g_amxxMode) +
            (amxx::active() ? ", on" : ", off") : std::string()));
    print(std::string("engine phases: ") + phases::status() +
        (phases::available() ? std::string(", ") + modeName(g_phasesMode) +
            (phases::active() ? ", on" : ", off") : std::string()));
    print("reports in " + reportsDir().u8string());
}

void commandUnload() {
    // meta unload run from rcon executes inside SV_Frame, under our own hook.
    // The command buffer runs between frames.
    const auto binary = pluginPath();
    if (binary.empty()) {
        print("plugin path unknown; use meta unload from the server console");
        return;
    }
    std::string command = "meta unload " + binary.stem().u8string() + "\n";
    g_engfuncs.pfnServerCommand(&command[0]);
    print("queued " + command.substr(0, command.size() - 1));
}

void commandHelp() {
    print("commands:");
    printIndented({
        "bench status                   frame timing over the last ~10 s",
        "bench start [tag] [seconds]    record a run; with seconds it stops itself",
        "bench stop                     end the run and write its report",
        "bench level [0|1|run]          1 also times every game DLL entry point; run = while recording",
        "bench top [rows|reset]         game DLL functions by self time",
        "bench spikes [rows|reset]      the worst frames and what was in them",
        "bench spikes floor <ms>        log only frames busier than this",
        "bench micro [name]             cost of one operation, hammered in a loop",
        "bench amxx [on|off|run|reset|rows] time in Pawn, by plugin and function",
        "bench engine [on|off|run|reset|rows] where the engine's own frame time went",
        "bench clock [auto|tsc|os]      which timer to measure with",
        "bench stalls [on|off]          read cpu time per frame to spot host stalls",
        "bench info                     clock, cpu, os, ReHLDS, report directory",
        "bench unload                   unload the plugin (safe from rcon)",
    });
}

void commandBench() {
    const std::string sub =
        g_engfuncs.pfnCmd_Argc() >= 2 ? lower(g_engfuncs.pfnCmd_Argv(1)) : std::string("help");
    if (sub == "status") {
        commandStatus();
    } else if (sub == "start") {
        commandStart();
    } else if (sub == "stop") {
        commandStop();
    } else if (sub == "level") {
        commandLevel();
    } else if (sub == "top") {
        commandTop();
    } else if (sub == "spikes") {
        commandSpikes();
    } else if (sub == "micro") {
        commandMicro();
    } else if (sub == "amxx") {
        commandAmxx();
    } else if (sub == "engine") {
        commandEngine();
    } else if (sub == "clock") {
        commandClock();
    } else if (sub == "stalls") {
        commandStalls();
    } else if (sub == "info") {
        commandInfo();
    } else if (sub == "unload") {
        commandUnload();
    } else if (sub == "configbegin") {
        commandConfigBegin();
    } else if (sub == "configdone") {
        commandConfigDone();
    } else {
        commandHelp();
    }
}

// From the first frame rather than Meta_Attach: the AMXX module has loaded by
// then, and level 1 wrappers installed while Metamod is still loading plugins
// are overwritten when it rebuilds its table.
bool g_defaultsPending = true;

void applyDefaults() {
    g_configCheckPending = false;
    g_defaultsPending = false;

    // The module outlives this plugin, so a previous load may have left its
    // timing on, and it chose its clock on its own when AMXX attached it.
    if (amxx::available() && !probe::recording()) {
        if (amxx::active()) amxxSet(false);
        syncModuleClock();
    }

    execConfig();
}

// These two bracket everything Metamod dispatches after us: with rebench loaded
// last, that is the real game DLL's StartFrame.
void startFramePre() {
    if (g_defaultsPending) {
        applyDefaults();
    } else if (g_configCheckPending && !probe::recording()) {
        g_configCheckPending = false;
        execConfig();
    }
    gamedll::markStartFrameEnter();
    gpMetaGlobals->mres = MRES_IGNORED;
}

void startFramePost() {
    gamedll::markStartFrameLeave();
    gpMetaGlobals->mres = MRES_IGNORED;
}

void serverActivatePost(edict_t*, int, int) {
    probe::mapStarted(STRING(gpGlobals->mapname));
    g_configCheckPending = true;
    gpMetaGlobals->mres = MRES_IGNORED;
}

void serverDeactivate() {
    probe::mapEnded();
    gpMetaGlobals->mres = MRES_IGNORED;
}

DLL_FUNCTIONS g_functions = {};
DLL_FUNCTIONS g_functionsPost = {};

int getEntityApi2(DLL_FUNCTIONS* table, int* version) {
    if (table == nullptr || version == nullptr) return FALSE;
    if (*version != INTERFACE_VERSION) {
        *version = INTERFACE_VERSION;
        return FALSE;
    }
    g_functions.pfnServerDeactivate = &serverDeactivate;
    g_functions.pfnStartFrame = &startFramePre;
    std::memcpy(table, &g_functions, sizeof(g_functions));
    return TRUE;
}

int getEntityApi2Post(DLL_FUNCTIONS* table, int* version) {
    if (table == nullptr || version == nullptr) return FALSE;
    if (*version != INTERFACE_VERSION) {
        *version = INTERFACE_VERSION;
        return FALSE;
    }
    g_functionsPost.pfnServerActivate = &serverActivatePost;
    g_functionsPost.pfnStartFrame = &startFramePost;
    std::memcpy(table, &g_functionsPost, sizeof(g_functionsPost));
    return TRUE;
}

} // namespace

plugin_info_t Plugin_info = {
    META_INTERFACE_VERSION,
    "ReBench",
    REBENCH_VERSION,
    __DATE__,
    "arc",
    "",
    "REBENCH",
    PT_ANYTIME,
    // Metamod tracks the command registered through its engine table and
    // disables it on unload, and the SV_Frame hook comes out in Meta_Detach.
    PT_ANYTIME
};

C_DLLEXPORT void WINAPI GiveFnptrsToDll(enginefuncs_t* engineFunctions, globalvars_t* globals) {
    std::memcpy(&g_engfuncs, engineFunctions, sizeof(g_engfuncs));
    gpGlobals = globals;
}

C_DLLEXPORT int Meta_Query(char*, plugin_info_t** pluginInfo, mutil_funcs_t* utilityFunctions) {
    if (pluginInfo == nullptr || utilityFunctions == nullptr) return FALSE;
    *pluginInfo = &Plugin_info;
    gpMetaUtilFuncs = utilityFunctions;
    return TRUE;
}

C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME now, META_FUNCTIONS* functionTable,
    meta_globals_t* globals, gamedll_funcs_t* gameFunctions) {
    if (functionTable == nullptr || globals == nullptr) return FALSE;
    std::string error;
    if (!rehldsInit(error)) {
        LOG_ERROR(PLID, "ReHLDS %d.%d or newer required: %s",
            REHLDS_API_VERSION_MAJOR, kRehldsMinMinor, error.c_str());
        return FALSE;
    }
    gpMetaGlobals = globals;
    gpGamedllFuncs = gameFunctions;
    META_FUNCTIONS functions = {};
    functions.pfnGetEntityAPI2 = &getEntityApi2;
    functions.pfnGetEntityAPI2_Post = &getEntityApi2Post;
    std::memcpy(functionTable, &functions, sizeof(functions));

    clk::init();
    engine::install();
    std::string regameError;
    if (regame::init(regameError)) {
        regame::install();
    } else {
        LOG_MESSAGE(PLID, "no ReGameDLL breakdown: %s", regameError.c_str());
    }
    probe::start(&finishRun, &clockReverted);
    // Loaded mid-map: ServerActivate has already been and gone.
    if (now != PT_STARTUP && g_rehldsSv->IsActive()) probe::mapStarted(STRING(gpGlobals->mapname));
    g_engfuncs.pfnAddServerCommand("bench", &commandBench);
    // The config waits for the first frame; see applyDefaults.
    gamedll::setSpikeFloorNs(kDefaultSpikeFloorNs);
    writeDefaultConfig();
    return TRUE;
}

C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME, PL_UNLOAD_REASON) {
    // Unloading now would unmap the code SV_Frame is about to return into.
    if (probe::inFrame()) {
        print("cannot unload from inside a server frame (rcon); use bench unload");
        return FALSE;
    }
    if (gamedll::busy()) {
        print("cannot unload from inside a game DLL call; run it again from the server console");
        return FALSE;
    }
    // Same for the game DLL table: the engine must not be left calling wrappers
    // that are about to disappear. uninstall refuses before touching anything,
    // so a refused unload leaves counting as it was.
    std::string error;
    if (!gamedll::uninstall(error)) {
        print("cannot unload: " + error);
        return FALSE;
    }
    if (probe::recording()) finishRun(probe::endRun("unload"));
    if (amxx::active()) amxx::setActive(false);
    if (phases::active()) {
        std::string phasesError;
        phasesSet(false, phasesError);
    }
    probe::stop();
    engine::uninstall();
    regame::uninstall();
    return TRUE;
}
