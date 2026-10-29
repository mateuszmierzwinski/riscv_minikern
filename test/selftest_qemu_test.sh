#!/usr/bin/env bash
# The trap path, proved by taking deliberate faults and surviving them.
#
# Nothing else can be built on a trap vector that has not been exercised, and
# a trap path that is wrong does not announce itself — it produces a second,
# unrelated fault somewhere later. So each fault is taken on purpose, and the
# kernel checks the cause *and* the faulting address against what it asked
# for. A test whose instruction does **not** fault fails too: that is the more
# dangerous outcome, because it means the kernel is running something it
# believed was forbidden.
#
# Each case also runs on its own, because that is what made the first bug here
# findable: with all four in one boot, the failure appeared in the test after
# the one that actually broke.
set -euo pipefail

image="$1"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"

command -v qemu-system-riscv64 >/dev/null || { echo "qemu-system-riscv64 is not installed" >&2; exit 1; }

run() {
    # Separate statements on purpose: in a single `local a=$1 b=$a`, bash
    # expands every word *before* making any assignment, so `b` sees the old
    # `a` — which here meant all four runs wrote to one file named ".log" and
    # the per-case logs never existed.
    local which="$1"
    local log="$tmp/$which.log"
    local status=0
    timeout 60 qemu-system-riscv64 -M virt -m 1G -nographic -bios default -kernel "$image" \
        -append "console=sbi minikern.halt=poweroff minikern.selftest=$which" > "$log" 2>&1 || status=$?
    tr -d '\r' < "$log" | sed -E 's/^\[[0-9:.]+\] [A-Z] //' > "$log.clean" && mv "$log.clean" "$log"
    [ "$status" -eq 0 ] || { cat "$log"; echo "FAIL: $which: qemu exited $status (124 = the kernel never got back)" >&2; exit 1; }
    ! grep -qF "PANIC" "$log" ||
        { cat "$log"; echo "FAIL: $which: the kernel panicked, so the fault was not the expected one" >&2; exit 1; }
    grep -qF "selftest: all faults taken and survived" "$log" ||
        { cat "$log"; echo "FAIL: $which did not report success" >&2; exit 1; }
    echo "ok: $which"
}

# One at a time, then all together in one boot — which is the case that
# catches a handler that corrupts state it has already used.
for which in illegal load store break rotext; do
    run "$which"
done
run all

# The causes must be the right ones, not merely *some* fault. An access fault
# and a page fault are different diagnoses — nothing answered on the bus,
# versus nothing in the table — and a kernel that reports the wrong one sends
# its reader to the wrong place. Translation is on by the time these run, so
# an unreachable address is a **page** fault; the kernel asks its own MMU
# rather than hard-coding either answer.
log="$tmp/all.log"
cat "$log"
grep -qF "illegal instruction at 0000000030102573" "$log" ||
    { echo "FAIL: the M-mode CSR read did not report the instruction in stval" >&2; exit 1; }
grep -qF "load page fault at 3000000000000000" "$log" ||
    { echo "FAIL: the load fault did not report a page fault at the faulting address" >&2; exit 1; }
grep -qF "store page fault at 3000000000000008" "$log" ||
    { echo "FAIL: the store fault did not report a page fault at the faulting address" >&2; exit 1; }
grep -qE "breakpoint at ffffffc0[0-9a-f]{8}" "$log" ||  # the kernel runs in its own half
    { echo "FAIL: the breakpoint did not report where it happened" >&2; exit 1; }
echo "ok: every cause and faulting address was reported correctly"

# Paging must actually be on for the above to mean what it says, and the
# kernel's own text must be unwritable — proved by writing to it, not asserted.
grep -qE "^mmu: sv39 on, [0-9]+ table pages, kernel text read-execute$" "$log" ||
    { echo "FAIL: translation was not enabled, so the page faults above prove less" >&2; exit 1; }
grep -qF "a store into the kernel's own text faulted as expected: store page fault at ffffffc000200000" "$log" ||
    { echo "FAIL: the kernel's text was writable, or the attempt was not reported" >&2; exit 1; }
echo "ok: sv39 is on and the kernel's own text is not writable"

# A name that does not exist must say so rather than silently running nothing,
# which is how a test suite comes to pass while testing nothing at all.
timeout 60 qemu-system-riscv64 -M virt -m 1G -nographic -bios default -kernel "$image" \
    -append "console=sbi minikern.halt=poweroff minikern.selftest=nosuchtest" > "$tmp/bogus.log" 2>&1
grep -qF 'selftest: no test called "nosuchtest"' "$tmp/bogus.log" ||
    { cat "$tmp/bogus.log"; echo "FAIL: an unknown test name was silently ignored" >&2; exit 1; }
echo "ok: an unknown test name is refused rather than ignored"
echo PASS
