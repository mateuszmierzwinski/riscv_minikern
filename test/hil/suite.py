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
import tempfile
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
     [f", {FIXTURE_ENTRIES + 2} entries in /",
      f"cat: /p{FIXTURE_ENTRIES - 1} is {fixtures.PAYLOAD_BYTES} bytes, "
      f"checksum {fixtures.PAYLOAD_CHECKSUM}",
      # **And answered by the index, not by walking.** The two paths return
      # the same inode, so without this the case passes just as happily
      # when the hash index is never reached.
      #
      # This asked for "0 walked" and was wrong: a shortform or block-format
      # directory has no leaf block to search, so a walk is the *correct*
      # path for one, and the boot looks things up in small directories —
      # `/var` for the log among them. The case failed on three legitimate
      # walks while the index was working. Pinning the exact counts instead
      # would make it fail whenever the boot's own lookups change, which is
      # not what it is about, so the kernel states the property and this
      # asserts the property.
      "cat: hash index answered"],
     150, None, True),
    # **A real v4 filesystem**, built by `mkfs.xfs -m crc=0` rather than by
    # altering a v5 image's version number — which is the only way the
    # shorter headers and the absent checksums get exercised rather than
    # asserted. The whole v4 difference is a handful of sizes and magics
    # taken from the superblock, so what this proves is that the *same*
    # reader handles both; a separate v4 path would pass this and mean
    # nothing.
    #
    # The payload checksum is what makes it a test rather than a mount: a
    # reader that found the extent fork at 176 instead of 100 would be
    # reading the middle of a v2 inode's own fork, and would either fail or
    # return the wrong blocks. Only the checksum tells those apart.
    ("v4: the pre-checksum format, read on the board",
     "minikern.cat=/seed",
     # "xfs: v4" is the assertion that matters: it says which reader ran.
     ["xfs: v4,",
      f"cat: /seed is {fixtures.PAYLOAD_BYTES} bytes, "
      f"checksum {fixtures.PAYLOAD_CHECKSUM}"],
     150, None, "v4"),
    # **§3: a directory block of two filesystem blocks** (`-n size=8192`),
    # which was refused at mount. Entries do not straddle a directory block
    # but they do straddle the filesystem blocks inside one, and the
    # checksum covers the whole of it — so this is the case that proves the
    # gathering, and the payload checksum proves it gathered them in order.
    ("§3: 8 KiB directory blocks",
     f"minikern.cat=/p{FIXTURE_ENTRIES - 1}",
     [f", {FIXTURE_ENTRIES + 2} entries in /",
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
      "1 to block, 1 to leaf, 1 to node, 8 data block(s), 5 leaf split(s)",
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
      # Attributes were written, read back, listed, and found again after
      # the filesystem was mounted afresh — which is the only step that
      # proves they reached the disk rather than a variable, since the
      # writer and the reader are the same code.
      # Attributes written, read back, listed, found again after the
      # filesystem was mounted afresh, and the fork pushed **out of the
      # inode into a leaf block** — the count matters because a set that
      # happened to fit inside the inode would exercise none of the leaf
      # writer, and the leaf *reader* was wrong for exactly that reason
      # until the writer existed to check it.
      "fswrite: attributes set, listed, survived a mount, removed; 1 to leaf",
      # A value too large to share a block with its name goes into blocks of
      # its own, and comes back byte for byte across a mount.
      "fswrite: a 3000-byte value went to its own blocks and came back",
      "the log was replayed; the filesystem is clean",
      "the log could not be replayed; mounting read-only"],
     700, None, "small"),

    # **The same write path on a filesystem made with `mkfs.xfs` defaults**:
    # reverse map, reflink, sparse inodes and 64-bit extent counts all on.
    # That filesystem mounted read-only until the writer kept the reverse
    # map, so this case is the board's half of that change; the host's
    # `xfs_write_test` is the other half, with `xfs_repair` as the judge.
    # The growth counts are left out for the reason the v4 case gives:
    # they are behavioural for one geometry, and sparse inodes change it.
    ("§3: the write path on a filesystem made with mkfs.xfs defaults",
     "minikern.fswrite",
     ["fswrite: every step passed",
      "crossed a group after",
      "fswrite: attributes set, listed, survived a mount, removed; 1 to leaf",
      "fswrite: a 3000-byte value went to its own blocks and came back",
      "the log was replayed; the filesystem is clean",
      "the log could not be replayed; mounting read-only"],
     700, None, "defaults"),

    # **The whole write path on a real v4 filesystem.** The same instrument
    # as the case above, pointed at an image `mkfs.xfs -m crc=0` made — so
    # create, write, link, rename, unlink, attributes and log replay all run
    # against the older layout rather than against an assertion that they
    # would.
    #
    # This is where v4 writing is actually dangerous rather than merely
    # absent. Three things in that path write at offsets a v4 inode does not
    # have: resealing puts a checksum at 100, which is the literal area's
    # first byte; `di_ino` and `di_uuid` at 152 and 160 are inside the fork;
    # and log replay's field table runs to 152 and copies a UUID at 160. Any
    # of them corrupts the file it is updating and none of them fails, so
    # only reading the file back afterwards can tell.
    #
    # The image has 512-byte inodes, like the v5 case, so that this
    # compares the *format* and nothing else: `mkfs.xfs` defaults to 256,
    # which leaves a v4 inode nine extents of literal area and makes a
    # growing directory need a b-tree fork — refused by design, and nothing
    # to do with v4. The default-size image is what the read case uses, and
    # it is what found the fork-capacity bug this now bounds.
    #
    # The growth counts the v5 case pins are deliberately left out: without
    # the file-type byte a directory entry is a byte shorter, so the same
    # names reach the same shapes after a different number of splits. Those
    # numbers are behavioural for a given format, not across two.
    ("v4: the write path on the pre-checksum format",
     "minikern.fswrite",
     ["xfs: v4,",
      "fswrite: every step passed",
      "fswrite: attributes set, listed, survived a mount, removed",
      "fswrite: a 3000-byte value went to its own blocks and came back",
      "the log was replayed; the filesystem is clean"],
     700, None, "v4big"),

    # **QuokkaOS's init, on the board, from a filesystem.**
    #
    # The whole userland on one XFS image: /init, /bin/rc, /bin/qservice, and
    # a default runlevel of four services as *symlinks* — which is what makes
    # this a test of the kernel's symlinkat and readlinkat as well as of
    # init. The symlinks were written by `mkfs.xfs`, not by this kernel, so
    # the read path is checked against the real format rather than against
    # our own writer.
    #
    # What is asserted is the behaviour the dependency graph exists for: b
    # starts only after a is *ready*, and a becomes ready 400ms after it
    # starts by writing a file. A dependent that started on a PID would beat
    # that. Then `dies` exits on purpose and is restarted, and rc's own
    # binary prints the status table to the console.
    ("quokka: init starts a runlevel in order, supervises it, and answers rc",
     "minikern.init",
     ["init: QuokkaOS on",
      # a's readiness comes from a file the service writes itself, and rc
      # reports it. The service's own "ready, wrote" line went to its log
      # file once QuokkaOS stopped sharing the console with services.
      "a        default   ready",
      # Every service up, which for b and c means their dependencies were
      # ready first — the graph refusing to start them otherwise.
      "init: level 1 of",
      # rc, through its own binary and its own socket.
      "init: running /bin/rc list",
      "SERVICE",
      "init: rc list returned cleanly"],
     400, None, "quokka"),

    # **Capabilities and `seccomp`, consulted rather than merely kept.**
    #
    # The rules are host-tested — `caps_test` covers every refusal the
    # policy makes and `bpf_test` every program the verifier rejects — so
    # what this adds is that the kernel asks them. A capability model whose
    # bits are maintained perfectly and never consulted is privilege
    # separation on paper, which is a failure this project has shipped
    # twice in other guises.
    #
    # The step that carries the claim is a `swapon` refused with EPERM from
    # a process that is **still uid 0**, having dropped CAP_SYS_ADMIN from
    # its bounding set. Its control is the same call made while the
    # capability is held, failing for a different reason — without that,
    # the refusal would prove nothing, because every `swapon` here fails.
    ("§1: capabilities and a seccomp filter",
     "minikern.privsep",
     ["uspace: privsep exited with 0"],
     120, None),

    # **The unified log**: timestamped, levelled, kept, written and readable.
    #
    # Four claims in one run, because they only mean anything together. The
    # prefix is asserted as exact characters, since anything that reads a
    # log depends on its shape. The rotation is run three times in one boot
    # — a RAM disk is re-staged on every boot, so there is otherwise no way
    # to watch it — and the third pass is the interesting one: it must move
    # 1 to 2 *before* 0 to 1, or a boot is overwritten before it is copied.
    # `kernel.2.log` is then read back to prove the rotated file is the
    # earlier boot rather than a copy of this one.
    #
    # The device is read by a *program*, because a descriptor needs a
    # process: the first version of this check ran in the boot path and was
    # refused with EBADF, which was the fd layer telling the truth.
    ("§1: the kernel log — timestamps, levels, rotation, and its device",
     "minikern.klogrotate minikern.klogdev minikern.cat=/var/log/kernel.2.log",
     ["] I mmu: sv39 on",
      "] N klog: 0 earlier boots rotated; this one becomes kernel.0.log",
      "] I klog: rotated kernel.1.log to kernel.2.log",
      "] I klog: rotated kernel.0.log to kernel.1.log",
      "uspace: klogdev exited with 0",
      "cat: /var/log/kernel.2.log is"],
     220, None, "small"),

    # **The descriptor layer's write pattern, against a disposable image.**
    #
    # This exists because that pattern corrupted the operator's own card: its
    # root went from 8 entries to 1116 with names that are not text, while
    # every individual call returned success. So the assertion is the entry
    # count, not a return code — a test that checked only return codes would
    # have passed on the run that did the damage.
    #
    # It runs against the `small` fixture, which is what should have been
    # used for the first run of untested write code in the first place. The
    # harness has had it all along.
    ("§3: a zero-length create then chunked writes at offsets",
     "minikern.chunked",
     ["chunked: the zero-length create said rc 0",
      "chunked: chunk 0 at offset 0 said rc 0",
      "chunked: chunk 2 at offset 65536 said rc 0",
      # One file is one entry. Anything else is the corruption.
      # 4 to 5: one new entry for one file. The count includes "." and
      # "..", which the mount message does not — so it reads two higher than
      # the line above it, and guessing rather than measuring it is how this
      # case failed its first run.
      "chunked: the root had 4 entries and now has 5"],
     220, None, "small"),

    # **The USB controller, as far as it is proved.**
    #
    # Deliberately asserts the controller and the port map and *not* an
    # enumerated device: the port never reaches its enabled state and the
    # cause is not identified (docs/ROADMAP.md §6 has the register traces).
    # A case that waited for a device would be a known-failing test, and one
    # that asserted nothing would not notice the controller breaking — so it
    # pins exactly what works, which is everything up to the port.
    #
    # The version, port count and slot count come from registers the
    # specification fixes, so a wrong window reads as zeroes rather than as
    # these numbers. "Running" means the controller accepted its rings and
    # left its halted state, which is the whole of reset, DCBAA, the command
    # ring, the event ring and the DMA fences working.
    ("§6: the USB controller runs and maps its ports",
     "minikern.usb",
     ["usb: xhci 1.0, 5 ports, 32 device slots",
      "usb: the controller is running",
      "xhci: ports 1..1 speak USB 2",
      "xhci: ports 2..5 speak USB 3"],
     200, None),

    # **The SoC's own temperature, and /proc.**
    #
    # The assertion is the kernel's plausibility verdict rather than a
    # number, because a number would fail on a cold morning and this must
    # fail only when the sensor is wrong. It catches both failures this
    # driver actually had: a raw zero, which converts to a confident
    # -81.1 C, and a wrong register offset, which reads as something absurd.
    #
    # `/proc/thermal` is asserted separately from the sensor, because the
    # two are different things — the first version read the sensor correctly
    # and could not be opened, since the generated files were wired into the
    # open path and not into stat.
    ("§6: the die temperature, and /proc/thermal",
     "",
     ["which is a plausible die temperature",
      "/proc/thermal reports it"],
     150, None),

    # **The four buses the board actually has**, each proved by something
    # that cannot pass if the driver is wrong.
    #
    # A driver that maps the wrong window, or whose block is unclocked, reads
    # zeroes — so none of these checks is "it bound". SPI reports the
    # PrimeCell identifier (ARM's 0x41, part 0x022) and then sends a byte
    # through internal loopback and gets it back, which is how the GMAC was
    # proved before a cable existed. I2C reports the DesignWare signature.
    # PWM and the watchdog are read twice with their counters running, and a
    # value that moved is a block that is clocked.
    #
    # The RTC line is asserted too, because "no driver" and "no such device
    # on this board" are different facts and the second one is the truth
    # here — the tree names none.
    ("§6: SPI, I2C, PWM and the watchdog",
     "minikern.busprobe",
     ["spi: PrimeCell id 00041022 (part 022, designer 41)",
      "spi: loopback sent a5, received a5 — the data path works",
      "i2c: component type 44570140",
      "pwm: counter",
      "it is clocked and running",
      "rtc: the tree names no real-time clock on this board"],
     200, None),

    # **A program asking the kernel what interfaces exist**, over netlink.
    #
    # The program parses the replies by stepping the lengths in the message
    # headers, which is the point: a case that only checked bytes came back
    # would pass against an encoder whose lengths are wrong, and a wrong
    # length is the likeliest defect in a hand-written one. 127 is all seven.
    ("§5: a program asks what interfaces exist, over netlink",
     "",
     ["netlink: the link dump ended with NLMSG_DONE",
      "netlink: both lo and eth0 were listed",
      "netlink: eth0's hardware address came back",
      "netlink: the default route came back with its gateway",
      "netlink: an unimplemented request was refused, not answered empty",
      "exited with 127"],
     200, "//:netlinkask.elf"),

    # **Two threads talking over a unix-domain socket**, which is the
    # roadmap's "nothing local can talk over a socket".
    #
    # 127 is all seven checks: a stream bound to an abstract name, connect
    # and accept, bytes each way, a half-close seen as end of file *after*
    # the buffered bytes, a datagram round trip, and a connect to an unbound
    # name refused. Bit 128 is set when the guards around the cloned thread's
    # stack were damaged, so 255 reads as a failure rather than as a better
    # 127 — a status that only goes up would have hidden that.
    ("§5: two threads over a unix-domain socket",
     "",
     ["unixsock: the client's bytes arrived byte for byte",
      "unixsock: end of file after the buffered bytes",
      "unixsock: a datagram arrived whole",
      "unixsock: connecting to an unbound name was refused",
      "unixsock: the guards around the cloned stack are intact",
      "exited with 127"],
     200, "//:unixsock.elf"),

    # **Does wait4 return for a child that has exited, two hundred times?**
    #
    # A Go init on this kernel shows two failures that alternate between
    # boots, and one is a parent whose wait4 never returns for a child the
    # kernel has already recorded as a zombie. Seven changes were aimed at
    # that from hypotheses before the wait itself was measured; this measures
    # the wait, at a rate that can tell "broken" from "rare" in one boot
    # instead of one wait per boot.
    #
    # Both forms, because init needs both and they are not the same call:
    # wait4(pid) is what os/exec does, wait4(-1) is what PID 1 collects
    # orphans with and is the one the reaper blocks in. A kernel that got one
    # right and the other wrong would look intermittent from a program using
    # both. 3 is both halves; the progress lines localise a hang, which is
    # the one fact silence destroys.
    ("§P2: wait4 returns for an exited child, two hundred times, both forms",
     "",
     ["waitloop: four children alive alongside the one being waited for",
      "waitloop: wait4(pid), the form os/exec uses",
      "waitloop:   through round 200",
      "waitloop: wait4(-1), the form PID 1 collects orphans with",
      "waitloop: passed mask 3",
      "exited with 3"],
     200, "//:waitloop.elf"),

    # **A name resolved from user space**, which is the roadmap's DNS item.
    #
    # The program is the assertion: it reads /etc/resolv.conf, sends a query
    # and walks the answer to an A record, exiting with a bitmask of the
    # steps that passed. 31 is all five, and anything less says where it
    # stopped — which is why the status is checked rather than the log.
    #
    # It needs the real network and the operator's own resolver, so it is as
    # much a test of the GMAC, the route and the interrupt as of DNS: three
    # defects in those showed up here before this ever returned 31.
    ("§5: a name resolved from user space",
     "",
     ["resolve: nameserver read from /etc/resolv.conf",
      "resolve: an A record was found; a name resolved from user space",
      "exited with 31"],
     220, "//:resolve.elf"),

    # **The stack over the board's own Ethernet**, which is the whole of
    # roadmap §5's "the board has no network".
    #
    # The driver had worked for a while — link, rings, frames both ways in
    # loopback — and `bring_up_network` never tried it: it asked for a
    # virtio device, got ENOENT, and reported that the machine had no
    # network with a working Ethernet controller sitting next to it.
    #
    # A DHCP lease is the assertion rather than a link or a sent frame,
    # because a lease is the first thing that cannot happen by accident: it
    # needs ARP, UDP, a broadcast out, two replies in from a real router,
    # and the stack to believe the result. The address is not pinned —
    # it comes from the operator's router — but the mask, gateway and the
    # word `dhcp` are.
    ("§5: a DHCP lease over the board's own Ethernet",
     "",
     ["net: over the board's Ethernet, link up at 1000 Mbit full duplex",
      "/24 gw 192.168.86.1",
      "(dhcp, lease",
      # And nothing was dropped as damaged, which a lease alone would not
      # show: a stack can bind while discarding most of what it receives.
      # `not-ours` is not asserted: the controller runs promiscuous (the
      # strict-unicast-filter workaround, ROADMAP §5), so other hosts'
      # frames on the LAN are counted there by design.
      "dropped: malformed 0 checksum 0 fragment 0 not-ours"],
     150, None),

    ("§4: the board's own Ethernet negotiates a link",
     "minikern.gmacup",
     [# The PHY the probe identified, and a negotiated link on the
      # controller that has the cable. The speed is asserted because the MAC
      # has to be configured to match it: a MAC set for gigabit on a
      # hundred-megabit link loses every frame and says nothing.
      "dwmac: PHY at 0, id 4f51e91b",
      "dwmac: link up at 1000 Mbit, full duplex",
      # And the rings, proved in loopback rather than against the network:
      # a quiet switch port and a broken receive ring are indistinguishable
      # from outside, and the first version of this test could not tell
      # them apart. The frame sent is the frame that must arrive.
      "dwmac: frame of 64 bytes, 6c:cf:39:00:b6:89 -> ff:ff:ff:ff:ff:ff type 0806",
      "dwmac: 1 sent, 1 received, 0 dropped"],
     120, None, False),

    ("§2: swap under a watermark, with the clock choosing",
     "minikern.swap=/swapfile:60 minikern.swappress",
     [# The control: swappiness zero really is off.
      "swappress: with swappiness at zero, nothing went out unasked",
      # Then both halves of the policy, because either alone can be
      # satisfied by the wrong code: pages going out says the watermark
      # fired, and second chances say the **clock** chose them rather than
      # the old take-the-first-page scan, which would report none.
      "swappress: the watermark and the clock both acted"],
     330, None, "swap"),

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

    # **The root by its label, behind a FAT boot partition.** Partition 1 is
    # FAT — what a disk that boots locally carries, because U-Boot cannot
    # read XFS — so a kernel that took partition 1 as the root would fail
    # here. ROOT is partition 2 and DATA partition 3, and the FAT is passed
    # over by automount as not a filesystem this kernel mounts.
    ("§3: the root is found by its label behind a FAT boot partition",
     "minikern.volumes",
     ["disk: the root is volume 1 on ram, labelled ROOT",
      "volumes: FAT32 \"BOOT\" mounted at /boot",
      "volumes: \"ROOT\" at / (the root filesystem's own mount)",
      "volumes: \"DATA\" mounted at /volumes/DATA",
      "volumes: every volume where its label says"],
     240, None, "bootfirst"),
    # **The boot partition at /boot**, read and written by the kernel the
    # firmware loaded from it: a file listed, written, read back, moved into
    # a directory made for it, refused across to the root filesystem, and
    # removed — the calls a program's `cp`, `mv` and `rm` make. The stand-in
    # kernel `mcopy` put there is read back too. The host's `fat_write_test`
    # judges the same driver with `fsck.fat`.
    ("§3: the FAT boot partition, read and written at /boot",
     "minikern.bootwrite minikern.cat=/boot/minikern.bin",
     ["volumes: FAT32 \"BOOT\" mounted at /boot",
      "the boot partition is read and written at /boot",
      "cat: /boot/minikern.bin is 48 bytes"],
     240, None, "bootfirst"),
    # **Saved ids** (§1): `setresuid` with root kept as the saved uid is a
    # drop that can be undone, and the privilege comes back with it; with
    # all three ids dropped it cannot. 127 is all seven.
    ("§1: saved ids — a privilege drop undone, and one that cannot be",
     "",
     ["exited with 127"],
     200, "//:ids.elf"),
    # **TRIM, judged by the device.** A file written and removed, then the
    # root trimmed: the RAM disk counts what it was told to discard, and the
    # kernel's figure must equal it — a trim that reports more than reached
    # the drive, or discards a live block (the kept file would read back
    # zeroed), fails. `minikern.autotrim` trims every volume at boot first.
    # On a RAM disk because the board's NVMe drive holds the operator's data.
    ("§4: TRIM on the RAM disk, judged by the device",
     "minikern.trimtest minikern.autotrim",
     ["KiB discarded",
      "the device saw",
      "the kept file reads back whole"],
     240, None, "defaults"),
    # **`fstrim`'s system call**: FITRIM on the root directory, from a
    # program — the struct copied in and back, the minimum honoured, an
    # unknown request ENOTTY, a non-root caller EPERM. 127 is all seven.
    ("§4: fstrim through FITRIM, from a program",
     "",
     ["fstrim: / trimmed, bytes: ",
      "fstrim: a long minimum trimmed nothing",
      "fstrim: an unknown ioctl on a directory is ENOTTY",
      "fstrim: an unprivileged caller is refused",
      "exited with 127"],
     240, "//:fstrim.elf", "defaults"),
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


