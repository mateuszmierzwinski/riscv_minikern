#!/usr/bin/env bash
# User mode: a program in an address space of its own, making syscalls.
#
# This is the step the rest of the kernel exists for, and it is tested with a
# page of hand-written instructions rather than an ELF on purpose. An ELF
# loader on top of a mode switch that has never been entered would be two
# untested things at once — and on the architecture this migrated from, the
# first bug found this way was in the process *start state*, which a loader
# has nothing to do with.
#
# Three properties, each of which a weaker test would miss:
#
#   the program **comes back**. A kernel that reaches user mode once and
#   never returns looks identical, in a log, to one that works — so the
#   program writes twice, with the kernel having resumed it in between;
#
#   the exit status and the **syscall count** both arrive, so "it ran" is
#   measured rather than inferred from one line of output;
#
#   a fault in user code **ends the process and not the kernel**. The second
#   program reads the kernel's own text, which every address space maps —
#   it has to, or the trap handler could not run — and which no user page
#   marks reachable. The kernel reports it, the process dies, and the
#   machine goes on to power itself off.
set -euo pipefail

image="$1"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
log="$tmp/console.log"

command -v qemu-system-riscv64 >/dev/null || { echo "qemu-system-riscv64 is not installed" >&2; exit 1; }

status=0
timeout 90 qemu-system-riscv64 -M virt -smp 4 -m 1G -nographic -bios default -kernel "$image" \
    -append "console=sbi minikern.halt=poweroff minikern.user=1" > "$log" 2>&1 || status=$?
tr -d '\r' < "$log" > "$log.clean" && mv "$log.clean" "$log"
cat "$log"

fail() { echo "FAIL: $1" >&2; exit 1; }

[ "$status" -eq 0 ] || fail "qemu exited $status — the kernel did not finish"
! grep -qF PANIC "$log" || fail "the kernel panicked: a user fault took the kernel with it"

# The program ran, and the kernel came back into it.
grep -qF "hello from user mode on RISC-V" "$log" || fail "user mode printed nothing"
grep -qF "and again, so the kernel came back" "$log" ||
    fail "the kernel reached user mode but never resumed it"

# Measured, not inferred: the status it asked to exit with, and how many
# syscalls it actually made (two writes and an exit).
grep -qF "uspace: hello exited with 7 after 3 syscalls" "$log" ||
    fail "the exit status or the syscall count is wrong"

# A user fault is the process's problem. The address is the kernel's text,
# which is mapped in that address space and not user-reachable.
grep -qF "about to touch the kernel's memory" "$log" || fail "the faulting program did not run"
# The process is named in the message: once there can be more than one user
# process, "a fault happened" is not enough to act on.
# The thread and its stack pointer are named too: with four harts running
# four threads, "a fault happened in this program" is not actionable, and the
# thread's own stack is the first thing to compare the address against.
grep -qE '^uspace: fault in user code \(pid [0-9]+ tid [0-9]+, sp [0-9a-f]+\): load page fault at sepc [0-9a-f]+, stval 0000000080200000$' "$log" ||
    fail "the fault was not reported, or not as a load page fault at the kernel's text"
grep -qF "uspace: faulter exited with -14 after 1 syscalls" "$log" ||
    fail "the faulting process did not end with a fault status"

# And the machine carried on afterwards, which is the difference between a
# user fault and a kernel one.
grep -qF "minikern: powering off" "$log" || fail "the kernel did not survive the user fault"
echo "ok: user mode entered, resumed, exited — and a user fault killed only the process"
echo PASS
