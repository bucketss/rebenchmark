#pragma once

#include "gamedll.h"

#include <cstdint>
#include <string>
#include <vector>

namespace phases {

struct Entry {
    int phase = -1;
    std::string name;
    std::uint64_t calls = 0;
    std::uint64_t selfNs = 0;
    std::uint64_t totalNs = 0;
    std::uint64_t maxTotalNs = 0;
};

struct Stats {
    bool present = false;
    std::vector<Entry> entries;
};

bool available();

const char* status();

const char* name(int phase);
int count();

bool active();

bool setActive(bool on, std::string& error);

bool foreign();

void beginFrame();

int frameTop(gamedll::SpikePhase* out, int max);

std::uint64_t dropped();

void resetMax();

void snapshot(Stats& out);

Stats diff(const Stats& from, const Stats& to);

double overheadNsPerMark();

} // namespace phases
