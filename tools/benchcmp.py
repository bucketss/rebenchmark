#!/usr/bin/env python3
"""Compare rebench reports.

    benchcmp.py before.json after.json
    benchcmp.py before/ after/
    benchcmp.py a1.json a2.json a3.json vs b1.json b2.json b3.json
    benchcmp.py --by-tag reports/

Each side may be several runs. Their histograms are pooled, so percentiles come
from every frame of every run on that side; per-run values give the spread that
decides whether a difference means anything.
"""

import argparse
import glob
import json
import math
import os
import re
import sys

# A difference has to clear this many pooled standard errors to be called real.
SIGMA = 2.0

# Percentiles worth comparing. p99.9 and max are in the report but not here:
# max is one frame and p99.9 is a handful, so both move on noise.
PERCENTILES = (50.0, 90.0, 99.0)

METADATA = (
    ("rebench version", lambda r: r.get("rebench")),
    ("os", lambda r: r.get("host", {}).get("os")),
    ("clock", lambda r: r.get("host", {}).get("clock")),
    ("sys_ticrate", lambda r: r.get("server", {}).get("sys_ticrate")),
    ("fps_max", lambda r: r.get("server", {}).get("fps_max")),
    ("maxplayers", lambda r: r.get("server", {}).get("maxplayers")),
    ("pingboost", lambda r: r.get("server", {}).get("pingboost")),
    ("stall detection", lambda r: r.get("settings", {}).get("stalls")),
    ("level 1 timing", lambda r: r.get("settings", {}).get("level_active")),
    ("plugin timing", lambda r: r.get("settings", {}).get("amxx_active")),
    ("engine phases", lambda r: r.get("settings", {}).get("engine_active")),
)


# --- reading ---------------------------------------------------------------

