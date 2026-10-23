#!/usr/bin/env python3
"""The board suite: every self-test this kernel has, run on real hardware.

**Hardware in the loop, as the ordinary check rather than the last one.**
The emulator agrees with whatever you believed — `CLAUDE.md` §5 is largely a
list of defects it hid — so the board is where this kernel is tested, and a
suite that takes one command is what makes that practical.

Two things make it quick enough to use often. The kernel runs **every
self-test named on its command line, in one boot**, so a dozen programs cost
one power cycle instead of a dozen; and the cases below are grouped with
that in mind. A full pass is a handful of boots rather than twenty.

The **staleness check comes first and is not optional**. `bazel cquery`
prints the path of an output without building it, so staging an image that
way can copy the *previous* build — which happened during the `MADV_DONTNEED`
work and cost a board run that reported a defect in code that was not on the
board. So the image is built, hashed, compared against what the web server
will actually hand out, and re-staged when they differ. A board result from
an unknown binary is worse than no board result.

Each case is a name, a kernel command line, and the lines that must appear.
A case fails if any of them is missing, and the log of every case is kept
under the output directory so a failure can be read rather than re-run.
"""
import argparse
import os
import subprocess
import sys

import image

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))
BOARD_TARGET = "//:minikern_board"
SERVED = os.path.expanduser(os.environ.get("MINIKERN_SERVED", "~/vf2/minikern.bin"))

# A case is (name, bootargs, [lines that must appear], capture seconds).
#
# **An exit status here is not a boolean, and getting that wrong cried wolf
# on its first run.** The first version of this file expected `exited with 0`
# from every program and reported six board failures that were all passes:
# these self-tests exit with a **bitmask of the checks that passed**, so
# `mremap` answers 63 and `mapflags` 127, and the ones that report a child's
# status answer whatever the child chose — 5 for `endsib`, 7 for `pgrp` and
# `wait`. A mask is the better design, because it says *which* check failed
# rather than that one did; what it is not is something to compare against
# zero. Each expectation below is the program's own full-pass value, taken
# from its `ori s2, s2, N` chain rather than assumed.
#
# The disk cases need the operator's SD card: an XFS filesystem with `/init`
# (a stock Go binary) and a 64 MiB non-sparse `/swapfile`.
CASES = [
    ("memory self-tests",
     "minikern.dontneed minikern.regs minikern.mremap minikern.mapflags "
     "minikern.huge minikern.mapchurn",
     ["dontneed exited with 0", "regs exited with 0", "mremap exited with 63",
      "mapflags exited with 127", "huge exited with 127",
      "mapchurn exited with 0"],
     140),
    ("MADV_DONTNEED control — the guarantee must fail when it is broken",
     "minikern.dontneed minikern.keepadvice",
     ["dontneed exited with 200"],
     60),
    ("process model self-tests",
     "minikern.pgrp minikern.uid minikern.rlimit minikern.wait minikern.endsib",
     ["pgrp exited with 7", "uid exited with 0", "rlimit exited with 0",
      "wait exited with 7", "endsib exited with 5"],
     100),
    ("hostile syscall arguments (Rule 25)",
     "minikern.hostile",
     ["hostile exited with 0"],
     60),
    # `filemap` maps a **file**, so it belongs with the cases that have a
    # filesystem: it printed nothing at all when it was grouped with the
    # anonymous-memory tests, which is what a program with nothing to open
    # does.
    ("swap and a file-backed mapping, on the card",
     "minikern.disk=sd minikern.swap=/swapfile:70 minikern.swaptest=1 "
     "minikern.swapout=1 minikern.filemap",
     ["came back byte for byte", "swap exited with 0", "filemap exited with 63"],
     110),
    ("a stock Go binary off the card",
     "minikern.disk=sd minikern.init=1",
     ["go: done", "init: /init exited with 0"],
     150),
]


# Staging is `image.stage`, called by `board.py` on every boot — the check
# belongs with the choice of image rather than with one of its callers, which
# is what let a single manual boot run a stale image after the suite had
# grown a check of its own.


def run_case(name, bootargs, wanted, seconds, out_dir):
    log_path = os.path.join(out_dir, name.split(" ")[0].replace("/", "_") + ".log")
    subprocess.run([sys.executable, os.path.join(HERE, "board.py"), bootargs,
                    "-t", str(seconds), "-o", log_path, "-q",
                    "-w", "900"], check=False)
    text = open(log_path, "rb").read().decode("utf-8", "replace") if os.path.exists(log_path) else ""
    missing = [line for line in wanted if line not in text]
    return missing, log_path


def main():
    ap = argparse.ArgumentParser(description="Run the board suite on the VisionFive 2.")
    ap.add_argument("-o", "--out", default="/tmp/minikern-hil", help="where logs go")
    ap.add_argument("-k", "--only", default=None,
                    help="run only the cases whose name contains this text")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    print(f"hil: image md5 {image.stage(PROJECT)}")
    failures = 0
    for name, bootargs, wanted, seconds in CASES:
        if args.only and args.only not in name:
            continue
        missing, log_path = run_case(name, bootargs, wanted, seconds, args.out)
        if missing:
            failures += 1
            print(f"FAIL  {name}")
            for line in missing:
                print(f"        missing: {line!r}")
            print(f"        log: {log_path}")
        else:
            print(f"ok    {name}")
    print(f"hil: {failures} case(s) failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
