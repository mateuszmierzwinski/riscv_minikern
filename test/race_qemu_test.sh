#!/usr/bin/env bash
# The four-hart race, hammered.
#
# `go_qemu_test` notices the corruption about once in thirty boots, which is
# enough to know it exists and far too rare to **compare** one configuration
# against another: at that rate every question — a mechanism on, the same
# mechanism off — needs hundreds of boots before its answer means more than
# luck. This runs the reproducer instead: the operations that have been seen
# to fail, in a loop, every result checked, stopping at the first one that is
# wrong.
#
# It is **not** a gate on the ordinary suite, which is why it is tagged
# `manual` and why its name says `race` rather than `test` of anything: the
# thing it looks for is known to be there. It is the instrument for the hunt.
# Exit 1 means it reproduced the corruption and the round that did it is
# printed with whatever the kernel's own detectors had to say; exit 0 means
# this configuration got through its rounds. `MINIKERN_EXTRA` passes kernel
# options, which is how a mechanism is turned off and the rate measured
# again,.
set -uo pipefail

image="${1:?the kernel image}"
program="${2:?the Go reproducer}"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
log="$tmp/console.log"

for tool in qemu-system-riscv64 mkfs.xfs; do
    command -v "$tool" >/dev/null || { echo "$tool is not installed" >&2; exit 2; }
done

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
    { echo "mkfs.xfs refused to make the image" >&2; exit 2; }

timeout 600 qemu-system-riscv64 -M virt -smp 4 -accel tcg,thread=multi -m 2G -nographic \
    -bios default -kernel "$image" \
    -drive "file=$tmp/fs.img,if=none,format=raw,id=nvm0" -device nvme,serial=mkrace,drive=nvm0 \
    -append "console=sbi minikern.halt=poweroff minikern.init=1 ${MINIKERN_EXTRA:-}" \
    > "$log" 2>&1

if grep -aq "stress: FAILED" "$log"; then
    grep -a "stress: FAILED" "$log"
    grep -aE "trap: \*\*|sched: \*\*|uspace: \*\*|signal:|stale-translation" "$log" | tail -6
    exit 1
fi
if ! grep -aq "nothing went wrong" "$log"; then
    echo "the reproducer did not finish:"
    tail -6 "$log"
    exit 2
fi
grep -a "rounds, nothing went wrong" "$log"
exit 0
