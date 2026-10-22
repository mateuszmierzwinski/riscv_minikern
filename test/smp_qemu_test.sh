#!/usr/bin/env bash
# Four harts, and a Go program using all of them.
#
# What it proves is **structural**, not timing: every hart enumerated from
# the device tree, started through the firmware's hart-state interface, and
# then actually running threads. That last one matters because the
# equivalent check on the architecture this migrated from was satisfiable by
# a core that had never left its idle loop — "the hart is up" and "the hart
# ran something" are different claims, and only the second is worth
# anything.
#
# The run uses `-accel tcg,thread=multi`. Without it QEMU round-robins every
# vCPU onto one host thread, so no amount of correct kernel work can produce
# a speedup — the emulator's threading model is the limit, and measuring
# through it would be measuring the wrong thing. Measured here: 1.6x for 8
# units of work on 4 harts with multi-threaded TCG, and 1.1x without. The
# ratio is **printed and not asserted**: it depends on how busy the host is,
# and a gate that fails under load would be a gate nobody trusts.
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
timeout 400 qemu-system-riscv64 -M virt -smp 4 -accel tcg,thread=multi -m 2G -nographic \
    -bios default -kernel "$image" \
    -drive "file=$tmp/fs.img,if=none,format=raw,id=nvm0" -device nvme,serial=mksmp,drive=nvm0 \
    -append "console=sbi minikern.halt=poweroff minikern.init=1" > "$log" 2>&1 || status=$?
tr -d '\r' < "$log" > "$log.clean" && mv "$log.clean" "$log"
cat "$log"

[ "$status" -eq 0 ] || fail "qemu exited $status"
! grep -qF PANIC "$log" || fail "the kernel panicked"
! grep -qF "fatal error:" "$log" || fail "the Go runtime aborted"

# Every hart in the tree, found by its `compatible` rather than counted —
# and the boot hart named, because it is **not necessarily hart 0**: QEMU
# picks one by lottery and this SoC's hart 0 is a monitor core.
grep -qE '^smp: 4 harts in the tree, booted on [0-3]$' "$log" ||
    fail "the harts were not enumerated from the device tree"
[ "$(grep -cE '^smp:   hart [0-3] (is this one|can be started)$' "$log")" = "4" ] ||
    fail "not every hart was classified"

# All three others started, and all four reached kernel code. A hart the
# firmware accepted and that never arrives is the failure this distinguishes.
grep -qF "smp: 3 of 3 started harts reached the kernel, 4 running in total" "$log" ||
    fail "a hart did not reach the kernel"

# The program sees them. This is where an under-reporting affinity mask
# shows up: a kernel can run four harts and tell the program it has one, and
# the program then uses one.
grep -qF "go: GOMAXPROCS=4 NumCPU=4" "$log" ||
    fail "the program was not told there are four CPUs"

# **Every hart ran threads.** The structural proof that work was spread: a
# zero here is a hart that stayed in its idle loop for the whole run.
usage=$(sed -n 's/^smp: thread switches by hart: //p' "$log" | head -1)
[ -n "$usage" ] || fail "no per-hart usage was reported"
for pair in $usage; do
    hart="${pair%%=*}"
    switches="${pair##*=}"
    [ "$switches" -gt 0 ] || fail "hart $hart never ran a thread (it only idled)"
done
[ "$(printf '%s\n' $usage | wc -l)" = "4" ] || fail "usage was not reported for four harts"

# The parallel measurement is printed, with its ratio, and the work came out
# right — correctness under four harts is the thing that would break if the
# scheduler handed one thread to two harts.
grep -qE '^go: 8 units serially [0-9]+ms, in parallel [0-9]+ms, speedup [0-9]+\.[0-9]x$' "$log" ||
    fail "the parallel measurement is missing"
grep -qF "go: contended counter reached 16000" "$log" ||
    fail "the contended counter is wrong on four harts, so an update or a futex wake was lost"
grep -qF "go: sum of primes below 200000 is 1709600813" "$log" ||
    fail "a long computation gave the wrong answer"
grep -qF "go: channel values [1 4 9 16 25 36]" "$log" || fail "channels are wrong on four harts"
grep -qE '^init: /init exited with 0 after [0-9]+ syscalls$' "$log" ||
    fail "it did not exit cleanly"
echo "ok: four harts from the device tree, all running Go threads — $usage"
echo "    $(sed -n 's/^go: \(8 units.*\)$/\1/p' "$log" | head -1)"
echo PASS