# QuokkaOS's binaries, cross-built for the board.
#
# Built here rather than taken from bazel-bin for the reason the image's own
# staleness check exists: `bazel-bin` points at the most recent
# configuration's output, so a binary staged from it after a coverage run is
# the instrumented one. Building to a known path means the bytes in the
# fixture are the bytes this function just produced.
QUOKKA = os.path.join(HERE, "..", "..", "..", "..", "..", "go", "quokkaos")


def built_go(what):
    """Cross-builds one QuokkaOS binary for riscv64 and returns its path."""
    out = os.path.join(tempfile.gettempdir(), f"quokka-{what}-riscv64")
    env = dict(os.environ, CGO_ENABLED="0", GOOS="linux", GOARCH="riscv64")
    # **Symbols kept.** The first version stripped them for size, and then a
    # fault in one of these binaries reported an address that could not be
    # looked up — so it was looked up in an unstripped rebuild instead, whose
    # addresses are different, and the answer was wrong. A megabyte of symbol
    # table is worth less than a diagnosable fault.
    subprocess.run(["go", "build", "-o", out, f"./cmd/{what}"],
                   cwd=os.path.abspath(QUOKKA), env=env, check=True)
    return out


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
        elif needs_image == "defaults":
            img = fixtures.small_directory_defaults(at)
        elif needs_image == "twovol":
            img = fixtures.two_labelled_volumes(at)
        elif needs_image == "bootfirst":
            img = fixtures.boot_partition_first(at)
        elif needs_image == "swap":
            img = fixtures.swap_volume(at)
        elif needs_image == "quokka":
            img = fixtures.quokka_userland(at, built_go("init"), built_go("rc"),
                                           built_go("qservice"))
        elif needs_image == "v4":
            img = fixtures.v4_filesystem(at)
        elif needs_image == "v4big":
            img = fixtures.v4_filesystem_big_inodes(at)
        else:
            size = None if needs_image is True else needs_image
            img = fixtures.big_directory(at, FIXTURE_ENTRIES, size)
        command += ["-i", img]
    subprocess.run(command, check=False)
    text = open(log_path, "rb").read().decode("utf-8", "replace") if os.path.exists(log_path) else ""
    missing = [line for line in wanted if line not in text]
    return missing, log_path


