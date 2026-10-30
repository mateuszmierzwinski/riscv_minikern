#!/usr/bin/env python3
"""Runs commands at the board's U-Boot prompt, and optionally watches the
boot that follows without interrupting it.

    uboot.py 'printenv bootcmd' 'setenv bootcmd ...' saveenv
    uboot.py --unattended -t 90 -o local.log     # power on, touch nothing

The first form power-cycles, catches the autoboot countdown exactly as
board.py does, types each command (waiting for the prompt after it), prints
what came back, and powers off. The second is the proof of an unattended
boot: power on and capture whatever the board does on its own. Both hold
the board's lock, so they cannot collide with a board.py run.
"""

import argparse
import sys
import time

import serial

import board


def run_commands(commands, log):
    held = board.take_the_board(0.0)
    with serial.Serial(board.CONSOLE, 115200, timeout=0.2) as port:
        board.power("POWER_DOWN")
        time.sleep(2.0)
        board.power("POWER_UP")
        if not board.read_until(port, b"Hit any key to stop autoboot", 40, log):
            print("uboot: never saw the autoboot prompt", file=sys.stderr)
            return 1
        if not board.stop_autoboot(port, log):
            print("uboot: never reached a U-Boot prompt", file=sys.stderr)
            return 1
        for command in commands:
            board.send(port, command)
            if not board.read_until(port, board.PROMPT, 30, log):
                print(f"uboot: no prompt after: {command}", file=sys.stderr)
                return 1
        board.power("POWER_DOWN")
    held.close()
    return 0


def unattended(seconds, out_path):
    held = board.take_the_board(0.0)
    log = bytearray()
    with serial.Serial(board.CONSOLE, 115200, timeout=0.2) as port:
        board.power("POWER_DOWN")
        time.sleep(2.0)
        board.power("POWER_UP")
        deadline = time.time() + seconds
        while time.time() < deadline:
            log.extend(port.read(4096))
    held.close()
    with open(out_path, "wb") as out:
        out.write(bytes(log))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("commands", nargs="*", help="U-Boot commands, run in order")
    ap.add_argument("--unattended", action="store_true",
                    help="power on and capture without typing anything")
    ap.add_argument("-t", "--seconds", type=float, default=90.0,
                    help="how long to capture an unattended boot")
    ap.add_argument("-o", "--out", default="uboot.log", help="where to write the log")
    args = ap.parse_args()
    if args.unattended:
        return unattended(args.seconds, args.out)
    log = bytearray()
    rc = run_commands(args.commands, log)
    with open(args.out, "wb") as out:
        out.write(bytes(log))
    text = bytes(log).decode("utf-8", "replace")
    print(text[text.rfind("Hit any key"):])
    return rc


if __name__ == "__main__":
    sys.exit(main())
