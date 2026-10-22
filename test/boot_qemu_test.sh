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

boot() {
    local memory="$1"
    local out="$2"
    local extra="${3:-}"
    local status=0
    # No trailing space when there is nothing extra: the kernel echoes its
    # command line verbatim and a test asserts it verbatim.
    local append="console=sbi minikern.halt=poweroff${extra:+ $extra}"
    timeout 60 qemu-system-riscv64 -M virt -smp 4 -m "$memory" -nographic \
        -bios default -kernel "$image" -append "$append" > "$out" 2>&1 || status=$?
    tr -d '\r' < "$out" > "$out.clean" && mv "$out.clean" "$out"
    [ "$status" -eq 0 ] || { cat "$out"; echo "FAIL: with $memory, qemu exited $status" >&2; exit 1; }
}

status=0
boot 1G "$log"
cat "$log"

fail() { echo "FAIL: $1" >&2; exit 1; }
expect() { grep -qF -- "$1" "$log" || fail "the console lacks: $1"; }

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
# The frame allocator over the memory the tree described, with the kernel
# image, the device tree and the page-table arena reserved.
grep -qE '^mem: 261[0-9]{3} of 262144 pages free \(10[0-9]{2} MiB of 1024 MiB\)$' "$log" ||
    { echo "FAIL: the frame allocator did not come up over 1 GiB" >&2; exit 1; }
echo "ok: firmware → S-mode → device tree → sv39 → the kernel's own UART → memory → power-off"

# **Eight gibibytes**, which is a VisionFive 2 configuration and the case the
# 32-bit reader this kernel grew from could not express at all: it refused a
# two-cell value whose high word was set, so a board like this would have
# booted believing it had no memory. 2 Mi frames is also 256 KiB of bitmap,
# which is sized for the largest board so one image runs on every one.
big="${log%.log}-8g.log"
boot 8G "$big"
grep -qE '^mem: 8192 MiB at 0000000080000000$' "$big" ||
    { cat "$big"; echo "FAIL: the 8 GiB memory node was not read" >&2; exit 1; }
grep -qE '^mem: 209[0-9]{4} of 2097152 pages free \(818[0-9] MiB of 8192 MiB\)$' "$big" ||
    { cat "$big"; echo "FAIL: the frame allocator did not come up over 8 GiB" >&2; exit 1; }
echo "ok: 8 GiB of RAM is read and tracked — 2 Mi frames, the case 32 bits could not express"

# ── interrupts and the tick ────────────────────────────────────────────────
#
# A timer that is armed and one that is *delivering* look identical in a boot
# log, and on the architecture this migrated from that difference was a real
# bug: a core whose interrupt controller had not been initialised ticked for
# ever without ever entering the kernel. So the kernel is made to wait for a
# known number of ticks and report how long they took by the counter.
ticks="${log%.log}-ticks.log"
boot 1G "$ticks" "minikern.ticks=50"

# The PLIC, and the part of it nobody can guess: a hart's **context** is a
# position in the controller's own list, not its hart id. On this machine
# hart 0's supervisor context is 1, because context 0 is its machine-mode
# context — the firmware's. A kernel that assumed they matched would write
# another context's enable bits.
grep -qE '^plic: at [0-9a-f]{16}, hart [0-9]+ uses context [0-9]+$' "$ticks" ||
    { cat "$ticks"; echo "FAIL: the PLIC was not found, or did not report its context" >&2; exit 1; }
grep -qE '^timer: [0-9]+ Hz counter, 100 ticks a second$' "$ticks" ||
    { cat "$ticks"; echo "FAIL: the timer did not read its frequency from the tree" >&2; exit 1; }

# 50 ticks at 100 Hz is half a second. The window is wide enough for a loaded
# host and narrow enough that a clock running at the wrong rate fails: a
# frequency misread by a factor of two would land at 250 ms or 1000 ms.
read -r got ms < <(sed -n 's/^tick: \([0-9]*\) ticks in \([0-9]*\) ms.*/\1 \2/p' "$ticks")
[ "${got:-0}" = "50" ] ||
    { cat "$ticks"; echo "FAIL: only ${got:-0} of 50 ticks arrived" >&2; exit 1; }
[ "${ms:-0}" -ge 400 ] && [ "${ms:-0}" -le 700 ] ||
    { cat "$ticks"; echo "FAIL: 50 ticks took ${ms} ms, which is not 100 Hz" >&2; exit 1; }
echo "ok: the tick is delivered and keeps time — 50 ticks in ${ms} ms at 100 Hz"
echo PASS
