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

import fixtures
import image

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))
BOARD_TARGET = "//:minikern_board"
SERVED = os.path.expanduser(os.environ.get("MINIKERN_SERVED", "~/vf2/minikern.bin"))

# The reproducer, named as a Bazel target because the suite must run the one
# that was just built rather than whatever is lying about.
RACESTRESS = "//src/userland/racestress:racestress"

# A filesystem with the shapes the reader has to cope with, built by
# `fixtures.py` rather than kept: an image in a tree is one nobody can say
# how to remake. 3,000 entries is past where a directory's extent map moves
# into a b-tree and the directory into node format.
FIXTURE_ENTRIES = 3000


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
    # **§0's reproducer, and the control that proves the gate is one.**
    #
    # This is the case that found the corruption and the case that now
    # guards it. It is a Go binary, so it is fetched into DRAM by the boot
    # loader rather than read off the card — see `board.py -p`. Two and a
    # half thousand rounds because a boot costs ninety seconds and the
    # capture window is the only part that measures anything.
    #
    # The control matters as much as the case: `minikern.keepadvice` puts
    # back the kernel that ignored `MADV_DONTNEED`, and with it the
    # reproducer fails inside ten rounds. A gate that cannot fail is not a
    # gate, and this one was watched failing before it was trusted passing.
    ("§0: the memory corruption, on the board",
     "minikern.disk=sd minikern.env=STRESS_ROUNDS=2500",
     ["stress: 2500 rounds, nothing went wrong"],
     260, RACESTRESS),
    ("§0 control — the corruption must come back without the fix",
     "minikern.disk=sd minikern.env=STRESS_ROUNDS=2500 minikern.keepadvice",
     ["stress: FAILED"],
     260, RACESTRESS),
    # **§3: a b-tree extent fork and a node directory.** Both were refused
    # outright — `inode 96 has a btree fork` — which meant the root
    # directory of any filesystem with a few thousand files in it could not
    # be listed. The checksum is the part that makes this a test: the
    # payload is `0..255` repeated, so the number the board prints has to be
    # the one `fixtures.py` computes, and a reader returning the wrong
    # blocks is caught rather than merely a reader that fails.
    ("§3: a b-tree extent fork and a 3,000-entry directory",
     f"minikern.cat=/p{FIXTURE_ENTRIES - 1}",
     [f"mounted read-only, {FIXTURE_ENTRIES + 2} entries in /",
      f"cat: /p{FIXTURE_ENTRIES - 1} is {fixtures.PAYLOAD_BYTES} bytes, "
      f"checksum {fixtures.PAYLOAD_CHECKSUM}",
      # **And answered by the index, not by walking.** The two paths return
      # the same inode, so without this the case passes just as happily
      # when the hash index is never reached.
      " hashed, 0 walked)"],
     150, None, True),
    # **§3: a directory block of two filesystem blocks** (`-n size=8192`),
    # which was refused at mount. Entries do not straddle a directory block
    # but they do straddle the filesystem blocks inside one, and the
    # checksum covers the whole of it — so this is the case that proves the
    # gathering, and the payload checksum proves it gathered them in order.
    ("§3: 8 KiB directory blocks",
     f"minikern.cat=/p{FIXTURE_ENTRIES - 1}",
     [f"mounted read-only, {FIXTURE_ENTRIES + 2} entries in /",
      f"cat: /p{FIXTURE_ENTRIES - 1} is {fixtures.PAYLOAD_BYTES} bytes, "
      f"checksum {fixtures.PAYLOAD_CHECKSUM}",
      " hashed, 0 walked)"],
     150, None, 8192),
    # **§3: the write path, which had never run.** `xfs_create` had no
    # caller at all, so create, write and unlink were code that compiled.
    # The filesystem is a RAM disk the boot loader fetched, discarded at the
    # next power cycle — so a test that corrupts it costs nothing and the
    # operator's card is never touched. Each step checks what the previous
    # one claimed rather than its return value, and the last pair is the
    # reason a link count exists: removing one name leaves the file
    # readable under the other.
    ("§3: create, write, link, rename, unlink, and growth",
     "minikern.fswrite",
     ["fswrite: every step passed",
      # **And the allocator's growth path was reached.** Sixty names need a
      # second chunk of inodes only if the first runs out; a run where it
      # did not would report every step passing while never touching it.
      # **And every growth path was reached.** Two hundred names need a
      # second inode chunk, a block directory and then a leaf directory; a
      # run that needed none of them would report every step passing while
      # never touching the code that makes them.
      # **And every growth path was reached.** Four hundred names need a
      # block directory, a leaf directory, a second data block under it and
      # six chunks of inodes; a run that needed none of them would report
      # every step passing while never touching the code that makes them.
      # Every shape a growing directory passes through, and the splits that
      # make the last of them grow rather than merely exist. The counts are
      # pinned because they are behavioural and deterministic — twelve
      # hundred fixed names hash the same way every run — so a change in
      # them is a change in what the code did, not in how big it is. The
      # splits mattered most: seven hundred names reached node format and
      # split nothing, because the conversion's two leaves hold about a
      # thousand hashes between them.
      "1 to block, 1 to leaf, 1 to node, 4 data block(s), 4 leaf split(s)",
      # The allocator left a full group — and its control, in this same
      # run, shows the write being refused without that search.
      "crossed a group after",
      # A filesystem the kernel left dirty mounts read-only after a check,
      # rather than being refused — a power cut must not make a disk
      # unreadable.
      # A filesystem the kernel left dirty is replayed and comes back
      # writable — and, in the same run, refusing the replay still falls
      # back to read-only. Both, because "replayed" would otherwise be
      # unfalsifiable: the fallback is what a kernel does when it meets a
      # log item it cannot apply, so it is a live path, not history.
      "the log was replayed; the filesystem is clean",
      "the log could not be replayed; mounting read-only"],
     700, None, "small"),

    ("§3: automount puts each volume where its label says",
     "minikern.volumes",
     [# The root filesystem is **registered**, not mounted a second time:
      # automount and `xfsmount` owning the same device gave it two
      # write-capable block caches. The wording differs between the two for
      # exactly that reason, so the test asserts both forms.
      "volumes: \"ROOT\" at / (the root filesystem's own mount)",
      "volumes: \"DATA\" mounted at /volumes/DATA",
      "volumes: 2 mounted",
      # The policy held *and* each volume's own file was reachable only
      # through its own mount point. A kernel that mounted both at / would
      # satisfy a test that merely counted mounts.
      "volumes: every volume where its label says"],
     240, None, "twovol"),
]


