#!/usr/bin/env bash
# A real disk: found, written, read back, and left exactly as it was.
#
# What this proves is not mainly the driver. The buffer cache and the XFS
# implementation came across from the ARM kernel with their host tests and
# have never had a device under them on this architecture — so this is the
# first time anything here has talked to storage at all. When the SD and NVMe
# drivers arrive, a failure will be in those drivers' registers rather than
# somewhere in a filesystem nobody has exercised.
#
# The kernel writes a pattern to the **last** sector and restores it. The
# last, because a filesystem lives at the beginning and a test that scribbles
# on sector 0 destroys what the next test wants to read. The image is checked
# here afterwards: a test that leaves a disk modified has broken the next one,
# and that is worth catching from the outside rather than trusting a report.
#
# `virtio-mmio.force-legacy=false` is needed because QEMU offers the legacy
# (version 1) transport by default, whose queue layout differs. This driver
# refuses legacy and says so, which is how the first attempt here was
# diagnosed: without that message, an emulator full of legacy transports
# looked exactly like a machine with no devices.
set -euo pipefail

image="$1"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
disk="$tmp/disk.img"
log="$tmp/console.log"

command -v qemu-system-riscv64 >/dev/null || { echo "qemu-system-riscv64 is not installed" >&2; exit 1; }

# A known pattern rather than zeroes, so "restored" means restored and not
# merely "zero again".
: > "$disk"
for i in $(seq 1 64); do
    printf 'minikern disk test block %02d%s' "$i" "$(printf '%.0s.' $(seq 1 1000))" >> "$disk"
done
truncate -s 64M "$disk"
before="$(sha256sum < "$disk" | cut -d' ' -f1)"

status=0
timeout 90 qemu-system-riscv64 -M virt -smp 4 -m 1G -nographic -bios default -kernel "$image" \
    -global virtio-mmio.force-legacy=false \
    -drive "file=$disk,if=none,format=raw,id=hd0" -device virtio-blk-device,drive=hd0 \
    -append "console=sbi minikern.halt=poweroff minikern.disktest=1" > "$log" 2>&1 || status=$?
tr -d '\r' < "$log" > "$log.clean" && mv "$log.clean" "$log"
cat "$log"

fail() { echo "FAIL: $1" >&2; exit 1; }

[ "$status" -eq 0 ] || fail "qemu exited $status"
! grep -qF PANIC "$log" || fail "the kernel panicked"

# It found the device through the transport, and read its capacity from the
# device's own configuration space rather than from the tree.
grep -qE '^disk: /soc/virtio_mmio@[0-9a-f]+, 131072 sectors of 512 bytes \(64 MiB\)$' "$log" ||
    fail "the disk was not found, or reported the wrong capacity"
grep -qF "disk: round trip ok" "$log" || fail "the write/read-back round trip failed"

# And the image is byte-for-byte what it was. This is the check that cannot
# be fooled by the kernel's own opinion of itself.
after="$(sha256sum < "$disk" | cut -d' ' -f1)"
[ "$before" = "$after" ] || fail "the disk was left modified: the restore did not work"
echo "ok: a real disk written, read back, and left byte-for-byte unchanged"

# A machine with no disk must say so once and carry on: a kernel that
# netboots has no disk, and that is not a failure.
nodisk="$tmp/nodisk.log"
timeout 90 qemu-system-riscv64 -M virt -m 1G -nographic -bios default -kernel "$image" \
    -append "console=sbi minikern.halt=poweroff" > "$nodisk" 2>&1 || fail "the no-disk boot failed"
tr -d '\r' < "$nodisk" > "$nodisk.clean" && mv "$nodisk.clean" "$nodisk"
grep -qF "disk: none attached" "$nodisk" || { cat "$nodisk"; fail "a machine with no disk did not say so"; }
grep -qF "minikern: powering off" "$nodisk" || fail "the no-disk boot did not finish"
echo "ok: a machine with no disk says so and carries on"
echo PASS
