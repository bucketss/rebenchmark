#include "regame.h"

#include <extdll.h>
#include <meta_api.h>

#undef min
#undef max
#undef clamp

#include <regamedll_api.h>

#include "clock.h"

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace regame {
namespace {

IReGameApi* g_api = nullptr;
IReGameHookchains* g_hooks = nullptr;
bool g_installed = false;
std::string g_version;

Stats g_stats;
std::uint64_t g_frameThinkTicks = 0;

struct RunMax {
    std::uint64_t think = 0;
    std::uint64_t restart = 0;
};
RunMax g_runMax;

void keepMax(std::uint64_t ns, std::uint64_t& lifetime, std::uint64_t& run) {
    if (ns > lifetime) lifetime = ns;
    if (ns > run) run = ns;
}

CreateInterfaceFn gameFactory() {
    const char* path = gpMetaUtilFuncs != nullptr
        ? gpMetaUtilFuncs->pfnGetGameInfo(PLID, GINFO_REALDLL_FULLPATH)
        : nullptr;
    if (path == nullptr || *path == '\0') return nullptr;
#ifdef _WIN32
    const HMODULE game = GetModuleHandleA(path);
    if (game == nullptr) return nullptr;
    return reinterpret_cast<CreateInterfaceFn>(GetProcAddress(game, CREATEINTERFACE_PROCNAME));
#else
    // RTLD_NOLOAD only finds the already-mapped game DLL and takes a reference,
    // which the dlclose gives back.
    void* game = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    if (game == nullptr) return nullptr;
    auto factory = reinterpret_cast<CreateInterfaceFn>(dlsym(game, CREATEINTERFACE_PROCNAME));
    dlclose(game);
    return factory;
#endif
}

void thinkHook(IReGameHook_CSGameRules_Think* chain) {
    const std::uint64_t start = clk::now();
    chain->callNext();
    const std::uint64_t end = clk::now();

    const std::uint64_t ticks = end > start ? end - start : 0;
    g_frameThinkTicks += ticks;
    const std::uint64_t ns = clk::toNs(ticks);
    ++g_stats.thinkCalls;
    g_stats.thinkNs += ns;
    keepMax(ns, g_stats.maxThinkNs, g_runMax.think);
}

void restartRoundHook(IReGameHook_CSGameRules_RestartRound* chain) {
    const std::uint64_t start = clk::now();
    chain->callNext();
    const std::uint64_t end = clk::now();

    const std::uint64_t ns = clk::toNs(end > start ? end - start : 0);
    ++g_stats.restarts;
    g_stats.restartNs += ns;
    keepMax(ns, g_stats.maxRestartNs, g_runMax.restart);
}

} // namespace

bool init(std::string& error) {
    const CreateInterfaceFn factory = gameFactory();
    if (factory == nullptr) {
        error = "game DLL module or its CreateInterface not found";
        return false;
    }
    int code = 0;
    auto* api = reinterpret_cast<IReGameApi*>(factory(VRE_GAMEDLL_API_VERSION, &code));
    if (api == nullptr) {
        error = "the game DLL is not ReGameDLL";
        return false;
    }
    const int major = api->GetMajorVersion();
    const int minor = api->GetMinorVersion();
    if (major != REGAMEDLL_API_VERSION_MAJOR) {
        error = "ReGameDLL API " + std::to_string(major) + "." + std::to_string(minor) +
            ", built against " + std::to_string(REGAMEDLL_API_VERSION_MAJOR) + ".x";
        return false;
    }
    g_api = api;
    g_hooks = api->GetHookchains();
    g_version = std::to_string(major) + "." + std::to_string(minor);
    return g_hooks != nullptr;
}

bool available() {
    return g_hooks != nullptr;
}

const std::string& version() {
    return g_version;
}

void install() {
    if (g_installed || !available()) return;
    g_hooks->CSGameRules_Think()->registerHook(&thinkHook, HC_PRIORITY_UNINTERRUPTABLE);
    g_hooks->CSGameRules_RestartRound()->registerHook(&restartRoundHook,
        HC_PRIORITY_UNINTERRUPTABLE);
    g_installed = true;
}

void uninstall() {
    if (!g_installed) return;
    g_hooks->CSGameRules_Think()->unregisterHook(&thinkHook);
    g_hooks->CSGameRules_RestartRound()->unregisterHook(&restartRoundHook);
    g_installed = false;
}

void beginFrame() {
    g_frameThinkTicks = 0;
}

std::uint64_t frameThinkNs() {
    return clk::toNs(g_frameThinkTicks);
}

void snapshot(Stats& out) {
    out = g_stats;
}

void snapshotRun(Stats& out) {
    snapshot(out);
    out.maxThinkNs = g_runMax.think;
    out.maxRestartNs = g_runMax.restart;
}

void resetRunMax() {
    g_runMax = RunMax();
}

} // namespace regame
