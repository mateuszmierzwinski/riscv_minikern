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
    # shellcheck disable=SC2086 # STEPS is a word list on purpose
    "$writecheck" "$img" ${STEPS:-} || fail "the writes did not all succeed on the $label image"
    if ! xfs_repair -n "$img" > "$tmp/$label.repair" 2>&1; then
        cat "$tmp/$label.repair"
        fail "xfs_repair finds the $label image damaged after the writes"
    fi
    rm -f "$img"
    echo "ok: $label — xfs_repair finds nothing after the writes"
}

# check_rt <label> <rt extent size>: the same, with a realtime device beside
# it — the bitmap, the summary and sb_frextents are judged with -r.
check_rt() {
    local label="$1" extsize="$2"
    local img="$tmp/$label.img" rt="$tmp/$label.rt"
    rm -f "$img" "$rt"
    truncate -s 300M "$img"
    truncate -s 64M "$rt"
    mkfs.xfs -q -f -r rtdev="$rt",extsize="$extsize" "$img" || fail "mkfs.xfs refused the $label image"
    XFS_WRITECHECK_RT="$rt" "$writecheck" "$img" realtime || fail "the realtime writes did not all succeed on $label"
    if ! xfs_repair -n -r "$rt" "$img" > "$tmp/$label.repair" 2>&1; then
        cat "$tmp/$label.repair"
        fail "xfs_repair finds the $label image damaged after the realtime writes"
    fi
    rm -f "$img" "$rt"
    echo "ok: $label — xfs_repair finds nothing after the realtime writes"
}

check_rt realtime4k 4k
check_rt realtime16k 16k

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
# 1024-byte inodes: a shortform directory whose fork is past the 412 bytes
# this reader caches is refused per inode, and the rest still works.
{
    printf 'dummy\n0 0\nd--755 0 0\n'
    echo 'big d--755 0 0'
    for i in $(seq -w 1 25); do echo "name-of-entry-$i ---644 0 0 /dev/null"; done
    echo '$'
    echo 'beside d--755 0 0'
    echo 's1 ---644 0 0 /dev/null'
    echo '$'
    echo '$'
} > "$tmp/bigproto"
# Inodes over 512 bytes: this writer keeps a file within the extents the
# reader's 412-byte fork cache holds (xfs_data_extents_cap), so the steps
# that build b-tree forks or directories of thousands of blocks are refused
# there by design; these are the ones that fit, and btreefile checks the
# refusal itself leaves nothing behind.
large="files dir many truncate unlink attr rename symlink attrfile btreefile holes eofzero uncommitted bigfork"
STEPS="$large" check bigfork -i size=1024 -p "$tmp/bigproto"
STEPS="$large" check inodes1k -i size=1024
STEPS="$large" check inodes2k -i size=2048
echo PASS
