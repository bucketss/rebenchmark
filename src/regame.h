#pragma once

#include <cstdint>
#include <string>

// Optional: only a ReGameDLL game DLL exposes this. Everything here degrades to
// nothing when the game is stock mp.dll or another mod.
//
// The game DLL's StartFrame does, in order: g_pGameRules->Think(),
// CLocalNav::Think(), TheBots->StartFrame(), TheTutor->StartFrame(). Only the
// first is hookable, so timing it separates round and game logic from the bot
// manager, which is the other candidate for an expensive StartFrame.
namespace regame {

struct Stats {
    std::uint64_t thinkCalls = 0;
    std::uint64_t thinkNs = 0;
    std::uint64_t maxThinkNs = 0;
    // Called from inside Think, so this time is also counted there.
    std::uint64_t restarts = 0;
    std::uint64_t restartNs = 0;
    std::uint64_t maxRestartNs = 0;
};

// Acquires the ReGameDLL API. False with the reason in error leaves everything
// below inert.
bool init(std::string& error);
bool available();
const std::string& version();

void install();
void uninstall();

void beginFrame();
std::uint64_t frameThinkNs();

void snapshot(Stats& out);
void snapshotRun(Stats& out);
void resetRunMax();

} // namespace regame
