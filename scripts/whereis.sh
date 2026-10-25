#!/usr/bin/env bash
# Names the function containing an address the running board printed.
#
# **The board kernel is a flat image and carries no symbols**, so an address
# in a fault report is a number and nothing more. `minikern_board_elf` is its
# symbol-bearing twin — the same sources and link address, linked as an ELF —
# and that is what this reads. Resolving against the QEMU kernel instead
# gives a plausible symbol rather than the right one, which is worse than no
# answer.
#
# One translation is needed: the symbols sit at the kernel's link address, so
# a direct-map pc (0xffffffc0_______) has its base swapped for 0x40200000.
#
# `eu-readelf` from elfutils does the reading. GNU `nm` and `objdump` cannot
# read this file at all, and there is no `llvm-nm` here.
#
#   scripts/whereis.sh 0xffffffc00020a754 0x4023b0b0
set -euo pipefail

# The monorepo root, five levels up from src/c/experiments/riscv_minikern/scripts.
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../.." && pwd)"
elf="$here/bazel-bin/minikern_board_elf.elf"

if [ $# -eq 0 ]; then
    echo "usage: $(basename "$0") <address> [address...]" >&2
    exit 2
fi
if [ ! -e "$elf" ]; then
    echo "whereis: build it first: bazel build //:minikern_board_elf" >&2
    exit 1
fi
command -v eu-readelf >/dev/null 2>&1 || { echo "whereis: eu-readelf (elfutils) is required" >&2; exit 1; }

syms="$(eu-readelf -s "$elf" | grep FUNC)"
for raw in "$@"; do
    # A direct-map address names the same byte as its identity-mapped alias.
    addr="$raw"
    case "$raw" in
    0xffffffc0*) addr="0x4$(printf '%s' "${raw#0xffffffc0}" | tail -c 7)" ;;
    esac
    printf '%s' "$syms" | awk -v PC="$addr" -v RAW="$raw" '
      { gsub(/:/, "", $2); a = strtonum("0x" $2); sz = $3 + 0; p = strtonum(PC);
        if (a <= p && p < a + sz) { printf "%s -> %s (+%d)\n", RAW, $NF, p - a; found = 1 } }
      END { if (!found) printf "%s -> no function contains it\n", RAW }'
done
