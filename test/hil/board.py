#!/usr/bin/env python3
"""One boot of the real VisionFive 2, captured.

**The harness this project did its hardware work without.** Every board run
up to now used a throwaway script that lived outside the tree, so each
session rediscovered the serial ports, the relay commands, the U-Boot
sequence and the load address — and a stale image was staged more than once
because nothing checked. That is the opposite of hardware-in-the-loop: the
board is the only thing that can answer most of the questions in this
kernel's `CLAUDE.md` §5, so reaching it has to be one command.

What a run does, in order: cut the power, restore it, catch the autoboot
window, bring the network up, fetch the image over HTTP, set the kernel
command line, boot, and capture the console for a while. Every step waits
for U-Boot's prompt rather than sleeping a guessed interval — the first
version used fixed delays and lost a command inside the previous one's
output, which presented as `Bad Linux RISCV Image magic!`: the loader
refusing an address nothing had been written to.

The operator's wiring, which is a fact about this machine and not a choice:
the board's console is `/dev/ttyUSB0`, and its power is switched by an
ESP32 relay on `/dev/ttyACM0` that takes `POWER_UP` and `POWER_DOWN`. The
image is served over HTTP from this host because U-Boot's `wget` is the one
transfer that needs nothing installed on the host beyond a web server.

Exit status is about the **harness**, not the test: 0 when the board booted
and a log was captured, 2 when a step did not complete. Whether the kernel
did the right thing is for the caller to decide by reading the log, which is
why the log is always written even when a step fails.
"""
import argparse
import fcntl
import gzip
import os
import shutil
import sys
import time

import image

try:
    import serial
except ImportError:  # pragma: no cover - the operator's machine has it
    sys.exit("pyserial is not installed: python3 -m pip install --user pyserial")

PROMPT = b"StarFive #"
CONSOLE = os.environ.get("MINIKERN_CONSOLE", "/dev/ttyUSB0")
RELAY = os.environ.get("MINIKERN_RELAY", "/dev/ttyACM0")
IMAGE_URL = os.environ.get("MINIKERN_IMAGE_URL", "http://192.168.86.119:8780/minikern.bin")
LOAD_AT = "0x82000000"

# How much room the kernel is given at `LOAD_AT`. It is a few megabytes of
# ELF; this is generous so that a fixture placed just above it still fails
# the overlap check rather than failing to boot.
KERNEL_ROOM = 64 * 1024 * 1024

# Where a program fetched alongside the kernel is put.
#
# **This is what lets a new test program reach the board without a new SD
# card.** U-Boot already has a network — it is how the kernel image arrives —
# so it can fetch a second file just as easily, and the kernel can build a
# process from a buffer. Writing to the card is the operator's to do and was
# the thing the §0 hunt was waiting on; this needs nobody.
#
# 256 MiB into DRAM: clear of the kernel (which loads at `0x4020_0000` and
# whose table arena follows it) and far below where U-Boot relocates itself.
# The kernel reserves the range before the frame allocator hands anything
# out, so nothing steps on the program before it is built.
PROGRAM_AT = 0x50000000

# Where the *compressed* image lands before U-Boot expands it.
#
# `mkfs.xfs` will not make a filesystem smaller than 300 MB, and an image
# with little in it is almost entirely zeroes — it gzips to about a third of
# a megabyte. Expanding it with U-Boot's `unzip` turns a 320 MB transfer
# into a trivial one.
#
# **Above the expanded image, not below it**, and the board found out why: an
# image full of random payloads does not compress, so a 188 MB archive
# landed at `0x5800_0000` and ran straight through `0x6000_0000`, where
# `unzip` was writing its output. The compressed data overwrote itself
# mid-decompression and the kernel reported a corrupt log — a defect in the
# harness wearing the filesystem's clothes. The ranges are checked against
# each other now, as well as moved apart.
# Raised from `0x8000_0000` once a fixture needed two 320 MB filesystems on
# one disk: 642 MB at `0x6000_0000` reaches past it, and the overlap check
# refused the run rather than producing another corrupt-log mystery. The
# board has 8 GiB at `0x4000_0000`, so 4 GiB in is clear of any image a
# fixture can reasonably be.
IMAGE_GZ_AT = 0x100000000

# How much smaller the archive has to be to be worth sending instead of the
# image. A filesystem holding random data compresses to nothing, so the
# archive is then twice the work: one transfer to send it, one decompression
# to undo it.
COMPRESS_IF_UNDER = 0.5

# Where a filesystem image fetched alongside the kernel is put: another
# 256 MiB up, so a program and an image can both be loaded in one boot.
#
# This is what makes the filesystem reader testable at all. Until now there
# was exactly one filesystem to read — the operator's SD card — so every gap
# in the XFS reader could be described and not exercised, because producing a
# filesystem with the awkward shape means writing a card. `mkfs.xfs` on this
# host makes any shape wanted; the board mounts it from DRAM.
# Above the kernel at `0x8200_0000` rather than below it, so that a large
# fixture grows away from it instead of into it.
IMAGE_AT = 0x90000000

