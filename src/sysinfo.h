#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sys {

struct Cpu {
    std::string brand;
    bool invariantTsc = false;   // CPUID 0x80000007 EDX bit 8
    bool hypervisor = false;     // CPUID 1 ECX bit 31
};

const Cpu& cpu();

// "Linux 6.12.38 x86_64", "Windows 10.0.19044".
std::string osName();

// Linux clocksource in use ("tsc", "kvm-clock", ...); empty elsewhere.
std::string clocksource();

// Counters that say whether something else had the CPU, read at both ends of a
// run and differenced. Linux only: /proc/stat and /proc/self/status.
struct Contention {
    bool available = false;
    std::uint64_t stealTicks = 0;   // all CPUs, jiffies the hypervisor took
    std::uint64_t totalTicks = 0;   // all CPUs, every state
    std::uint64_t voluntarySwitches = 0;
    std::uint64_t involuntarySwitches = 0;   // pre-empted while runnable
};

Contention contention();

// 1-minute load average, or a negative number where there is none.
double loadAverage();

// Every binary mapped into the process under root, relative to it, sorted.
std::vector<std::string> loadedBinaries(const std::string& root);

} // namespace sys
