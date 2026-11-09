#pragma once

#include <extdll.h>
#include <rehlds_api.h>

#include <string>

// 3.10 is the first to have both the SV_Frame hookchain (3.7) and
// IRehldsServerData::IsActive (3.10).
constexpr int kRehldsMinMinor = 10;

extern IRehldsApi* g_rehldsApi;
extern IRehldsHookchains* g_rehldsHooks;
extern IRehldsServerStatic* g_rehldsSvs;
extern IRehldsServerData* g_rehldsSv;
extern const RehldsFuncs_t* g_rehldsFuncs;

CreateInterfaceFn engineFactory();

// Fills the globals above, or returns false with the reason in error.
bool rehldsInit(std::string& error);
