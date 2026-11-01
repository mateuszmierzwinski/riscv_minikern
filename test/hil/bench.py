#!/usr/bin/env python3
"""The board benchmark suite: every figure in docs/BENCHMARKS.md, measured
on the board, kept, and compared with what was measured before.

    python3 test/hil/bench.py                 # all of them
    python3 test/hil/bench.py -k nvme -k sd   # some of them
    python3 test/hil/bench.py --dry-run       # what would run, and the history

Each measure is one or more boots through `board.py`; each reading is pulled
from the console by the same expression docs/BENCHMARKS.md gives a person.
The readings go to `docs/benchmarks.tsv` (one row per reading: when, the
monorepo commit, the measure, the value) — **appended, never rewritten**, for
the same reason the table in BENCHMARKS.md is a history rather than a score.

A measure **regresses** when the median of this run is worse than the median
of every earlier reading of it by more than its tolerance. The tolerance is
the measure's own run-to-run spread on this board, recorded beside it, not a
round number: the Go speedup has been seen anywhere from 1.9x to 3.7x on one
build, so a tolerance tighter than that would report noise as a regression,
and one wider would hide one. The two counters that must never move (missed
pokes, lost deadlines) have no tolerance — any non-zero reading fails.

A measure that produced no reading **fails** rather than being skipped: a
benchmark whose output stopped parsing is the commonest way a suite like
this goes quiet while looking green.

The disks are only read. `minikern.nomount` attaches the device and mounts
nothing on it, so nothing can be written there, and `minikern.nodisklog`
keeps the kernel log off it, because the board's NVMe drive holds the
operator's data.
"""
import argparse
import csv
import datetime
import os
import re
import statistics
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.abspath(os.path.join(HERE, "..", ".."))
HISTORY = os.path.join(PROJECT, "docs", "benchmarks.tsv")
GO_PROGRAM = "//src/userland/gotest:gotest"
READ_ONLY = "minikern.nomount minikern.nodisklog"


class Measure:
    """One figure: how to boot for it, how to read it, and how it may move.

    `better` is +1 when higher is better and -1 when lower is; `tolerance`
    is the fraction of the earlier median a run may lose before it counts
    as a regression, and None for a counter that must read zero — or, with
    `ceiling`, stay at or under it."""

    def __init__(self, key, bootargs, pattern, better, tolerance, runs=1, seconds=110,
                 program=None, ceiling=None):
        self.key = key
        self.bootargs = bootargs
        self.pattern = re.compile(pattern)
        self.better = better
        self.tolerance = tolerance
        self.runs = runs
        self.seconds = seconds
        self.program = program
        self.ceiling = ceiling  # a fixed limit instead of a comparison with the past

    def readings(self, text):
        """Every value the pattern finds in one boot's console."""
        return [float(m.group(1)) for m in self.pattern.finditer(text)]


class SpinnerHart(Measure):
    """Timer interrupts on the spinner's hart **during the spin**: the hart
    with a switch on the `smp: switches/ticks by hart` line (which hart it
    lands on varies), looked up on the `spin: timer interrupts in the window`
    line. The `smp:` counts run from boot, so they fold the boot hart's
    periodic start-up and the idle harts' backstops into a figure meant for
    one busy thread — reading them is what made this ~400 instead of 0."""

    def readings(self, text):
        switches = re.search(r"smp: switches/ticks by hart:[^\n]*", text)
        window = re.search(r"spin: timer interrupts in the window by hart:[^\n]*", text)
        if switches is None or window is None:
            return []
        busy = max((int(n), hart) for hart, n in re.findall(r" (\d+)=(\d+)/", switches.group(0)))[1]
        counts = dict(re.findall(r" (\d+)=(\d+)", window.group(0)))
        return [float(counts[busy])] if busy in counts else []


# The spreads behind the tolerances are the ones docs/BENCHMARKS.md records:
# Go 1.9–3.7x across runs of one build (a median of five moves far less, so
# 25 % rather than the full range), and storage within ±3 % between rows.
MEASURES = [
    Measure("go_speedup", "", r"speedup ([0-9.]+)x", +1, 0.25, runs=5, program=GO_PROGRAM),
    Measure("nvme_kib_s", f"minikern.disk=nvme {READ_ONLY} minikern.diskbench=64:16",
            r"bench: \d+ KiB in \d+ ms, (\d+) KiB/s, 16 blocks", +1, 0.10),
    Measure("sd_kib_s", f"minikern.disk=sd {READ_ONLY} minikern.diskbench=16:16",
            r"bench: \d+ KiB in \d+ ms, (\d+) KiB/s, 16 blocks", +1, 0.10),
    # Dynamic ticks leave a hart running one thread alone: 0 in the window on
    # the board, 1,503 with `minikern.periodic=1`. A ceiling rather than an
    # exact zero, because the beat may land on a busy hart now and then.
    SpinnerHart("spin_ticks_busy_hart", "minikern.spin=1", r"unused", -1, None, ceiling=5),
    Measure("missed_pokes", "minikern.spin=1", r"sched: (\d+) missed pokes", -1, None),
    Measure("lost_deadlines", "minikern.spin=1", r"(\d+) lost deadlines", -1, None),
]


