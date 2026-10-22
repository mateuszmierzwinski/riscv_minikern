#!/usr/bin/env bash
# The kernel boots on QEMU's virt machine, under real SBI firmware.
#
# This is the whole chain a RISC-V board uses: firmware (OpenSBI, which QEMU
# bundles) loads the payload at 0x80200000, enters it in S-mode with the hart
# id in a0 and a device tree in a1, and the kernel reads that tree to learn
# what machine it is on. Nothing here is emulator-specific: the same image and
# the same path run on a VisionFive 2 under U-Boot's SBI.
#
# The test passes when QEMU exits **by itself**, which means the kernel asked
# the firmware to power the machine off and the firmware did it. That is worth
# noting against the kernel this replaces: there, PSCI's SYSTEM_OFF was
# refused when booted through U-Boot, so its own boot test could not wait for
# an exit and had to grep for a line and then kill the emulator.
set -euo pipefail

image="$1"
log="${TEST_TMPDIR:-$(mktemp -d)}/console.log"

command -v qemu-system-riscv64 >/dev/null || { echo "qemu-system-riscv64 is not installed" >&2; exit 1; }

status=0
timeout 60 qemu-system-riscv64 -M virt -smp 4 -m 1G -nographic \
    -bios default -kernel "$image" \
    -append "console=sbi minikern.halt=poweroff" > "$log" 2>&1 || status=$?
# The console ends its lines with a carriage return, which an anchored
# pattern would otherwise have to match. Strip them once, here.
tr -d '\r' < "$log" > "$log.clean" && mv "$log.clean" "$log"
cat "$log"

fail() { echo "FAIL: $1" >&2; exit 1; }
expect() { grep -qF -- "$1" "$log" || fail "the console lacks: $1"; }

[ "$status" -eq 0 ] || fail "qemu exited $status (124 = hang, so no power-off)"

# The firmware ran and handed over.
expect "OpenSBI"
expect "minikern"
expect "RISC-V 64, rv64gc"

# It knows which hart it is on — and the banner prints it because on a
# machine with a monitor core (sifive_u, and the JH7110 with its S7 beside
# four U74s) the boot hart is 1, not 0.
grep -qE '^minikern .*, hart [0-9]+$' "$log" || fail "no hart id in the banner"

# The firmware's own capabilities, probed rather than assumed.
grep -qE '^sbi: timer yes, hart-state yes, reset yes' "$log" ||
    fail "the firmware did not report the extensions this kernel needs"

# The device tree, which is the only description of the machine there is.
grep -qE '^boot: device tree at [0-9a-f]{16}, [0-9]+ bytes$' "$log" ||
    fail "it did not read a device tree"
# 1 GiB was asked of the emulator, and the memory node must say so — a
# two-cell address and size, which the 32-bit reader this grew from would
# have refused outright for anything above 4 GiB.
grep -qE '^mem: 1024 MiB at 0000000080000000$' "$log" ||
    fail "the memory node was not read as 1024 MiB at 0x80000000"
expect 'cmdline: "console=sbi minikern.halt=poweroff"'
# The kernel finds its own UART in the device tree and takes the console
# over from the firmware — which is the pattern every later driver uses: ask
# the tree what the machine has, rather than being compiled for one board.
# Everything printed after this line came out of the kernel's own driver.
grep -qE '^uart: /soc/serial@[0-9a-f]+ at [0-9a-f]{16}, taken over from sbi$' "$log" ||
    { echo "FAIL: it did not find and adopt a 16550 from the device tree" >&2; exit 1; }
expect "minikern: boot complete"
expect "minikern: powering off"
! grep -qF "refused to power off" "$log" || fail "the firmware refused the power-off"
# Translation is on, and the lines above it were printed through the UART the
# kernel mapped itself — so the device mapping works too.
grep -qE '^mmu: sv39 on, [0-9]+ table pages, kernel text read-execute$' "$log" ||
    { echo "FAIL: translation was not enabled" >&2; exit 1; }
echo "ok: firmware → S-mode → device tree → sv39 → the kernel's own UART → power-off"
echo PASS
