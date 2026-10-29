#!/usr/bin/env bash
# The XFS writer, judged by the vendor's checker on filesystems mkfs.xfs made.
#
# `xfs_writecheck` mounts an image, does one of everything the writer does and
# flushes; `xfs_repair -n` then has to find nothing. Two images: the minimal
# feature set this writer started with, and **mkfs.xfs's defaults** — reflink
# and the reverse-map tree on — which is what any drive formatted without
# special options carries, and so what the writer has to keep consistent.
set -euo pipefail

writecheck="$1"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"

for tool in mkfs.xfs xfs_repair; do
    command -v "$tool" >/dev/null || { echo "$tool is not installed" >&2; exit 1; }
done

fail() { echo "FAIL: $1" >&2; exit 1; }

# check <label> [mkfs options...]
check() {
    local label="$1"
    shift
    local img="$tmp/$label.img"
    rm -f "$img"
    truncate -s 300M "$img"
    mkfs.xfs -q -f "$@" "$img" || fail "mkfs.xfs refused the $label image"
    "$writecheck" "$img" || fail "the writes did not all succeed on the $label image"
    if ! xfs_repair -n "$img" > "$tmp/$label.repair" 2>&1; then
        cat "$tmp/$label.repair"
        fail "xfs_repair finds the $label image damaged after the writes"
    fi
    rm -f "$img"
    echo "ok: $label — xfs_repair finds nothing after the writes"
}

check minimal -m crc=1,finobt=1,reflink=0,rmapbt=0,bigtime=0,inobtcount=0 -i sparse=0,nrext64=0
check defaults
check sector4k -s size=4096

# Files `mkfs` put there itself, through a proto file: chunks filled the way
# Linux fills them, so the full ones are missing from the free-inode tree,
# and the steps read every file back before removing them all. The payload
# is the board fixtures' — bytes `i % 256` — made without any interpreter.
LC_ALL=C awk 'BEGIN { for (i = 0; i < 65536; i++) printf "%c", i % 256 }' > "$tmp/payload"
{
    printf 'dummy\n0 0\nd--755 0 0\n'
    for i in $(seq 1 200); do echo "p$i ---644 0 0 $tmp/payload"; done
    echo '$'
} > "$tmp/proto"
check fromproto -p "$tmp/proto"
echo PASS