# Staging is `image.stage`, called by `board.py` on every boot — the check
# belongs with the choice of image rather than with one of its callers, which
# is what let a single manual boot run a stale image after the suite had
# grown a check of its own.


def built(target):
    """The path of a target's output, built first — `cquery` alone does not
    build, which is the mistake this file's staleness check exists for."""
    subprocess.run(["bazel", "build", target], cwd=PROJECT, check=True,
                   capture_output=True)
    rel = subprocess.run(["bazel", "cquery", "--output=files", target], cwd=PROJECT,
                         check=True, capture_output=True, text=True).stdout.split()[0]
    workspace = subprocess.run(["bazel", "info", "workspace"], cwd=PROJECT, check=True,
                               capture_output=True, text=True).stdout.strip()
    return os.path.join(workspace, rel)


def run_case(name, bootargs, wanted, seconds, out_dir, program=None, needs_image=False):
    log_path = os.path.join(out_dir, name.split(" ")[0].replace("/", "_").replace("§", "s")
                            + ".log")
    command = [sys.executable, os.path.join(HERE, "board.py"), bootargs,
               "-t", str(seconds), "-o", log_path, "-q", "-w", "900"]
    if program is not None:
        command += ["-p", built(program)]
    if needs_image:
        # True for the default geometry, or a directory-block size in bytes.
        at = os.path.join(out_dir, "fx")
        if needs_image == "small":
            img = fixtures.small_directory(at)
        elif needs_image == "twovol":
            img = fixtures.two_labelled_volumes(at)
        else:
            size = None if needs_image is True else needs_image
            img = fixtures.big_directory(at, FIXTURE_ENTRIES, size)
        command += ["-i", img]
    subprocess.run(command, check=False)
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
    for case in CASES:
        name, bootargs, wanted, seconds = case[:4]
        program = case[4] if len(case) > 4 else None
        needs_image = len(case) > 5 and case[5]
        if args.only and args.only not in name:
            continue
        missing, log_path = run_case(name, bootargs, wanted, seconds, args.out, program,
                                     needs_image)
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
