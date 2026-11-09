#include "rehlds.h"

#ifndef _WIN32
#include <dlfcn.h>
#endif

IRehldsApi* g_rehldsApi = nullptr;
IRehldsHookchains* g_rehldsHooks = nullptr;
IRehldsServerStatic* g_rehldsSvs = nullptr;
IRehldsServerData* g_rehldsSv = nullptr;
const RehldsFuncs_t* g_rehldsFuncs = nullptr;

namespace {

CreateInterfaceFn findEngineFactory() {
#ifdef _WIN32
    const HMODULE engine = GetModuleHandleA("swds.dll");
    if (engine == nullptr) return nullptr;
    return reinterpret_cast<CreateInterfaceFn>(GetProcAddress(engine, CREATEINTERFACE_PROCNAME));
#else
    // RTLD_NOLOAD only finds the already-mapped engine and takes a reference,
    // which the dlclose gives back.
    void* engine = dlopen("engine_i486.so", RTLD_NOW | RTLD_NOLOAD);
    if (engine == nullptr) return nullptr;
    auto factory = reinterpret_cast<CreateInterfaceFn>(dlsym(engine, CREATEINTERFACE_PROCNAME));
    dlclose(engine);
    return factory;
#endif
}

} // namespace

CreateInterfaceFn engineFactory() {
    return findEngineFactory();
}

bool rehldsInit(std::string& error) {
    const CreateInterfaceFn factory = findEngineFactory();
    if (factory == nullptr) {
        error = "engine module or its CreateInterface not found";
        return false;
    }
    int code = 0;
    auto* api = reinterpret_cast<IRehldsApi*>(factory(VREHLDS_HLDS_API_VERSION, &code));
    if (api == nullptr) {
        error = "engine is not ReHLDS";
        return false;
    }
    const int major = api->GetMajorVersion();
    const int minor = api->GetMinorVersion();
    if (major != REHLDS_API_VERSION_MAJOR || minor < kRehldsMinMinor) {
        error = "ReHLDS API " + std::to_string(major) + "." + std::to_string(minor) +
            ", need " + std::to_string(REHLDS_API_VERSION_MAJOR) + "." +
            std::to_string(kRehldsMinMinor) + " or newer";
        return false;
    }
    g_rehldsApi = api;
    g_rehldsHooks = api->GetHookchains();
    g_rehldsSvs = api->GetServerStatic();
    g_rehldsSv = api->GetServerData();
    g_rehldsFuncs = api->GetFuncs();
    return true;
}