# **There is one board, so there is one lock.** Two runs at once give
# `device reports readiness to read but returned no data`, which reads like
# a cable fault and is not one — it is the other run holding the port. A
# lock turns that into a sentence. The file is only a handle for `flock`;
# nothing is written to it.
LOCK = os.environ.get("MINIKERN_HIL_LOCK", "/tmp/minikern-hil.lock")

# **What every board boot wants, added here so no caller can forget it.**
# `console=sbi` keeps the firmware's console, which is the one that is
# trustworthy before the kernel's own UART driver has proved itself (§5), and
# `minikern.halt=poweroff` is what makes a boot a *test* rather than a board
# left running. The suite's first version left both out of every case: the
# board printed anyway, through its own driver, and never powered off.
STANDARD_ARGS = "console=sbi minikern.halt=poweroff"


def power(command):
    """Switches the board's power through the relay. Fire and forget: the
    relay does not answer, and a missing relay must fail loudly here rather
    than as a board that never prints anything."""
    with serial.Serial(RELAY, 115200, timeout=1) as esp:
        esp.write((command + "\r\n").encode())
        esp.flush()
        time.sleep(1.0)


def send(port, line):
    """Sends one command, terminated with a lone carriage return.

    **`\r\n` is two line terminators to U-Boot, and an empty line at its
    prompt repeats the previous command.** So every step was being run
    twice, which went unnoticed for as long as the steps were `dhcp` and
    `wget` — idempotent, and quick enough that the repeat finished before
    the next command was sent. Adding `unzip`, which takes seconds to expand
    a 320 MB image, made the repeat collide with the command after it: the
    `setenv bootargs "..."` went into U-Boot mid-decompression and came out
    as `setenv bootargs booti 0x82000000 ...`, with the kernel command line
    silently gone. One character, and the whole fixture was booting with no
    arguments."""
    port.write(line.encode() + b"\r")
    port.flush()


def read_until(port, needle, timeout, log):
    """Reads until `needle` appears, appending everything to `log`."""
    deadline = time.time() + timeout
    seen = b""
    while time.time() < deadline:
        chunk = port.read(4096)
        if chunk:
            log.extend(chunk)
            seen += chunk
            if needle in seen:
                return True
    return False


def take_the_board(wait_seconds):
    """Returns the held lock file, or exits saying who has it.

    The caller keeps the returned object alive for as long as it is using
    the board: closing it releases the lock."""
    handle = open(LOCK, "w")
    deadline = time.time() + wait_seconds
    while True:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return handle
        except BlockingIOError:
            if time.time() >= deadline:
                sys.exit(f"hil: another run has the board ({LOCK}); "
                         f"waited {wait_seconds:.0f}s")
            time.sleep(2.0)


def offer_filesystem_image(path):
    """Serves a filesystem image, compressed when that is worth doing.

    Returns `(served name, expanded length, compressed length or None)`. The
    expanded length is what the kernel needs — the archive's size says
    nothing about how much memory the image occupies — and the third value
    is what decides whether U-Boot has to expand anything at all."""
    served_dir = os.path.dirname(image.SERVED)
    raw = open(path, "rb").read()
    gz_name = os.path.basename(path) + ".gz"
    gz_target = os.path.join(served_dir, gz_name)
    with gzip.open(gz_target, "wb", compresslevel=9) as f:
        f.write(raw)
    packed = os.path.getsize(gz_target)
    if packed < len(raw) * COMPRESS_IF_UNDER:
        os.chmod(gz_target, 0o644)
        return gz_name, len(raw), packed
    os.remove(gz_target)
    name, size = serve_beside_the_image(path)
    return name, size, None


def ranges_overlap(spans):
    """Refuses a layout where two things the boot loader loads collide.

    Each span is `(name, address, length)`. This exists because the one that
    collided was invisible: the transfer succeeded, the decompression
    succeeded, and the filesystem looked corrupt."""
    ordered = sorted(spans, key=lambda s: s[1])
    for (an, aa, al), (bn, ba, _) in zip(ordered, ordered[1:]):
        if aa + al > ba:
            sys.exit(f"hil: {an} at {aa:#x} ({al} bytes) runs into {bn} at {ba:#x}")


def serve_beside_the_image(path):
    """Copies a program next to the served kernel so U-Boot can fetch it.

    Serving the file that was just handed over, rather than trusting one to
    already be there, is the same lesson as the image's own staleness check:
    a board result from an unknown binary is read as evidence."""
    served_dir = os.path.dirname(image.SERVED)
    name = os.path.basename(path)
    target = os.path.join(served_dir, name)
    if os.path.abspath(target) != os.path.abspath(path):
        shutil.copyfile(path, target)
    os.chmod(target, 0o755)
    return name, os.path.getsize(target)


