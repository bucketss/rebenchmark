#include "gamedll.h"

#include <extdll.h>

#undef min
#undef max
#undef clamp

#include "clock.h"
#include "rehlds.h"

#include <cstring>

namespace gamedll {
namespace {

// Deep enough for the game calling back into itself through the engine, which
// happens when an entity spawns or touches another during a think.
constexpr int kMaxDepth = 64;

const char* const g_names[kFunctions] = {
#define REBENCH_NAME(name) #name + 3,
    REBENCH_DLL_FUNCTIONS(REBENCH_NAME)
#undef REBENCH_NAME
};

DLL_FUNCTIONS* g_table = nullptr;
void* g_original[kFunctions] = {};
bool g_installed = false;
bool g_enabled = false;

FuncStat g_stat[kFunctions];
// The worst single call since the run began, which the sums cannot express.
std::uint64_t g_runMax[kFunctions];
std::uint64_t g_topMax[kFunctions];

// Reset every frame, so a spike can be broken down by function.
struct FrameAcc {
    std::uint32_t calls;
    std::uint64_t selfNs;
    std::uint64_t totalNs;
};
FrameAcc g_frame[kFunctions];
bool g_listed[kFunctions];
int g_touched[kFunctions];
int g_touchedCount = 0;

struct Active {
    int function;
    std::uint64_t start;
    std::uint64_t childTicks;
};
Active g_stack[kMaxDepth];
int g_depth = 0;
std::uint64_t g_topLevelTicks = 0;
std::uint64_t g_topLevelTotalNs = 0;
std::uint64_t g_overflows = 0;

// Kept in descending order of busyNs, so the weakest entry is the last.
Spike g_spikes[kSpikes];
int g_spikeCount = 0;
Spike g_staged;
bool g_hasStaged = false;
std::uint64_t g_spikeFloorNs = 0;

void enter(int function) {
    Active& active = g_stack[g_depth++];
    active.function = function;
    active.childTicks = 0;
    active.start = clk::now();
}

void leave() {
    const std::uint64_t end = clk::now();
    Active& active = g_stack[--g_depth];

    // The TSC can step back when a thread migrates between unsynchronised cores.
    std::uint64_t total = end > active.start ? end - active.start : 0;
    if (active.childTicks > total) active.childTicks = total;
    const std::uint64_t self = total - active.childTicks;

    if (g_depth > 0) {
        g_stack[g_depth - 1].childTicks += total;
    } else {
        g_topLevelTicks += total;
    }

    const int function = active.function;
    const std::uint64_t totalNs = clk::toNs(total);
    const std::uint64_t selfNs = clk::toNs(self);
    if (g_depth == 0) g_topLevelTotalNs += totalNs;

    FuncStat& stat = g_stat[function];
    ++stat.calls;
    stat.selfNs += selfNs;
    stat.totalNs += totalNs;
    if (totalNs > g_topMax[function]) g_topMax[function] = totalNs;
    if (totalNs > g_runMax[function]) g_runMax[function] = totalNs;

    FrameAcc& frame = g_frame[function];
    if (!g_listed[function]) {
        g_listed[function] = true;
        g_touched[g_touchedCount++] = function;
    }
    ++frame.calls;
    frame.selfNs += selfNs;
    frame.totalNs += totalNs;
}

// Times one call and charges it to its caller. Inactive when counting is off or
// the stack is full, so that leave() only ever runs for a matching enter().
class Scope {
public:
    explicit Scope(int function) : active_(g_enabled && g_depth < kMaxDepth) {
        if (active_) {
            enter(function);
        } else if (g_enabled) {
            ++g_overflows;
        }
    }
    ~Scope() {
        if (active_) leave();
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const bool active_;
};

template <int Function, typename Fn>
struct Thunk;

template <int Function, typename Ret, typename... Args>
struct Thunk<Function, Ret (*)(Args...)> {
    static Ret call(Args... args) {
        Scope scope(Function);
        return reinterpret_cast<Ret (*)(Args...)>(g_original[Function])(args...);
    }
};

#define REBENCH_THUNK(name) (&Thunk<k_##name, decltype(g_table->name)>::call)

Inner g_inner;
std::uint64_t g_innerStart = 0;
bool g_inInner = false;

void insertStaged() {
    int at = g_spikeCount;
    while (at > 0 && g_spikes[at - 1].busyNs < g_staged.busyNs) --at;
    if (at >= kSpikes) return;
    const int last = g_spikeCount < kSpikes ? g_spikeCount : kSpikes - 1;
    for (int i = last; i > at; --i) g_spikes[i] = g_spikes[i - 1];
    g_spikes[at] = g_staged;
    if (g_spikeCount < kSpikes) ++g_spikeCount;
}

} // namespace

const char* name(int function) {
    return function >= 0 && function < kFunctions ? g_names[function] : "?";
}

bool install(std::string& error) {
    if (g_installed) return true;
    if (g_rehldsFuncs->GetEntityInterface == nullptr) {
        error = "ReHLDS has no GetEntityInterface";
        return false;
    }
    g_table = g_rehldsFuncs->GetEntityInterface();
    if (g_table == nullptr) {
        error = "the engine has no game DLL table";
        return false;
    }
#define REBENCH_INSTALL(name)                                                 \
    if (g_table->name != nullptr) {                                           \
        g_original[k_##name] = reinterpret_cast<void*>(g_table->name);        \
        g_table->name = REBENCH_THUNK(name);                                  \
    }
    REBENCH_DLL_FUNCTIONS(REBENCH_INSTALL)
#undef REBENCH_INSTALL
    g_installed = true;
    return true;
}

int foreignSlots() {
    if (!g_installed) return 0;
    int foreign = 0;
#define REBENCH_CHECK(name) \
    if (g_original[k_##name] != nullptr && g_table->name != REBENCH_THUNK(name)) ++foreign;
    REBENCH_DLL_FUNCTIONS(REBENCH_CHECK)
#undef REBENCH_CHECK
    return foreign;
}

bool uninstall(std::string& error) {
    if (!g_installed) return true;
    if (const int foreign = foreignSlots()) {
        error = std::to_string(foreign) + (foreign == 1 ? " game DLL slot was" : " game DLL slots were") +
            " replaced by something else; removing ours would crash";
        return false;
    }
#define REBENCH_UNINSTALL(name)                                               \
    if (g_original[k_##name] != nullptr) {                                    \
        g_table->name = reinterpret_cast<decltype(g_table->name)>(g_original[k_##name]); \
    }
    REBENCH_DLL_FUNCTIONS(REBENCH_UNINSTALL)
#undef REBENCH_UNINSTALL
    g_installed = false;
    g_enabled = false;
    return true;
}

bool installed() {
    return g_installed;
}

bool enabled() {
    return g_enabled && g_installed;
}

void setEnabled(bool on) {
    g_enabled = on && g_installed;
    if (!g_enabled) clearFrame();
}

bool busy() {
    return g_depth > 0;
}

std::uint64_t depthOverflows() {
    return g_overflows;
}

std::uint64_t topLevelTotalNs() {
    return g_topLevelTotalNs;
}

void beginFrame() {
    g_topLevelTicks = 0;
}

std::uint64_t frameGamedllNs() {
    return clk::toNs(g_topLevelTicks);
}

bool spikeWanted(std::uint64_t busyNs) {
    if (!enabled() || busyNs < g_spikeFloorNs) return false;
    return g_spikeCount < kSpikes || busyNs > g_spikes[g_spikeCount - 1].busyNs;
}

void stageSpike(const Context& context) {
    Spike& spike = g_staged;
    spike = Spike();
    spike.busyNs = context.busyNs;
    spike.gamedllNs = context.gamedllNs;
    spike.atSeconds = context.atSeconds;
    spike.mapTime = context.mapTime;
    spike.humans = context.humans;
    spike.bots = context.bots;
    spike.extra = context.extra;
    if (context.map != nullptr) {
        std::strncpy(spike.map, context.map, sizeof(spike.map) - 1);
    }

    // The heaviest functions of this frame by self time, most expensive first.
    for (int n = 0; n < g_touchedCount; ++n) {
        const int function = g_touched[n];
        const FrameAcc& frame = g_frame[function];
        const int last = spike.entries < kSpikeTop ? spike.entries : kSpikeTop - 1;
        if (spike.entries == kSpikeTop && frame.selfNs <= spike.top[last].selfNs) continue;
        int at = spike.entries < kSpikeTop ? spike.entries : kSpikeTop - 1;
        while (at > 0 && spike.top[at - 1].selfNs < frame.selfNs) --at;
        for (int i = last; i > at; --i) spike.top[i] = spike.top[i - 1];
        spike.top[at].function = function;
        spike.top[at].calls = frame.calls;
        spike.top[at].selfNs = frame.selfNs;
        spike.top[at].totalNs = frame.totalNs;
        if (spike.entries < kSpikeTop) ++spike.entries;
    }
    g_hasStaged = true;
}

void commitSpike(std::uint64_t periodNs, bool hasCpu, std::uint64_t cpuNs) {
    if (!g_hasStaged) return;
    g_hasStaged = false;
    g_staged.periodNs = periodNs;
    g_staged.cpuNs = hasCpu ? cpuNs : 0;
    g_staged.hasCpu = hasCpu;
    insertStaged();
}

void dropSpike() {
    g_hasStaged = false;
}

void clearFrame() {
    for (int n = 0; n < g_touchedCount; ++n) {
        const int function = g_touched[n];
        g_frame[function] = FrameAcc{};
        g_listed[function] = false;
    }
    g_touchedCount = 0;
    g_topLevelTicks = 0;
}

void markStartFrameEnter() {
    g_innerStart = clk::now();
    g_inInner = true;
}

void markStartFrameLeave() {
    if (!g_inInner) return;
    g_inInner = false;
    const std::uint64_t end = clk::now();
    if (end > g_innerStart) g_inner.ns += clk::toNs(end - g_innerStart);
    ++g_inner.calls;
}

void snapshotInner(Inner& out) {
    out = g_inner;
}

std::uint64_t measureWrapperNs(int iterations, std::uint64_t& sink) {
    const bool wasEnabled = g_enabled;
    const FuncStat savedStat = g_stat[k_pfnGameInit];
    const std::uint64_t savedMax = g_runMax[k_pfnGameInit];
    const std::uint64_t savedTopMax = g_topMax[k_pfnGameInit];
    const std::uint64_t savedCallerChild = g_depth > 0 ? g_stack[g_depth - 1].childTicks : 0;
    const std::uint64_t savedTop = g_topLevelTicks;
    const std::uint64_t savedTopTotal = g_topLevelTotalNs;
    const bool wasListed = g_listed[k_pfnGameInit];
    const FrameAcc savedFrame = g_frame[k_pfnGameInit];
    const int savedTouched = g_touchedCount;
    g_enabled = true;

    const std::uint64_t start = clk::now();
    for (int i = 0; i < iterations; ++i) {
        Scope scope(k_pfnGameInit);
        sink += static_cast<std::uint64_t>(i);
    }
    const std::uint64_t elapsed = clk::toNs(clk::now() - start);

    g_enabled = wasEnabled;
    g_stat[k_pfnGameInit] = savedStat;
    g_runMax[k_pfnGameInit] = savedMax;
    g_topMax[k_pfnGameInit] = savedTopMax;
    if (g_depth > 0) g_stack[g_depth - 1].childTicks = savedCallerChild;
    g_topLevelTicks = savedTop;
    g_topLevelTotalNs = savedTopTotal;
    g_frame[k_pfnGameInit] = savedFrame;
    g_listed[k_pfnGameInit] = wasListed;
    g_touchedCount = savedTouched;
    return elapsed;
}

void snapshot(Totals& out) {
    std::memcpy(out.f, g_stat, sizeof(g_stat));
    for (int i = 0; i < kFunctions; ++i) out.f[i].maxTotalNs = g_topMax[i];
    out.overflows = g_overflows;
}

void resetTopMax() {
    std::memset(g_topMax, 0, sizeof(g_topMax));
}

void snapshotRun(Totals& out) {
    snapshot(out);
    for (int i = 0; i < kFunctions; ++i) out.f[i].maxTotalNs = g_runMax[i];
}

void resetRunMax() {
    std::memset(g_runMax, 0, sizeof(g_runMax));
}

void resetSpikes() {
    g_spikeCount = 0;
    g_hasStaged = false;
}

int spikeCount() {
    return g_spikeCount;
}

const Spike& spike(int index) {
    return g_spikes[index];
}

void setSpikeFloorNs(std::uint64_t ns) {
    g_spikeFloorNs = ns;
}

std::uint64_t spikeFloorNs() {
    return g_spikeFloorNs;
}

} // namespace gamedll
