#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Micro benchmarks: one operation, hammered in a loop, so its cost is known
// rather than inferred from a whole frame. The wrapper bench is the important
// one, because every per-call number in a level 1 report carries it.
//
// Read-only operations only: the loop count is derived from a calibration pass,
// not chosen, so anything with a side effect happens an unbounded number of
// times. See the note above Loop in micro.cpp.
namespace micro {

struct Result {
    const char* name = "";
    const char* what = "";
    double nsPerOp = 0.0;
    std::uint64_t ops = 0;
    // Spread over the batches, as a fraction of the median. Anything much above
    // a few percent means the box was busy and the number is soft.
    double spread = 0.0;
    std::string skipped;   // set when the bench could not run
};

// Names accepted by run(), for the usage message.
std::vector<const char*> names();

// Runs one bench by name, or all of them when name is empty. Returns an empty
// vector if the name is unknown.
std::vector<Result> run(const std::string& name);

// Roughly how long run() will block the server, in ms.
double budgetMs(const std::string& name);

} // namespace micro
