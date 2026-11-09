#!/usr/bin/env python3
"""Drive rebench runs over rcon, repeatably.

    scenario.py scenarios/baseline.json
    scenario.py scenarios/baseline.json --password SECRET
    scenario.py scenarios/baseline.json --dry-run

A scenario is a list of cases. Each case sets up the server, waits out a warmup,
records a run, and repeats. Reports land in the plugin's own reports directory
tagged `<case>_<n>`, which is what benchcmp --by-tag groups on.

The point is that a difference between two cases is attributable to the change
rather than to whatever the server happened to be doing. Bots wander, rounds
end, players join: only repetition separates a real difference from that.
"""

import argparse
import json
import os
import re
import socket
import sys
import time

TIMEOUT = 5.0
REPLY_GAP = 0.3
MAX_TAG = 32
STOP_GRACE = 5.0


# --- rcon ------------------------------------------------------------------

class Rcon:
    """GoldSrc rcon. One socket, re-challenged per command.

    The challenge reply can carry a trailing NUL, so it is matched by pattern
    rather than split on whitespace; splitting yields b"123456\\x00" and the
    server answers "Bad rcon_password", which sends you after the wrong problem.
    """

    def __init__(self, host, port, password):
        self.address = (host, port)
        self.password = password

    def _socket(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(TIMEOUT)
        return sock

    def challenge(self, sock):
        sock.sendto(b"\xff\xff\xff\xffchallenge rcon\n", self.address)
        data, _ = sock.recvfrom(4096)
        match = re.search(rb"challenge rcon (\d+)", data)
        if not match:
            raise RuntimeError(f"bad challenge reply: {data!r}")
        return match.group(1)

    def __call__(self, command):
        sock = self._socket()
        try:
            token = self.challenge(sock)
            packet = (b"\xff\xff\xff\xffrcon " + token + b" " +
                      self.password.encode("ascii", "ignore") + b" " +
                      command.encode("ascii", "ignore") + b"\n")
            sock.sendto(packet, self.address)
            reply = b""
            while True:
                try:
                    chunk, _ = sock.recvfrom(4096)
                    reply += chunk
                    sock.settimeout(REPLY_GAP)
                except socket.timeout:
                    break
        finally:
            sock.close()
        return reply.replace(b"\xff\xff\xff\xffl", b"").decode("utf-8", "replace")


# --- scenario --------------------------------------------------------------

DEFAULTS = {
    "host": "127.0.0.1",
    "port": 27015,
    "duration": 120,
    "warmup": 30,
    "repeat": 3,
    "level": 1,
    "settle": 5,
    "spike_floor_ms": None,
    "map": None,
    "reports": None,
}


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and value == value \
        and value not in (float("inf"), float("-inf"))


def check_settings(where, settings):
    """Types and ranges of the keys a scenario or a case may set."""
    def fail(key, want):
        raise SystemExit(f"{where}: {key} must be {want}, not {settings[key]!r}")

    if "port" in settings and (not isinstance(settings["port"], int) or isinstance(settings["port"], bool)
                               or not 0 < settings["port"] < 65536):
        fail("port", "a port number")
    if "duration" in settings and (not is_number(settings["duration"]) or settings["duration"] <= 0):
        fail("duration", "a positive number of seconds")
    for key in ("warmup", "settle"):
        if key in settings and (not is_number(settings[key]) or settings[key] < 0):
            fail(key, "zero or more seconds")
    if "repeat" in settings and (not isinstance(settings["repeat"], int) or isinstance(settings["repeat"], bool)
                                 or settings["repeat"] < 1):
        fail("repeat", "a whole number of at least 1")
    if "level" in settings and settings["level"] not in (0, 1, "run"):
        fail("level", "0, 1 or \"run\"")
    floor = settings.get("spike_floor_ms")
    if floor is not None and (not is_number(floor) or floor <= 0):
        fail("spike_floor_ms", "a positive number of ms")


def load(path):
    with open(path, "r", encoding="utf-8") as handle:
        scenario = json.load(handle)
    for key, value in DEFAULTS.items():
        scenario.setdefault(key, value)
    check_settings(path, scenario)
    if not scenario.get("cases"):
        raise SystemExit(f"{path}: no cases")
    seen = set()
    for n, case in enumerate(scenario["cases"]):
        if "name" not in case:
            raise SystemExit(f"{path}: case {n} has no name")
        name = case["name"]
        check_settings(f"{path}: case {name!r}", case)
        if not re.fullmatch(r"[A-Za-z0-9_-]+", name):
            raise SystemExit(f"{path}: case name {name!r} must be "
                             f"letters, digits, _ or -")
        # The tag suffix is what groups repeats, so the name must not end in a
        # number of its own -- "bots30" is fine, "bots_30" would group wrong.
        if re.search(r"[_-]\d+$", name):
            raise SystemExit(f"{path}: case name {name!r} ends in _<number>, "
                             f"which is how repeats are numbered; rename it")
        longest = MAX_TAG - len(f"_{setting(scenario, case, 'repeat')}")
        if len(name) > longest:
            raise SystemExit(f"{path}: case name {name!r} is longer than {longest} "
                             f"characters, and the plugin would cut its repeat number off")
        if name in seen:
            raise SystemExit(f"{path}: two cases are named {name!r}")
        seen.add(name)
    return scenario


def setting(scenario, case, key):
    return case.get(key, scenario[key])


def refused(reply):
    """A reply that means the server did not run the command at all."""
    return re.search(r"Bad rcon_password|Unknown command|You have been banned", reply)


class Runner:
    def __init__(self, rcon, scenario, dry_run=False, verbose=False):
        self.rcon = rcon
        self.scenario = scenario
        self.dry_run = dry_run
        self.verbose = verbose
        self.tags = []
        self.active = None
        self.level = None

    def send(self, command, show=False):
        if self.dry_run:
            print(f"    rcon  {command}")
            return ""
        reply = self.rcon(command).strip()
        if refused(reply):
            raise RuntimeError(f"{command!r}: {reply.splitlines()[0]}")
        if (self.verbose or show) and reply:
            for line in reply.splitlines():
                print(f"      | {line}")
        return reply

    def wait(self, seconds, what):
        if self.dry_run:
            print(f"    wait  {seconds}s ({what})")
            return
        end = time.monotonic() + seconds
        while True:
            left = end - time.monotonic()
            if left <= 0:
                break
            print(f"\r    {what}: {left:5.0f}s left ", end="", flush=True)
            time.sleep(min(1.0, left))
        print(f"\r    {what}: done{' ' * 12}")

    def read_level(self):
        """"0", "1" or "run", from what bench level says with no argument."""
        reply = self.send("bench level")
        if self.dry_run:
            return "run"
        if "pinned" in reply:
            return "1" if "level 1" in reply else "0"
        return "run"

    def setup(self, case):
        if setting(self.scenario, case, "map"):
            target = setting(self.scenario, case, "map")
            self.send(f"changelevel {target}")
            # A map change takes the server away for a moment; nothing measured
            # until it is back.
            self.wait(setting(self.scenario, case, "settle"), f"loading {target}")
        for name, value in case.get("cvars", {}).items():
            self.send(f"{name} {value}")
        for command in case.get("commands", []):
            if not self.dry_run:
                print(f"    {command}")
            self.send(command, show=True)
        self.send(f"bench level {setting(self.scenario, case, 'level')}")
        floor = setting(self.scenario, case, "spike_floor_ms")
        if floor is not None:
            self.send(f"bench spikes floor {floor}")

    def record(self, tag, duration):
        reply = self.send(f"bench start {tag} {duration}")
        if not self.dry_run and f'recording "{tag}"' not in reply:
            raise RuntimeError(f"bench start {tag} did not start a run: {reply or 'no reply'}")
        self.active = tag
        self.wait(duration, f"recording {tag}")
        if self.dry_run:
            self.active = None
            self.tags.append(tag)
            return
        deadline = time.monotonic() + STOP_GRACE
        while True:
            status = self.send("bench status")
            if "not recording" in status:
                break
            if f'recording "{tag}"' not in status:
                raise RuntimeError(f"bench status after {tag}: {status or 'no reply'}")
            if time.monotonic() >= deadline:
                print("    warning: still recording after the limit; stopping it")
                self.send("bench stop")
                break
            time.sleep(1.0)
        self.active = None
        self.tags.append(tag)

    def case(self, case):
        repeat = setting(self.scenario, case, "repeat")
        duration = setting(self.scenario, case, "duration")
        warmup = setting(self.scenario, case, "warmup")
        print(f"\n  case {case['name']}: {repeat} x {duration}s"
              + (f", {warmup}s warmup" if warmup else ""))
        if case.get("note"):
            print(f"    {case['note']}")
        self.setup(case)
        if warmup:
            self.wait(warmup, "warmup")
        for n in range(1, repeat + 1):
            self.record(f"{case['name']}_{n}", duration)

    def run(self):
        scenario = self.scenario
        print(f"scenario {scenario.get('name', '?')} "
              f"on {scenario['host']}:{scenario['port']}")
        total = sum(
            setting(scenario, c, "warmup") +
            setting(scenario, c, "repeat") * (setting(scenario, c, "duration") + 1) +
            (setting(scenario, c, "settle") if setting(scenario, c, "map") else 0)
            for c in scenario["cases"])
        print(f"{len(scenario['cases'])} cases, about {total / 60.0:.0f} minutes")
        if not self.dry_run:
            info = self.send("bench info")
            if "rebench" not in info:
                raise SystemExit(
                    "the server did not answer 'bench info'. Check the rcon password, "
                    "and that rebench is loaded (meta list).")
        self.level = self.read_level()
        for case in scenario["cases"]:
            self.case(case)

    def cleanup(self):
        """Best effort, each step on its own: stop, level, teardown."""
        steps = []
        if self.active:
            steps.append(("stop the run still going", "bench stop"))
        if self.level is not None:
            steps.append((f"put the level back to {self.level}", f"bench level {self.level}"))
        for command in self.scenario.get("teardown", []):
            steps.append((command, command))
        failed = []
        for what, command in steps:
            try:
                self.send(command)
            except Exception as error:
                failed.append(f"{what}: {error}")
        self.active = None
        return failed


def report_dir(scenario, args):
    for candidate in (args.reports, scenario.get("reports")):
        if candidate:
            return candidate
    return None


def main():
    parser = argparse.ArgumentParser(
        description="Drive rebench runs over rcon.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    parser.add_argument("scenario", help="scenario .json")
    parser.add_argument("--password", help="rcon password "
                        "(default: the scenario's, else $RCON_PASSWORD)")
    parser.add_argument("--host", help="override the scenario's host")
    parser.add_argument("--port", type=int, help="override the scenario's port")
    parser.add_argument("--reports", help="report directory, for the benchcmp hint")
    parser.add_argument("--dry-run", action="store_true",
                        help="print what would be sent, touch nothing")
    parser.add_argument("--verbose", action="store_true", help="echo rcon replies")
    args = parser.parse_args()

    scenario = load(args.scenario)
    if args.host:
        scenario["host"] = args.host
    if args.port:
        scenario["port"] = args.port

    password = (args.password or scenario.get("rcon_password")
                or os.environ.get("RCON_PASSWORD", ""))
    if not password and not args.dry_run:
        raise SystemExit("no rcon password: --password, the scenario's "
                         "rcon_password, or $RCON_PASSWORD")

    rcon = Rcon(scenario["host"], scenario["port"], password)
    runner = Runner(rcon, scenario, args.dry_run, args.verbose)
    started = time.monotonic()
    result = 0
    try:
        runner.run()
    except KeyboardInterrupt:
        print("\ninterrupted")
        result = 1
    except Exception as error:
        print(f"\nfailed: {error}")
        result = 1
    finally:
        failed = runner.cleanup()
        for line in failed:
            print(f"  cleanup could not {line}")

    if result:
        if runner.tags:
            print(f"completed before stopping: {', '.join(runner.tags)}")
        return result

    print(f"\ndone in {(time.monotonic() - started) / 60.0:.1f} minutes, "
          f"{len(runner.tags)} runs: {', '.join(runner.tags)}")
    directory = report_dir(scenario, args)
    cases = [c["name"] for c in scenario["cases"]]
    here = os.path.dirname(os.path.abspath(__file__))
    benchcmp = os.path.join(here, "benchcmp.py")
    if not directory:
        print("\ncompare with benchcmp.py \"<reports>/<case>_*.json\" vs "
              "\"<reports>/<other case>_*.json\", or pass --reports for the exact command")
    elif len(cases) == 1:
        # One case measures nothing on its own. It is a baseline: change
        # something, run it again under another name, then compare the two.
        print(f"\nthat is a baseline, not a comparison. Change what you want to "
              f"test, rename the case, run it again, then:\n"
              f'  "{sys.executable}" "{benchcmp}" '
              f'"{directory}/{cases[0]}_*.json" vs "{directory}/<the new case>_*.json"')
    else:
        prefix = "compare with" if len(cases) == 2 else f'compare any two of {", ".join(cases)} with'
        print(f'\n{prefix}:\n'
              f'  "{sys.executable}" "{benchcmp}" '
              f'"{directory}/{cases[0]}_*.json" vs "{directory}/{cases[1]}_*.json"')
        print("  (reports from an earlier run of the same case names match too; move "
              "them aside first)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
