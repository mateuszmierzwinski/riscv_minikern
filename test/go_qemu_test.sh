#!/usr/bin/env bash
# A stock gc Go binary, running unchanged.
#
# This is the project's goal, and the test is written to be hard to fool.
# The binary is built by the **ordinary Go rules** for `linux/riscv64` — no
# build tags, nothing conditional on this kernel, no awareness that this
# kernel exists. It is then read off an XFS filesystem on an NVMe drive and
# run, and what it prints is checked.
#
# Printing "hello" would prove only a loader. What is asserted here is the
# machinery that needs the kernel to be *correct* rather than merely
# present:
#
#   - the garbage collector, which means mmap, munmap and madvise over a
#     heap that grows and shrinks;
#   - eight goroutines doing real work at once, which means clone;
#   - a mutex contended by all of them, which means futex waiting **and**
#     waking — the half that was wrong here for a day;
#   - channels, which means the runtime's own scheduler on top of ours;
#   - a 50 ms sleep that really takes 50 ms, which means nanosleep and
#     clock_gettime agree with each other and with the hardware;
#   - a panic recovered, which means the runtime can walk its own stack;
#   - and exit status 0, which means it got to the end.
set -euo pipefail

image="$1"
program="$2"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
log="$tmp/console.log"

for tool in qemu-system-riscv64 mkfs.xfs; do
    command -v "$tool" >/dev/null || { echo "$tool is not installed" >&2; exit 1; }
done

fail() { echo "FAIL: $1" >&2; exit 1; }

install -m 755 "$program" "$tmp/init"
cat > "$tmp/proto" <<PROTO
dummy
0 0
d--755 0 0
init ---755 0 0 $tmp/init
\$
PROTO

dd if=/dev/zero of="$tmp/fs.img" bs=1M count=300 status=none
mkfs.xfs -q -f -m crc=1,finobt=1,reflink=0,rmapbt=0,bigtime=0,inobtcount=0 \
    -i sparse=0,size=512,nrext64=0 -n ftype=1 -b size=4096 -p "$tmp/proto" "$tmp/fs.img" ||
    fail "mkfs.xfs refused to make the image"

status=0
# Four harts with multi-threaded TCG: the ABI is exercised on the machine
# this kernel actually runs, and the emulator is not made to pretend four
# cores onto one host thread — which makes everything about four times
# slower and tells nothing extra about the ABI.
timeout 300 qemu-system-riscv64 -M virt -smp 4 -accel tcg,thread=multi -m 2G -nographic \
    -bios default -kernel "$image" \
    -drive "file=$tmp/fs.img,if=none,format=raw,id=nvm0" -device nvme,serial=mkgo,drive=nvm0 \
    -append "console=sbi minikern.halt=poweroff minikern.init=1" > "$log" 2>&1 || status=$?
tr -d '\r' < "$log" > "$log.clean" && mv "$log.clean" "$log"
cat "$log"

[ "$status" -eq 0 ] || fail "qemu exited $status"
! grep -qF PANIC "$log" || fail "the kernel panicked"

# A Go runtime's own fatal errors, which are how this fails in practice: it
# aborts during initialisation long before main when something it needs is
# refused, and the message names what.
! grep -qF "fatal error:" "$log" || fail "the Go runtime aborted: $(grep -m1 'fatal error:' "$log")"
! grep -qF "runtime:" "$log" || fail "the Go runtime complained: $(grep -m1 'runtime:' "$log")"

# **Every syscall it made is implemented.** A missing one may be survivable
# — a runtime often has a fallback — but it is a gap, and a test that lets
# one through hides the next one too.
! grep -qE '^syscall: [0-9]+ is not implemented$' "$log" ||
    fail "it needed a syscall this kernel does not have: $(grep -m1 'not implemented' "$log")"

# It is the real toolchain's output, on the architecture this migrated to.
grep -qE '^go: go1\.[0-9.]+ on linux/riscv64$' "$log" ||
    fail "the binary did not report itself as linux/riscv64 gc Go"

