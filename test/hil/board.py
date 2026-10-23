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
import os
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


def boot(bootargs, capture_seconds, out_path, wait_seconds=0.0):
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
        port.write(b"\r\n")
        port.flush()
        if not read_until(port, PROMPT, 10, log):
            print("hil: never reached a U-Boot prompt", file=sys.stderr)
            steps_ok = False
        for command, wait in (("dhcp", 30),
                              (f"wget {LOAD_AT} {IMAGE_URL}", 30),
                              (f'setenv bootargs "{bootargs}"', 5)):
            port.write(command.encode() + b"\r\n")
            port.flush()
            if not read_until(port, PROMPT, wait, log):
                print(f"hil: step did not finish: {command}", file=sys.stderr)
                steps_ok = False
        port.write(f"booti {LOAD_AT} - ".encode() + b"${fdtcontroladdr}\r\n")
        port.flush()
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
    ok = boot(bootargs, args.seconds, args.out, args.wait)
    if not args.quiet:
        text = open(args.out, "rb").read().decode("utf-8", "replace")
        start = text.find("Starting kernel")
        sys.stdout.write(text[start:] if start >= 0 else text[-2500:])
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