def load(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        report = json.load(handle)
    report["_path"] = path
    report["_name"] = os.path.basename(path)
    return report


def expand(paths):
    """Directories become their .json files; patterns are expanded here.

    Windows does not glob for us, and neither does a quoted argument anywhere
    else, so a pattern has to survive as far as this.
    """
    out = []
    for path in paths:
        if os.path.isdir(path):
            found = sorted(
                os.path.join(path, name)
                for name in os.listdir(path)
                if name.endswith(".json")
            )
            if not found:
                raise SystemExit(f"no .json reports in {path}")
            out.extend(found)
        elif any(c in path for c in "*?["):
            found = sorted(glob.glob(path))
            if not found:
                raise SystemExit(f"nothing matches {path}")
            out.extend(found)
        else:
            out.append(path)
    return out


def tag_stem(report):
    """"bots30_2" -> "bots30", so a scenario's repeats group together."""
    return re.sub(r"[_-]\d+$", "", report.get("tag", "?"))


def wall_s(report):
    """Wall time of a run.

    Function, Pawn and hook totals keep counting through map loads, which
    measured_s leaves out, so rates of those totals are per wall time.
    """
    return report.get("duration_s") or report.get("measured_s", 0.0)


def level_measured(report):
    settings = report.get("settings", {})
    if "level_active" in settings:
        return bool(settings["level_active"]) and bool(report.get("gamedll"))
    return bool(report.get("gamedll"))


def amxx_measured(report):
    """Plugin timing was on. The amxx section is left out when no Pawn ran."""
    settings = report.get("settings", {})
    if "amxx_active" in settings:
        return bool(settings["amxx_active"])
    return bool(report.get("amxx"))


def is_stalled(frame):
    """True when a frame's wall time was not spent running.

    Newer reports say so outright; older ones are judged the same way the
    plugin does, by the wall time the CPU counter did not account for. The
    margin scales with the frame, since a millisecond is the whole story in a
    2 ms frame and noise in a 400 ms one.
    """
    if "stalled" in frame:
        return bool(frame["stalled"])
    cpu = frame.get("cpu_ms")
    if cpu is None:
        return False
    busy = frame.get("busy_ms", 0.0)
    return busy - cpu > max(1.0, busy / 16.0)


# --- histograms ------------------------------------------------------------

class Pooled:
    """Buckets from several runs added together.

    The report stores [low, high, count] triples, so the bucket layout does not
    have to be reproduced here. Interpolation inside a bucket matches the
    plugin's own, including the clamp to the true min and max.
    """

    def __init__(self):
        self.buckets = {}
        self.count = 0
        self.low = None
        self.high = None

    def add(self, triples, stats):
        for low, high, count in triples:
            key = (low, high)
            self.buckets[key] = self.buckets.get(key, 0) + count
            self.count += count
        if stats:
            lo, hi = stats.get("min"), stats.get("max")
            if lo is not None:
                self.low = lo * 1e6 if self.low is None else min(self.low, lo * 1e6)
            if hi is not None:
                self.high = hi * 1e6 if self.high is None else max(self.high, hi * 1e6)

    def percentile(self, p):
        if not self.count:
            return None
        rank = p / 100.0 * self.count
        below = 0.0
        for (low, high), count in sorted(self.buckets.items()):
            if below + count >= rank:
                value = low + (rank - below) / count * (high - low)
                if self.low is not None:
                    value = max(value, self.low)
                if self.high is not None:
                    value = min(value, self.high)
                return value
            below += count
        return self.high

    def above(self, ns):
        """Frames slower than ns, interpolating the straddling bucket.

        Only for reports without exact counts. Nothing is past the known max.
        """
        if self.high is not None and ns >= self.high:
            return 0.0
        total = 0.0
        for (low, high), count in self.buckets.items():
            if low > ns:
                total += count
            elif high > ns + 1:
                total += count * (high - ns - 1) / (high - low)
        return total


# --- a side of the comparison ----------------------------------------------

class Side:
    def __init__(self, label, reports):
        self.label = label
        self.reports = reports
        self.frames = sum(r["frames"]["count"] for r in reports)
        self.seconds = sum(r["measured_s"] for r in reports)
        self.wall = sum(wall_s(r) for r in reports)
        self.stalled = sum(r["frames"].get("stalled", 0) for r in reports)
        self.hist = {}
        for key in ("period", "busy", "game", "engine", "cpu", "delta"):
            pooled = Pooled()
            for report in reports:
                triples = report.get("histograms", {}).get(key)
                if triples:
                    pooled.add(triples, report["frames"].get(key + "_ms"))
            if pooled.count:
                self.hist[key] = pooled

    @property
    def level_reports(self):
        return [r for r in self.reports if level_measured(r)]

    @property
    def amxx_reports(self):
        return [r for r in self.reports if amxx_measured(r)]

    @property
    def level(self):
        return bool(self.level_reports)

    @property
    def has_amxx(self):
        return bool(self.amxx_reports)

    @property
    def level_wall(self):
        return sum(wall_s(r) for r in self.level_reports)

    @property
    def amxx_wall(self):
        return sum(wall_s(r) for r in self.amxx_reports)

    def per_run(self, pick, reports=None):
        """A metric read from each run separately, for the spread."""
        values = []
        for report in self.reports if reports is None else reports:
            value = pick(report)
            if value is not None:
                values.append(value)
        return values

    def over(self, ms):
        """Frames with a period over ms, exact where every report counted them."""
        key = f"{ms}ms"
        if all(key in r["frames"].get("over", {}) for r in self.reports):
            return float(sum(r["frames"]["over"][key] for r in self.reports))
        if "period" not in self.hist:
            return None
        return self.hist["period"].above(ms * 1_000_000)

    def functions(self):
        """name -> (self_ms, calls) summed over the runs that timed them."""
        out = {}
        for report in self.level_reports:
            for func in report.get("gamedll", {}).get("functions", []):
                ms, calls = out.get(func["name"], (0.0, 0))
                out[func["name"]] = (ms + func["self_ms"], calls + func["calls"])
        return out

    def plugins(self):
        """"plugin function" -> (self_ms, calls) summed over the runs."""
        out = {}
        for report in self.amxx_reports:
            for func in report.get("amxx", {}).get("functions", []):
                key = f"{func['plugin']} {func['function']}"
                ms, calls = out.get(key, (0.0, 0))
                out[key] = (ms + func["self_ms"], calls + func["calls"])
        return out

    def stalled_spikes(self):
        return [spike for report in self.level_reports
                for spike in report.get("spikes", []) if is_stalled(spike)]

    def stall_ms(self):
        """name -> self ms of each function in logged stalled frames.

        This is the whole frame's time for every function in it, real work done
        before the stall included, so taking it out removes more than the stall.
        Unlogged stalls are not in it.
        """
        out = {}
        for spike in self.stalled_spikes():
            for entry in spike.get("top", []):
                out[entry["name"]] = out.get(entry["name"], 0.0) + entry["self_ms"]
        return out

    def startframe(self):
        """StartFrame total, the game DLL's share and the plugins' share, in ms.

        The game DLL share needs the Metamod bracket, which older reports do
        not have; it comes back None unless every run has it.
        """
        reports = self.level_reports
        if not reports:
            return None
        total = 0.0
        for report in reports:
            for func in report["gamedll"].get("functions", []):
                if func["name"] == "StartFrame":
                    total += func["total_ms"]
        if not total:
            return None
        if not all("startframe_inner_ms" in r["gamedll"] for r in reports):
            return total, None, None
        inner = sum(r["gamedll"]["startframe_inner_ms"] for r in reports)
        return total, inner, max(total - inner, 0.0)

    def spike_pawn(self):
        """plugin -> self ms in the Pawn breakdown of logged spikes."""
        out = {}
        for report in self.reports:
            for spike in report.get("spikes", []):
                for entry in spike.get("pawn", []):
                    out[entry["plugin"]] = out.get(entry["plugin"], 0.0) + entry["self_ms"]
        return out


def mean(values):
    return sum(values) / len(values) if values else 0.0


def stddev(values):
    if len(values) < 2:
        return 0.0
    m = mean(values)
    return math.sqrt(sum((v - m) ** 2 for v in values) / (len(values) - 1))


def per_minute(value, seconds):
    return value / seconds * 60.0 if seconds else 0.0


# --- formatting ------------------------------------------------------------

def change(base, new):
    if base is None or new is None:
        return "", 0.0
    if base == 0:
        return ("--" if new == 0 else "new"), 0.0
    pct = 100.0 * (new - base) / base
    return f"{pct:+.1f}%", pct


def significance(base_values, new_values, base, new):
    """Blank when there is one run a side, so no claim is made without evidence.

    With repeats, the difference between the numbers base and new has to clear
    SIGMA standard errors of the two run-to-run spreads combined.
    """
    if len(base_values) < 2 or len(new_values) < 2:
        return ""
    se = math.sqrt(
        stddev(base_values) ** 2 / len(base_values)
        + stddev(new_values) ** 2 / len(new_values)
    )
    if se == 0:
        return "*" if base != new else ""
    return "*" if abs(new - base) > SIGMA * se else "~"


def row(name, base, new, note="", decimals=4, unit="", width=20):
    delta, _ = change(base, new)
    b = "-" if base is None else f"{base:,.{decimals}f}{unit}"
    n = "-" if new is None else f"{new:,.{decimals}f}{unit}"
    if len(name) > width:
        name = name[:width - 1] + "…"
    return f"  {name:<{width}} {b:>13} {n:>13} {delta:>9}  {note}".rstrip()


def header(base, new, width=20):
    return f"  {'':<{width}} {base.label:>13} {new.label:>13} {'change':>9}"


# --- the report ------------------------------------------------------------

def describe(side):
    runs = len(side.reports)
    tags = sorted({r.get("tag", "?") for r in side.reports})
    shown = ", ".join(tags[:4]) + (", ..." if len(tags) > 4 else "")
    line = (f"{side.label:<8} {runs} run{'s' if runs != 1 else ''}, "
            f"{side.frames:,} frames, {side.seconds:,.1f} s measured of "
            f"{side.wall:,.1f} s  ({shown})")
    if side.stalled:
        line += f"\n{'':<9}{side.stalled} stalled frame(s): wall time the server did not spend running"
    maps = sorted({m for r in side.reports for m in r.get("maps", [])})
    if maps:
        line += f"\n{'':<9}maps: {', '.join(maps)}"
    return line


def compare_frames(out, base, new):
    out.append("frames")
    out.append(header(base, new))

    for name, key, decimals, unit in (
        ("fps", "fps", 1, ""),
        ("busy %", "busy_pct", 2, ""),
    ):
        b = base.per_run(lambda r: r["frames"].get(key))
        n = new.per_run(lambda r: r["frames"].get(key))
        out.append(row(name, mean(b), mean(n),
                       significance(b, n, mean(b), mean(n)), decimals, unit))

    for key, label in (("period", "period"), ("busy", "busy"),
                       ("game", "game"), ("engine", "engine"), ("cpu", "cpu")):
        if key not in base.hist or key not in new.hist:
            continue
        for p in PERCENTILES:
            bp = base.hist[key].percentile(p)
            np_ = new.hist[key].percentile(p)
            # Spread across runs, from each run's own stats block.
            field = f"p{int(p)}"
            b = base.per_run(lambda r, k=key, f=field: (r["frames"].get(k + "_ms") or {}).get(f))
            n = new.per_run(lambda r, k=key, f=field: (r["frames"].get(k + "_ms") or {}).get(f))
            b = [v * 1e6 for v in b]
            n = [v * 1e6 for v in n]
            out.append(row(f"{label} p{p:g} ms", bp / 1e6, np_ / 1e6,
                           significance(b, n, bp, np_), 4))

    out.append("")
    out.append("slow frames per minute of measured time")
    out.append(header(base, new))
    for ms in (5, 10, 20, 50, 100):
        b, n = base.over(ms), new.over(ms)
        if b is None or n is None:
            break
        out.append(row(f"over {ms} ms", per_minute(b, base.seconds),
                       per_minute(n, new.seconds), "", 2))


def compare_run(out, base, new):
    """rebench's own cost and the map loads inside the runs."""
    def overhead(side):
        values = [(r["instrumentation"]["total_ms"], wall_s(r))
                  for r in side.reports if "instrumentation" in r]
        if not values:
            return None
        return per_minute(sum(v for v, _ in values), sum(s for _, s in values))

    def loads(side):
        changes = [c for r in side.reports for c in r.get("map_changes", [])]
        return len(changes), (mean([c["load_ms"] for c in changes]) if changes else None)

    b_over, n_over = overhead(base), overhead(new)
    (b_count, b_load), (n_count, n_load) = loads(base), loads(new)
    if b_over is None and n_over is None and not b_count and not n_count:
        return
    out.append("")
    out.append("run")
    out.append(header(base, new))
    if b_over is not None or n_over is not None:
        out.append(row("rebench ms/minute", b_over, n_over, "a floor", 2))
    if b_count or n_count:
        out.append(row("map changes", float(b_count), float(n_count), "", 0))
        out.append(row("map load ms", b_load, n_load, "mean", 1))


def compare_engine(out, base, new):
    # A per-call cost is already a rate and is averaged; everything else is a
    # count or a total over the run, and is scaled to a minute of wall time.
    RATES = ("delta_us_per_call", "edict_alloc_us_per_call", "edict_free_us_per_call")
    keys = [
        ("delta calls", "delta_calls", 0),
        ("delta ms", "delta_ms", 1),
        ("delta us/call", "delta_us_per_call", 3),
        ("delta KB", None, 1),
        ("edict allocs", "edict_allocs", 0),
        ("edict alloc ms", "edict_alloc_ms", 2),
        ("edict alloc us/call", "edict_alloc_us_per_call", 3),
        ("edict frees", "edict_frees", 0),
        ("edict free ms", "edict_free_ms", 2),
        ("edict free us/call", "edict_free_us_per_call", 3),
        ("alloc failures", "edict_alloc_failures", 0),
    ]
    if not any(r.get("engine") for r in base.reports + new.reports):
        return
    out.append("")
    out.append("engine  (per minute of wall time)")
    out.append(header(base, new))
    for label, key, decimals in keys:
        def pick(report, key=key):
            engine = report.get("engine")
            if not engine:
                return None
            if key is None:
                raw = engine.get("delta_bytes")
                return None if raw is None else raw / 1024.0
            return engine.get(key)
        b = base.per_run(pick)
        n = new.per_run(pick)
        if not b or not n:
            continue
        if key in RATES:
            bv, nv = mean(b), mean(n)
        else:
            bv = per_minute(sum(b), base.wall)
            nv = per_minute(sum(n), new.wall)
        out.append(row(label, bv, nv, "", decimals))


def compare_startframe(out, base, new):
    b = base.startframe()
    n = new.startframe()
    if not b or not n:
        return
    out.append("")
    out.append("StartFrame  (ms per minute of wall time)")
    out.append(header(base, new))
    labels = ("total", "game DLL", "other plugins")
    for label, bv, nv in zip(labels, b, n):
        out.append(row(label,
                       None if bv is None else per_minute(bv, base.level_wall),
                       None if nv is None else per_minute(nv, new.level_wall), "", 1))


def compare_functions(out, base, new, rows, exclude_stalls):
    if not base.level or not new.level:
        return
    b = base.functions()
    n = new.functions()
    if not b and not n:
        return

    b_stall = base.stall_ms() if exclude_stalls else {}
    n_stall = new.stall_ms() if exclude_stalls else {}
    excluded = set()

    names = set(b) | set(n)
    ranked = []
    for name in names:
        braw = b.get(name, (0.0, 0))[0]
        nraw = n.get(name, (0.0, 0))[0]
        bms = per_minute(braw - b_stall.get(name, 0.0), base.level_wall)
        nms = per_minute(nraw - n_stall.get(name, 0.0), new.level_wall)
        if b_stall.get(name, 0.0) > 0.02 * braw or n_stall.get(name, 0.0) > 0.02 * nraw:
            excluded.add(name)
        ranked.append((abs(nms - bms), name, bms, nms))
    # Biggest absolute movers first; a 300% swing on 0.01 ms is not news.
    ranked.sort(reverse=True)

    out.append("")
    title = f"game DLL self time  (ms per minute of wall time, {len(ranked)} functions"
    if exclude_stalls:
        title += ", logged stalled frames excluded"
    out.append(title + ")")
    out.append(header(base, new))
    for _, name, bms, nms in ranked[:rows]:
        bcalls = b.get(name, (0.0, 0))[1]
        ncalls = n.get(name, (0.0, 0))[1]
        notes = []
        if bcalls and ncalls:
            call_delta, pct = change(per_minute(bcalls, base.level_wall),
                                     per_minute(ncalls, new.level_wall))
            if abs(pct) >= 5.0:
                notes.append(f"calls {call_delta}")
        if name in excluded:
            notes.append("stalls excluded")
        out.append(row(name, bms, nms, " ".join(notes), 1))
    if len(ranked) > rows:
        out.append(f"  {len(ranked) - rows} smaller movers not shown")


# "plugin.amxx some_forward" needs more room than a game DLL function name.
PLUGIN_WIDTH = 38


def compare_plugins(out, base, new, rows):
    if not base.has_amxx or not new.has_amxx:
        return
    b = base.plugins()
    n = new.plugins()
    if not b and not n:
        return

    def total(side):
        return sum(r.get("amxx", {}).get("total_ms", 0.0) for r in side.amxx_reports)

    def rate(report):
        seconds = wall_s(report)
        return per_minute(report.get("amxx", {}).get("total_ms", 0.0), seconds) if seconds else None

    btotal = per_minute(total(base), base.amxx_wall)
    ntotal = per_minute(total(new), new.amxx_wall)

    out.append("")
    out.append("time in Pawn  (ms per minute of wall time; part of the game DLL "
               "table above, not extra)")
    out.append(header(base, new, PLUGIN_WIDTH))
    out.append(row("total", btotal, ntotal,
                   significance(base.per_run(rate, base.amxx_reports),
                                new.per_run(rate, new.amxx_reports),
                                btotal, ntotal), 1, width=PLUGIN_WIDTH))

    ranked = []
    for key in set(b) | set(n):
        bms = per_minute(b.get(key, (0.0, 0))[0], base.amxx_wall)
        nms = per_minute(n.get(key, (0.0, 0))[0], new.amxx_wall)
        ranked.append((abs(nms - bms), key, bms, nms))
    ranked.sort(reverse=True)

    for _, key, bms, nms in ranked[:rows]:
        bcalls = b.get(key, (0.0, 0))[1]
        ncalls = n.get(key, (0.0, 0))[1]
        note = ""
        if bcalls and ncalls:
            call_delta, pct = change(per_minute(bcalls, base.amxx_wall),
                                     per_minute(ncalls, new.amxx_wall))
            if abs(pct) >= 5.0:
                note = f"calls {call_delta}"
        out.append(row(key, bms, nms, note, 1, width=PLUGIN_WIDTH))
    if len(ranked) > rows:
        out.append(f"  {len(ranked) - rows} smaller movers not shown")


def compare_spike_pawn(out, base, new, rows):
    b = base.spike_pawn()
    n = new.spike_pawn()
    if not b and not n:
        return
    ranked = sorted(set(b) | set(n), key=lambda k: -max(b.get(k, 0.0), n.get(k, 0.0)))
    out.append("")
    out.append("Pawn in logged spikes  (ms, summed over the spike logs)")
    out.append(header(base, new, PLUGIN_WIDTH))
    for key in ranked[:rows]:
        out.append(row(key, b.get(key, 0.0), n.get(key, 0.0), "", 2, width=PLUGIN_WIDTH))
    if len(ranked) > rows:
        out.append(f"  {len(ranked) - rows} more not shown")


def warnings(base, new, exclude_stalls):
    out = []
    for side in (base, new):
        worst = [r["frames"]["worst"] for r in side.reports
                 if is_stalled(r["frames"].get("worst", {}))]
        if worst:
            top = max(worst, key=lambda w: w.get("busy_ms", 0.0))
            out.append(
                f"{side.label}: worst frame is {top['busy_ms']:.1f} ms of wall time but "
                f"only {top.get('cpu_ms', 0.0):.2f} ms of CPU -- a stall, not work. That "
                f"side's mean, stddev and max include it, and so does p99 if stalls "
                f"are common.")
        stalled = side.stalled_spikes()
        stall = side.stall_ms()
        if stalled and stall:
            ms = sum(stall.values())
            top = max(stall.items(), key=lambda kv: kv[1])
            if exclude_stalls:
                out.append(
                    f"{side.label}: excluded {ms:,.0f} ms of function time from "
                    f"{len(stalled)} logged stalled frame(s), {top[1]:,.0f} ms of it off "
                    f"{top[0]}. That is each function's whole frame, real work included; "
                    f"stalls outside the spike log stay in.")
            else:
                out.append(
                    f"{side.label}: {len(stalled)} logged stalled frame(s) hold "
                    f"{ms:,.0f} ms of function time, {top[1]:,.0f} ms of it in {top[0]}. "
                    f"--exclude-stalls takes those frames out.")
        for what, reports in (("game DLL timing", side.level_reports),
                              ("plugin timing", side.amxx_reports)):
            if reports and len(reports) != len(side.reports):
                out.append(f"{side.label}: {what} in {len(reports)} of {len(side.reports)} "
                           f"runs; its rates use only those runs' time")

    hosts = {r["host"]["cpu"] for r in base.reports + new.reports}
    if len(hosts) > 1:
        out.append("runs come from different CPUs: " + "; ".join(sorted(hosts)))
    for name, pick in METADATA:
        values = {json.dumps(pick(r)) for r in base.reports + new.reports if pick(r) is not None}
        if len(values) > 1:
            out.append(f"runs differ in {name}: " + "; ".join(sorted(values)))
    for side in (base, new):
        maps = {m for r in side.reports for m in r.get("maps", [])}
        other = {m for r in (new if side is base else base).reports for m in r.get("maps", [])}
        if maps and other and maps != other:
            out.append("the two sides did not run the same maps; the comparison is weaker")
            break
    if len(base.reports) < 2 or len(new.reports) < 2:
        out.append("one run a side: no spread to judge against, so nothing is marked "
                   "significant. Repeat each side 3 times for that.")
    return out


def render(base, new, rows, exclude_stalls=False):
    out = []
    out.append("rebench comparison")
    out.append("  " + describe(base).replace("\n", "\n  "))
    out.append("  " + describe(new).replace("\n", "\n  "))
    out.append("")
    compare_frames(out, base, new)
    compare_run(out, base, new)
    compare_engine(out, base, new)
    compare_startframe(out, base, new)
    compare_functions(out, base, new, rows, exclude_stalls)
    compare_plugins(out, base, new, rows)
    compare_spike_pawn(out, base, new, rows)

    notes = warnings(base, new, exclude_stalls)
    if notes:
        out.append("")
        out.append("notes")
        for note in notes:
            out.append("  - " + note)
    if any(len(s.reports) > 1 for s in (base, new)):
        out.append("")
        out.append(f"  * beyond {SIGMA:g} sigma of the run-to-run spread   ~ within it")
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(
        description="Compare rebench reports.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    parser.add_argument("paths", nargs="+",
                        help="reports or directories; 'vs' separates the two sides")
    parser.add_argument("--by-tag", action="store_true",
                        help="group one directory by tag and compare the first two groups")
    parser.add_argument("--rows", type=int, default=15,
                        help="game DLL functions to show (default 15)")
    parser.add_argument("--exclude-stalls", action="store_true",
                        help="take logged stalled frames out of the game DLL table")
    parser.add_argument("--json", action="store_true", help="machine readable output")
    args = parser.parse_args()

    if args.by_tag:
        reports = [load(p) for p in expand(args.paths)]
        groups = {}
        for report in reports:
            groups.setdefault(tag_stem(report), []).append(report)
        if len(groups) < 2:
            raise SystemExit(f"need two tags to compare, found: {', '.join(groups) or 'none'}")
        # Oldest group first, so the case that ran first is the baseline.
        # Alphabetical would make "after" the baseline of "before".
        names = sorted(groups, key=lambda tag: min(r.get("started", "") for r in groups[tag]))
        if len(names) > 2:
            print(f"comparing {names[0]} against {names[1]}; also present: "
                  f"{', '.join(names[2:])}. Name two explicitly to pick a different "
                  f"pair.\n", file=sys.stderr)
        sides = [Side(names[0], groups[names[0]]), Side(names[1], groups[names[1]])]
    else:
        if "vs" in args.paths:
            cut = args.paths.index("vs")
            left, right = args.paths[:cut], args.paths[cut + 1:]
        elif len(args.paths) == 2:
            left, right = args.paths[:1], args.paths[1:]
        else:
            raise SystemExit("give two reports, two directories, or 'A... vs B...'")
        if not left or not right:
            raise SystemExit("both sides need at least one report")
        groups = [[load(p) for p in expand(side)] for side in (left, right)]
        sides = [Side("base", groups[0]), Side("new", groups[1])]

    if args.json:
        json.dump({
            side.label: {
                "runs": len(side.reports),
                "frames": side.frames,
                "seconds": side.seconds,
                "wall_seconds": side.wall,
                "stalled": side.stalled,
                "percentiles_ms": {
                    key: {f"p{p:g}": side.hist[key].percentile(p) / 1e6 for p in PERCENTILES}
                    for key in side.hist
                },
                "functions_ms": {k: v[0] for k, v in side.functions().items()},
            }
            for side in sides
        }, sys.stdout, indent=2)
        print()
    else:
        print(render(sides[0], sides[1], args.rows, args.exclude_stalls))


if __name__ == "__main__":
    main()
