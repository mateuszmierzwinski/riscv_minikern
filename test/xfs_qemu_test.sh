#!/usr/bin/env bash
# A real XFS filesystem, on a real device, read by the kernel.
#
# The filesystem implementation carried over from the ARM kernel with its host
# tests and had never had a device beneath it on this architecture. This is
# the whole stack at once: a controller, a block-size adapter, a buffer cache,
# and XFS — reading a filesystem that `mkfs.xfs` made and `xfs_repair` calls
# clean. The authority on whether the implementation is right is the vendor's
# own tools, not this project's opinion.
#
# It is populated with `mkfs.xfs -p`, a proto file, because mounting one to
# copy files in needs root and this does not.
#
# **The feature set is explicit and that is the point.** Modern mkfs enables
# reflink, rmapbt, sparse inodes and nrext64 by default, and this
# implementation **refuses every feature it does not fully understand** rather
# than reading a filesystem it might misunderstand. So the image is made with
# the set it does support — and the refusals are tested too, further down,
# because "refuses what it should" is as much a requirement as "reads what it
# can".
set -euo pipefail

image="$1"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"

for tool in qemu-system-riscv64 mkfs.xfs xfs_repair; do
    command -v "$tool" >/dev/null || { echo "$tool is not installed" >&2; exit 1; }
done

fail() { echo "FAIL: $1" >&2; exit 1; }

# The feature set this kernel reads: v5 with CRCs and the free-inode btree,
# and nothing newer. Given as two strings rather than one, because mkfs takes
# `-m` and `-i` once each — passing a second of either to override the first
# is simply refused.
MINIMAL_M="crc=1,finobt=1,reflink=0,rmapbt=0,bigtime=0,inobtcount=0"
MINIMAL_I="sparse=0,size=512,nrext64=0"

printf 'hello from a real XFS filesystem on RISC-V\n' > "$tmp/hello.txt"
printf 'second file\n' > "$tmp/notes.txt"
cat > "$tmp/proto" <<PROTO
dummy
0 0
d--755 0 0
hello.txt ---644 0 0 $tmp/hello.txt
notes.txt ---644 0 0 $tmp/notes.txt
sub d--755 0 0
\$
\$
PROTO

# make_image <image> [m-options] [i-options], defaulting to the supported set.
make_image() {
    local img="$1"
    local mopts="${2:-$MINIMAL_M}"
    local iopts="${3:-$MINIMAL_I}"
    dd if=/dev/zero of="$img" bs=1M count=300 status=none
    mkfs.xfs -q -f -m "$mopts" -i "$iopts" -n ftype=1 -b size=4096 -p "$tmp/proto" "$img" ||
        fail "mkfs.xfs refused to make $img"
    xfs_repair -n "$img" >/dev/null 2>&1 || fail "xfs_repair rejects the image we just made"
}

boot() {
    local img="$1"
    local out="$2"
    local status=0
    timeout 120 qemu-system-riscv64 -M virt -smp 4 -m 1G -nographic -bios default \
        -kernel "$image" -drive "file=$img,if=none,format=raw,id=nvm0" \
        -device nvme,serial=mkxfs,drive=nvm0 \
        -append "console=sbi minikern.halt=poweroff" > "$out" 2>&1 || status=$?
    tr -d '\r' < "$out" > "$out.clean" && mv "$out.clean" "$out"
    [ "$status" -eq 0 ] || { cat "$out"; fail "qemu exited $status"; }
    ! grep -qF PANIC "$out" || { cat "$out"; fail "the kernel panicked"; }
}

make_image "$tmp/fs.img"
boot "$tmp/fs.img" "$tmp/mount.log"
cat "$tmp/mount.log"

# The device, presented to the filesystem in **its** block size rather than
# the controller's sectors. Without that bridge the superblock reads (it fits
# in one sector) and the first directory read does not.
grep -qE '^disk: nvme .*, 76800 sectors of 4096 bytes \(300 MiB\)$' "$tmp/mount.log" ||
    fail "the device was not presented in 4 KiB blocks"

# The superblock, read and agreeing with what mkfs made.
grep -qF "xfs: 76800 blocks of 4096 bytes, 4 groups, 512-byte inodes, root 96" "$tmp/mount.log" ||
    fail "the superblock was not read, or disagrees with mkfs"

# The root directory: every entry mkfs put there, and the two that are always
# there. A directory read is what proves the inode and the extent map rather
# than just the superblock.
for entry in "./" "../" "hello.txt" "notes.txt" "sub/"; do
    grep -qE "^xfs:   $entry\$" "$tmp/mount.log" || fail "the root listing lacks $entry"
done
grep -qF "xfs: mounted read-only, 5 entries in /" "$tmp/mount.log" ||
    fail "the entry count is wrong"

# And a file's contents, which is the data path rather than the metadata.
grep -qF 'xfs: /hello.txt is 43 bytes, begins "hello from a real XFS filesystem on RISC-V"' \
    "$tmp/mount.log" || fail "the file's contents or length came back wrong"
echo "ok: a filesystem mkfs.xfs made, mounted off an NVMe drive, listed and read"

# ── what it refuses, which matters as much ─────────────────────────────────
#
# A feature this kernel does not fully understand must be a refusal, not a
# best effort: reading a filesystem whose layout you have guessed at is how
# data is lost quietly. Each of these is made deliberately and must be turned
# away.
# Reflink and reverse mapping are a different case from the rest, and the
# difference is the format's own: they are **read-only compatible**. The
# structures they add are a writer's obligation — a shared extent is read
# exactly like an unshared one — so a reader that never writes may ignore
# them, and must then actually never write.
#
# This asserted a refusal until a real drive arrived. `mkfs.xfs` turns both
# on by default, so refusing them meant the kernel could only read a
# filesystem formatted specially for it, which is not a filesystem reader.
make_image "$tmp/reflink.img" "crc=1,finobt=1,reflink=1,rmapbt=1,bigtime=0,inobtcount=0"
boot "$tmp/reflink.img" "$tmp/reflink.log"
grep -qF "xfs: read-only features" "$tmp/reflink.log" ||
    { cat "$tmp/reflink.log"; fail "the read-only features were not reported"; }
grep -qF "xfs: mounted read-only" "$tmp/reflink.log" ||
    { cat "$tmp/reflink.log"; fail "a reflink filesystem did not mount"; }
grep -qF "hello.txt" "$tmp/reflink.log" ||
    { cat "$tmp/reflink.log"; fail "a reflink filesystem mounted but could not be listed"; }
echo "ok: reflink and rmapbt mount read-only, as the format allows"

make_image "$tmp/sparse.img" "$MINIMAL_M" "sparse=1,size=512,nrext64=0"
boot "$tmp/sparse.img" "$tmp/sparse.log"
grep -qF "xfs: unsupported features" "$tmp/sparse.log" ||
    { cat "$tmp/sparse.log"; fail "sparse inodes were not refused"; }
echo "ok: sparse inodes are refused too"

# And the refusal must name its own reason rather than have it paraphrased:
# an earlier version of the boot path guessed a different cause from the error
# code and printed "a dirty log" over the top of the real message.
grep -qF "the reason is above" "$tmp/sparse.log" ||
    fail "the mount failure paraphrases the filesystem's diagnosis instead of deferring to it"
echo "ok: the kernel defers to the filesystem's own diagnosis"
echo PASS
