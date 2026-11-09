#include "sysinfo.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif

namespace sys {
namespace {

void cpuid(unsigned leaf, unsigned out[4]) {
#ifdef _MSC_VER
    int regs[4];
    __cpuid(regs, static_cast<int>(leaf));
    for (int i = 0; i < 4; ++i) out[i] = static_cast<unsigned>(regs[i]);
#else
    __cpuid(leaf, out[0], out[1], out[2], out[3]);
#endif
}

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

Cpu probeCpu() {
    Cpu result;
    unsigned regs[4];
    cpuid(1, regs);
    result.hypervisor = (regs[2] & (1u << 31)) != 0;

    cpuid(0x80000000u, regs);
    const unsigned maxExtended = regs[0];
    if (maxExtended >= 0x80000004u) {
        char brand[49] = {};
        for (unsigned leaf = 0; leaf < 3; ++leaf) {
            cpuid(0x80000002u + leaf, regs);
            std::memcpy(brand + leaf * 16, regs, 16);
        }
        result.brand = trim(brand);
    }
    if (maxExtended >= 0x80000007u) {
        cpuid(0x80000007u, regs);
        result.invariantTsc = (regs[3] & (1u << 8)) != 0;
    }
    return result;
}

} // namespace

const Cpu& cpu() {
    static const Cpu value = probeCpu();
    return value;
}

std::string osName() {
#ifdef _WIN32
    // GetVersionEx reports 6.2 to anything without a compatibility manifest.
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    const auto getVersion = ntdll != nullptr
        ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))
        : nullptr;
    OSVERSIONINFOW info = {};
    info.dwOSVersionInfoSize = sizeof(info);
    if (getVersion == nullptr || getVersion(&info) != 0) return "Windows";
    return "Windows " + std::to_string(info.dwMajorVersion) + "." +
        std::to_string(info.dwMinorVersion) + "." + std::to_string(info.dwBuildNumber);
#else
    utsname info;
    if (uname(&info) != 0) return "Linux";
    return std::string(info.sysname) + " " + info.release + " " + info.machine;
#endif
}

std::string clocksource() {
#ifdef _WIN32
    return std::string();
#else
    std::ifstream in("/sys/devices/system/clocksource/clocksource0/current_clocksource");
    std::string value;
    std::getline(in, value);
    return trim(value);
#endif
}

Contention contention() {
    Contention result;
#ifndef _WIN32
    std::ifstream stat("/proc/stat");
    std::string label;
    if (!(stat >> label) || label != "cpu") return result;
    // user nice system idle iowait irq softirq steal; guest time is already
    // inside user and nice, so it is left out of the total.
    std::uint64_t fields[8] = {};
    for (auto& field : fields) {
        if (!(stat >> field)) return result;
    }
    for (const auto field : fields) result.totalTicks += field;
    result.stealTicks = fields[7];

    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = line.substr(0, colon);
        const std::uint64_t value = std::strtoull(line.c_str() + colon + 1, nullptr, 10);
        if (key == "voluntary_ctxt_switches") result.voluntarySwitches = value;
        if (key == "nonvoluntary_ctxt_switches") result.involuntarySwitches = value;
    }
    result.available = true;
#endif
    return result;
}

double loadAverage() {
#ifdef _WIN32
    return -1.0;
#else
    std::ifstream in("/proc/loadavg");
    double value = -1.0;
    if (!(in >> value)) return -1.0;
    return value;
#endif
}

namespace {

std::string normalize(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
#ifdef _WIN32
    for (char& c : path) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    return path;
}

} // namespace

std::vector<std::string> loadedBinaries(const std::string& root) {
    std::vector<std::string> paths;
#ifdef _WIN32
    using EnumFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);
    const HMODULE kernel = GetModuleHandleA("kernel32.dll");
    const auto enumModules = kernel != nullptr
        ? reinterpret_cast<EnumFn>(GetProcAddress(kernel, "K32EnumProcessModules"))
        : nullptr;
    if (enumModules == nullptr) return paths;
    HMODULE modules[512];
    DWORD needed = 0;
    if (!enumModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return paths;
    const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 512);
    for (DWORD i = 0; i < count; ++i) {
        char name[MAX_PATH];
        if (GetModuleFileNameA(modules[i], name, sizeof(name)) != 0) paths.push_back(name);
    }
#else
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        const auto slash = line.find('/');
        if (slash == std::string::npos) continue;
        paths.push_back(line.substr(slash));
    }
#endif
    std::string base = normalize(root);
    if (!base.empty() && base.back() != '/') base += '/';

    std::vector<std::string> result;
    for (const std::string& path : paths) {
        const std::string normal = normalize(path);
        if (base.empty() || normal.compare(0, base.size(), base) != 0) continue;
        const std::string lower = normal.substr(base.size());
        const auto endsWith = [&lower](const char* suffix) {
            const std::size_t n = std::strlen(suffix);
            return lower.size() > n && lower.compare(lower.size() - n, n, suffix) == 0;
        };
        const bool binary = endsWith(".so") || lower.find(".so.") != std::string::npos ||
            endsWith(".dll");
        if (!binary) continue;
        std::string relative = path.substr(base.size());
        std::replace(relative.begin(), relative.end(), '\\', '/');
        result.push_back(relative);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

} // namespace sys
