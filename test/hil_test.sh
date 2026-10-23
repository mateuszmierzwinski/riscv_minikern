#!/usr/bin/env bash
# The board suite, as a Bazel target.
#
# **Hardware in the loop as the ordinary check, not the last one.** The
# emulator agrees with whatever you believed — most of `CLAUDE.md` §5 is a
# list of defects it hid — so this is the gate that matters, and it has to
# be one command to be used.
#
# `manual`, and more emphatically than the QEMU targets: those need a host
# tool, this needs **the physical board** attached to this machine, powered
# through the operator's relay and serving its image over HTTP. A wildcard
# build must never reach for it.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

for dev in "${MINIKERN_CONSOLE:-/dev/ttyUSB0}" "${MINIKERN_RELAY:-/dev/ttyACM0}"; do
    if [ ! -c "$dev" ]; then
        echo "hil: $dev is not there — is the board attached?" >&2
        exit 2
    fi
done

exec python3 "$here/hil/suite.py" -o "${TEST_TMPDIR:-/tmp}/minikern-hil" "$@"