def aslr_differs(out_dir):
    """**Two runs, because one proves nothing.**

    A program cannot test its own randomness: printing an address and
    asserting it looks random is unfalsifiable from the inside. The claim
    is that two runs differ, and only something outside a run can make it.

    This found a real defect rather than confirming a belief: the entropy
    pool had no callers at all, so every boot hashed the same zeroed pool
    and the addresses were *identical*. A single-run case would have passed
    while randomisation randomised nothing.
    """
    seen = []
    for n in (1, 2):
        log = os.path.join(out_dir, f"aslr{n}.log")
        subprocess.run([sys.executable, os.path.join(HERE, "board.py"), "minikern.aslr",
                        "-t", "90", "-o", log, "-q", "-w", "900"], check=False)
        text = open(log, "rb").read().decode("utf-8", "replace") if os.path.exists(log) else ""
        got = {}
        for line in text.splitlines():
            for key in ("sp=", "brk=", "map="):
                if line.startswith(key):
                    got[key] = line[len(key):].strip()
        seen.append(got)
    missing = []
    for key in ("sp=", "brk=", "map="):
        a, b = seen[0].get(key), seen[1].get(key)
        if a is None or b is None:
            missing.append(f"{key} was not reported in both runs ({a!r}, {b!r})")
        elif a == b:
            # Named with the value, because "they matched" sends the next
            # reader to guess which of the three and at what address.
            missing.append(f"{key}{a} was the same in both runs")
    return missing, os.path.join(out_dir, "aslr1.log")


def main():
    ap = argparse.ArgumentParser(description="Run the board suite on the VisionFive 2.")
    ap.add_argument("-o", "--out", default="/tmp/minikern-hil", help="where logs go")
    ap.add_argument("-k", "--only", default=None,
                    help="run only the cases whose name contains this text")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    print(f"hil: image md5 {image.stage(PROJECT)}")
    failures = 0
    extra = [("§1: addresses are randomised, compared across two runs", aslr_differs)]
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
    # The cases that cannot be a line match, because they compare two runs.
    for name, runner in extra:
        if args.only and args.only not in name:
            continue
        missing, log_path = runner(args.out)
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