def boot(bootargs, capture_seconds, out_path, wait_seconds=0.0, program=None, fs_image=None):
    """Power-cycles, loads and boots, and captures the console."""
    log = bytearray()
    steps_ok = True
    held = take_the_board(wait_seconds)
    with serial.Serial(CONSOLE, 115200, timeout=0.2) as port:
        power("POWER_DOWN")
        time.sleep(2.0)
        power("POWER_UP")
        # The autoboot window announces itself, so wait for it rather than
        # flooding the line with newlines and arriving at eighty prompts.
        if not read_until(port, b"Hit any key to stop autoboot", 40, log):
            print("hil: never saw the autoboot prompt", file=sys.stderr)
            steps_ok = False
        send(port, "")
        if not read_until(port, PROMPT, 10, log):
            print("hil: never reached a U-Boot prompt", file=sys.stderr)
            steps_ok = False
        steps = [("dhcp", 30), (f"wget {LOAD_AT} {IMAGE_URL}", 30)]
        if program is not None:
            url = IMAGE_URL.rsplit("/", 1)[0] + "/" + program[0]
            steps.append((f"wget {PROGRAM_AT:#x} {url}", 120))
        if fs_image is not None:
            url = IMAGE_URL.rsplit("/", 1)[0] + "/" + fs_image[0]
            if fs_image[2] is None:
                steps.append((f"wget {IMAGE_AT:#x} {url}", 180))
            else:
                steps.append((f"wget {IMAGE_GZ_AT:#x} {url}", 180))
                steps.append((f"unzip {IMAGE_GZ_AT:#x} {IMAGE_AT:#x}", 180))
        steps.append((f'setenv bootargs "{bootargs}"', 5))
        for command, wait in steps:
            send(port, command)
            if not read_until(port, PROMPT, wait, log):
                print(f"hil: step did not finish: {command}", file=sys.stderr)
                steps_ok = False
            # Nothing more is coming: a prompt can be printed while output
            # is still arriving, and the next command must not be typed into
            # the middle of it.
            while port.read(4096):
                pass
        send(port, f"booti {LOAD_AT} - " + "${fdtcontroladdr}")
        deadline = time.time() + capture_seconds
        while time.time() < deadline:
            log.extend(port.read(4096))
    with open(out_path, "wb") as f:
        f.write(bytes(log))
    held.close()
    return steps_ok


def main():
    ap = argparse.ArgumentParser(description="Boot the VisionFive 2 and capture its console.")
    ap.add_argument("bootargs", help="the kernel command line, added to the standard pair")
    ap.add_argument("--bare", action="store_true",
                    help=f"pass bootargs alone, without {STANDARD_ARGS!r}")
    ap.add_argument("-t", "--seconds", type=float, default=60.0,
                    help="how long to capture after booti (default 60)")
    ap.add_argument("-o", "--out", default="hil.log", help="where to write the log")
    ap.add_argument("-q", "--quiet", action="store_true", help="do not echo the log")
    ap.add_argument("-i", "--image", default=None,
                    help="a filesystem image to fetch into DRAM and mount (minikern.disk=ram)")
    ap.add_argument("-p", "--program", default=None,
                    help="a local program to fetch into DRAM and run instead of the card's")
    ap.add_argument("--no-stage", dest="stage", action="store_false",
                    help="boot whatever is already served, without building")
    ap.add_argument("-w", "--wait", type=float, default=0.0,
                    help="seconds to wait for another run to release the board")
    args = ap.parse_args()
    if args.stage:
        digest = image.stage(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        if digest is None:
            return 2
        print(f"hil: image md5 {digest}")
    bootargs = args.bootargs if args.bare else f"{STANDARD_ARGS} {args.bootargs}"
    program = None
    if args.program:
        program = serve_beside_the_image(args.program)
        bootargs += f" minikern.loadat={PROGRAM_AT:#x}:{program[1]:#x} minikern.runloaded"
        print(f"hil: {program[0]} is {program[1]} bytes, fetched to {PROGRAM_AT:#x}")
    fs = None
    if args.image:
        fs = offer_filesystem_image(args.image)
        bootargs += f" minikern.diskat={IMAGE_AT:#x}:{fs[1]:#x} minikern.disk=ram"
        # **The kernel is one of the things loaded**, and leaving it out of
        # this list cost a board run: a 642 MB image at `0x6000_0000` runs
        # through `0x8200_0000`, so U-Boot decompressed the filesystem over
        # the kernel it was about to boot and answered `Bad Linux RISCV
        # Image magic!`. The archive collision this check was written for
        # was caught; the kernel's own address was not in it. A range check
        # that does not check every range is the same defect twice.
        spans = [("the image", IMAGE_AT, fs[1]),
                 ("the kernel", int(LOAD_AT, 16), KERNEL_ROOM)]
        if program is not None:
            spans.append(("the program", PROGRAM_AT, program[1]))
        if fs[2] is not None:
            spans.append(("the archive", IMAGE_GZ_AT, fs[2]))
        ranges_overlap(spans)
        how = "sent whole" if fs[2] is None else f"expands from {fs[2]} bytes"
        print(f"hil: {fs[0]} is {fs[1]} bytes at {IMAGE_AT:#x}, {how}")
    ok = boot(bootargs, args.seconds, args.out, args.wait, program, fs)
    if not args.quiet:
        text = open(args.out, "rb").read().decode("utf-8", "replace")
        start = text.find("Starting kernel")
        sys.stdout.write(text[start:] if start >= 0 else text[-2500:])
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
