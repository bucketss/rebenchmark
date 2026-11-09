# rebench_mm

Metamod plugin that measures server frame timing on ReHLDS. Runs can be recorded to JSON
for comparison.

Requires ReHLDS with API 3.10 or newer.

## Commands

Server console / rcon:

- `bench status`: frame timing over the last ~10 s, player counts, run state
- `bench start [tag] [seconds]`: start recording a run. With `seconds` it stops
  itself; `bench start 300` is a 300 s run tagged `run`
- `bench stop`: end the run, print a summary, write the report
- `bench level [0|1|run]`: 1 also times every game DLL entry point; `run`
  (default) is 1 while recording, 0 otherwise. `0` and `1` are refused while
  recording
- `bench top [rows|reset]`: game DLL functions by self time, since the level was
  turned on or last reset
- `bench spikes [rows|reset]`: the worst frames and what was in them
- `bench spikes floor <ms>`: log only frames busier than this
- `bench micro [name]`: cost of one operation, hammered in a loop
- `bench amxx [on|off|run|reset|rows]`: time in Pawn, by plugin and function;
  `run` (default) times only while recording
- `bench engine [on|off|run|reset|rows]`: where the engine's own frame time
  went, by phase; needs the engine probe patch, `run` (default) marks only
  while recording
- `bench clock [auto|tsc|os]`: which timer to measure with; no argument reports
  the current one and the cross-check
- `bench stalls [on|off]`: read thread CPU time each frame to tell a frame the
  host descheduled from one the game spent. On by default
- `bench info`: clock source and rate, CPU, OS, ReHLDS version, report directory
- `bench unload`: queue `meta unload` for this plugin

Tools:

    python tools/scenario.py tools/scenarios/plugincost.json --password SECRET
    python tools/benchcmp.py before.json after.json

## Build

32-bit, headers vendored in `sdk/`.

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build

Builds two binaries: `rebench_mm` for Metamod and `rebench_amxx` for AMXX.

Windows needs an x86 MSVC environment (`vcvars32.bat`). Linux needs
`g++-multilib` and `libc6-dev-i386` and produces `rebench_mm_i386.so`.

Install by adding `rebench_mm` to `addons/metamod/plugins.ini`, last. For
per-plugin AMXX timing, put `rebench_amxx` in `addons/amxmodx/modules/` and add
`rebench` to `modules.ini`.