# The garbage collector ran, more than once, over a heap of megabytes.
grep -qE '^go: allocated [0-9]+ MiB total, [1-9][0-9]* collections, 32 blocks kept$' "$log" ||
    fail "the heap or the collector did not behave"

# Arithmetic, to a value that cannot be produced by accident.
grep -qF "go: sum of primes below 200000 is 1709600813" "$log" ||
    fail "a long computation gave the wrong answer"

# Eight goroutines, each on its own work, both ends of the range checked.
grep -qF "go: 8 goroutines finished, first=21171191 last=37583560" "$log" ||
    fail "parallel goroutines did not all finish with the right results"

# The contended mutex: 8 goroutines x 2000 increments, and **every one
# landed**. A lost futex wake shows up here as a hang; a lost update as a
# number below 16000.
grep -qF "go: contended counter reached 16000" "$log" ||
    fail "the contended counter is wrong, so a futex wake or an update was lost"

grep -qF "go: channel values [1 4 9 16 25 36]" "$log" ||
    fail "values did not come back through channels correctly"

# The sleep really slept. "true" is the program's own comparison against
# 50 ms, so a clock that does not move fails here rather than silently.
# **Both bounds.** The lower one catches a broken clock; the upper one
# catches a kernel that made the runtime wait — and that is the half that
# found two real bugs. A 50 ms sleep took 950 ms twice over: once because
# the tick was 100 Hz and every sleep was rounded up to it, and once because
# the runtime's own way of interrupting its poller early was ignored. Neither
# is visible to a test that only asks "was it at least 50 ms?".
grep -qE '^go: slept for [0-9]+ms \(at least 50ms: true, under 500ms: true\)$' "$log" ||
    fail "the sleep was too short (a broken clock) or far too long (the kernel made it wait)"

grep -qF "go: recovered from assignment to entry in nil map" "$log" ||
    fail "panic and recover did not work"

# **A fault recovered**, which is the kernel's half: a nil pointer
# dereference is a page fault, and it only becomes a Go panic if the kernel
# delivers SIGSEGV with the faulting address in a frame the runtime can
# read. Without signals the process dies here instead.
grep -qF "go: recovered from a fault: runtime error: invalid memory address or nil pointer dereference" "$log" ||
    fail "a nil pointer dereference did not become a recoverable panic — signal delivery is broken"

# And the program is healthy afterwards, which is what signal **return**
# proves: the handler ran, the runtime unwound, and unrelated code carried on.
grep -qE '^go: still running after the fault, 76127 primes below 1000$' "$log" ||
    fail "it did not survive the fault, or came back with the wrong state"

# No handler's frame was overwritten while it ran. Zero is the only
# acceptable number: a frame that comes back changed hands ordinary code a
# register set that is nearly right, and the failures that causes look like
# anything but signals. A second signal arriving inside a handler used to
# restart its frame at the top of the alternate stack, on top of the outer
# handler's.
grep -qE '^signal: [0-9]+ delivered, 0 frames overwritten$' "$log" ||
    fail "a signal frame was overwritten while its handler ran: $(grep -m1 'frames overwritten' "$log")"

# No goroutine leaked, and it reached its own last line.
#
# The duration is matched loosely on purpose: Go prints "56ms" or "1.022s"
# depending on how long it took, and under load — the export script runs the
# whole suite at once — it takes seconds. An exact unit here failed as
# "goroutines leaked", which is the wrong diagnosis attached to the right
# assertion, and that is worse than no assertion.
grep -qE '^go: 1 goroutines at the end \(1 at the start\), total time .+$' "$log" ||
    fail "goroutines leaked, or the final line is missing"
grep -qF "go: done" "$log" || fail "it did not reach its end"
grep -qE '^init: /init exited with 0 after [0-9]+ syscalls \([0-9]+ stale-translation faults\)$' "$log" ||
    fail "it did not exit cleanly"
echo "ok: a stock gc Go binary for linux/riscv64, unchanged, off a filesystem — threads, GC, channels and all"
echo PASS