def commit():
    """The monorepo commit the tree is at, marked `+` when it has changes."""
    def git(*args):
        return subprocess.run(["git", *args], cwd=PROJECT, capture_output=True,
                              text=True).stdout.strip()
    head = git("rev-parse", "--short=9", "HEAD") or "unknown"
    return head + ("+" if git("status", "--porcelain", "--", ".") else "")


def history():
    """Earlier readings, by measure key."""
    out = {}
    if not os.path.exists(HISTORY):
        return out
    with open(HISTORY, newline="") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            out.setdefault(row["measure"], []).append(float(row["value"]))
    return out


def record(rows):
    """Appends this run's readings; writes the header on a new file."""
    new = not os.path.exists(HISTORY)
    with open(HISTORY, "a", newline="") as f:
        w = csv.writer(f, delimiter="\t", lineterminator="\n")
        if new:
            w.writerow(["when", "commit", "measure", "value"])
        w.writerows(rows)


def built(target):
    """Builds a target and returns its output path (suite.py's rule:
    `cquery` alone does not build)."""
    subprocess.run(["bazel", "build", target], cwd=PROJECT, check=True, capture_output=True)
    rel = subprocess.run(["bazel", "cquery", "--output=files", target], cwd=PROJECT,
                         check=True, capture_output=True, text=True).stdout.split()[0]
    workspace = subprocess.run(["bazel", "info", "workspace"], cwd=PROJECT, check=True,
                               capture_output=True, text=True).stdout.strip()
    return os.path.join(workspace, rel)


def boot_once(m, out_dir, n):
    """One boot for measure `m`; returns the console text."""
    log = os.path.join(out_dir, f"{m.key}-{n}.log")
    args = f"console=sbi minikern.halt=poweroff {m.bootargs}".strip()
    command = [sys.executable, os.path.join(HERE, "board.py"), args, "-t", str(m.seconds),
               "-o", log, "-q", "-w", "900"]
    if m.program is not None:
        command += ["-p", built(m.program)]
    subprocess.run(command, check=False)
    return open(log, "rb").read().decode("utf-8", "replace") if os.path.exists(log) else ""


def verdict(m, now, before):
    """'ok', or why this measure fails, given this run's and earlier readings."""
    if not now:
        return "no reading: the console no longer prints what the pattern expects"
    median = statistics.median(now)
    if m.ceiling is not None:
        return "ok" if max(now) <= m.ceiling else f"read {max(now):g}; the ceiling is {m.ceiling:g}"
    if m.tolerance is None:
        return "ok" if max(now) == 0 else f"read {max(now):g}; this counter must stay 0"
    if not before:
        return "ok"
    was = statistics.median(before)
    if was == 0:
        return "ok"
    lost = (was - median) / was * m.better
    if lost > m.tolerance:
        return (f"regressed: median {median:g} against {was:g} before, "
                f"{lost * 100:.0f}% worse, tolerance {m.tolerance * 100:.0f}%")
    return "ok"


def main():
    ap = argparse.ArgumentParser(description="Board benchmarks, kept and compared.")
    ap.add_argument("-k", action="append", default=None, help="run measures whose key has this")
    ap.add_argument("-o", "--out", default=None, help="directory for the boot logs")
    ap.add_argument("--dry-run", action="store_true", help="list what would run, and stop")
    args = ap.parse_args()
    chosen = [m for m in MEASURES if not args.k or any(k in m.key for k in args.k)]
    before = history()
    if args.dry_run:
        for m in chosen:
            print(f"{m.key}: {m.runs} boot(s) of '{m.bootargs}', "
                  f"{len(before.get(m.key, []))} earlier reading(s)")
        return 0
    out_dir = args.out or tempfile.mkdtemp(prefix="bench-")
    os.makedirs(out_dir, exist_ok=True)
    when = datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S")
    at = commit()
    texts = {}
    failed = 0
    for m in chosen:
        # The counters come from the spin boot: boot it once, read it thrice.
        key = (m.bootargs, m.program, m.runs)
        if key not in texts:
            texts[key] = [boot_once(m, out_dir, n) for n in range(m.runs)]
        now = [v for t in texts[key] for v in m.readings(t)[-1:]]
        result = verdict(m, now, before.get(m.key, []))
        record([[when, at, m.key, f"{v:g}"] for v in now])
        print(f"{'ok  ' if result == 'ok' else 'FAIL'}  {m.key}: {now} — {result}")
        failed += result != "ok"
    print(f"bench: {len(chosen) - failed} of {len(chosen)} ok, logs in {out_dir}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
