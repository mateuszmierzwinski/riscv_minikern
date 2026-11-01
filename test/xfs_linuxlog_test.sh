#!/usr/bin/env bash
# The log replay, judged against a log Linux wrote (test/testdata/
# linux_dirty_log.img.xz, made by scripts/make_linux_log.sh): the image is
# mounted by this kernel's XFS, which must replay the log and come up
# writable, the files must be as Linux's last changes left them, a write
# goes over the result, and `xfs_repair -n` must find nothing.
set -euo pipefail

writecheck="$1"
fixture="$2"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
for tool in xz xfs_repair; do
    command -v "$tool" >/dev/null || { echo "$tool is not installed" >&2; exit 1; }
done
img="$tmp/linux.img"
xz -dc "$fixture" > "$img"
if xfs_repair -n "$img" > "$tmp/before" 2>&1; then
    echo "FAIL: the fixture's log is not dirty; it no longer tests replay" >&2
    exit 1
fi
XFS_WRITECHECK_LINUXLOG=1 "$writecheck" "$img" linuxlog || { echo "FAIL: the replayed filesystem" >&2; exit 1; }
if ! xfs_repair -n "$img" > "$tmp/after" 2>&1; then
    cat "$tmp/after"
    echo "FAIL: xfs_repair finds the replayed filesystem damaged" >&2
    exit 1
fi
echo "PASS: a log Linux left dirty is replayed, and xfs_repair finds nothing"
