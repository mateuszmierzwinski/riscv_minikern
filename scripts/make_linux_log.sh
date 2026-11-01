#!/usr/bin/env bash
# Makes test/testdata/linux_dirty_log.img.xz: an XFS filesystem whose log
# Linux left dirty — the fixture the log replay is judged against.
#
# **Why a guest, and why no root.** Only Linux writes a Linux log, and
# mounting a filesystem needs root on the host. QEMU needs neither: the
# host's own kernel boots with a two-file initramfs — `xfs.ko` and a static
# Go init (test/linuxlog/init) — mounts the image on a virtio disk with
# quotas on, creates, writes, chowns and removes files, then forces the log
# out and shuts the filesystem down (`XFS_IOC_GOINGDOWN`, LOGFLUSH) without
# writing the metadata back. What is left is the log Linux wrote and
# metadata that is behind it.
#
# Needs qemu-system-x86_64 (KVM is used when /dev/kvm is usable), cpio, xz,
# mkfs.xfs, a readable /usr/lib/modules/$(uname -r)/vmlinuz, and an XFS
# module for that kernel. The output records the kernel that wrote it.
set -euo pipefail

project="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$project/test/testdata/linux_dirty_log.img.xz"
kernel="/usr/lib/modules/$(uname -r)/vmlinuz"
module="$(modinfo -n xfs)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

(cd "$project" && bazel build //test/linuxlog/init:init)
init_bin="$(cd "$project" && bazel cquery --output=files \
    //test/linuxlog/init:init 2>/dev/null)"
mkdir -p "$work/root/dev"
cp "$project/../../../../$init_bin" "$work/root/init"
case "$module" in
*.xz) xz -dc "$module" > "$work/root/xfs.ko" ;;
*) cp "$module" "$work/root/xfs.ko" ;;
esac
(cd "$work/root" && find . | cpio -o -H newc 2>/dev/null) > "$work/initrd.cpio"

truncate -s 300M "$work/log.img"
mkfs.xfs -q -f "$work/log.img"
accel=()
[ -w /dev/kvm ] && accel=(-enable-kvm)
timeout 300 qemu-system-x86_64 "${accel[@]}" -smp 2 -m 1024 -nographic -no-reboot \
    -kernel "$kernel" -initrd "$work/initrd.cpio" \
    -append "console=ttyS0 panic=-1 quiet" \
    -drive "file=$work/log.img,format=raw,if=virtio" | tee "$work/console.txt" | grep -a linuxlog
grep -aq "log flushed, filesystem shut down" "$work/console.txt" || {
    echo "make_linux_log: the guest did not finish" >&2
    exit 1
}
xz -9c "$work/log.img" > "$out"
echo "make_linux_log: $(uname -r) wrote $out"
