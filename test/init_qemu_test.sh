#!/usr/bin/env bash
# A real program: an ELF off a filesystem, loaded and started properly.
#
# This is the whole machine in one test — firmware, S-mode, paging, an NVMe
# controller, a block-size adapter, a buffer cache, XFS, an ELF64 loader, a
# process address space, and the state a C runtime expects — and it is the
# path a Go binary will take unchanged.
#
# The program under test **reads the state the kernel started it in** and
# reports it: argc, argv, the environment, and the auxiliary vector. That
# matters because a loader can place every segment correctly and still start
# a program wrongly; on the architecture this migrated from, the first bug
# found in user mode was exactly that, and it was invisible until something
# actually read `argv`.
#
# So the assertions are on the program's own account of what it found, not on
# the kernel's account of what it did.
set -euo pipefail

image="$1"
init="$2"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
log="$tmp/console.log"

for tool in qemu-system-riscv64 mkfs.xfs xfs_repair; do
    command -v "$tool" >/dev/null || { echo "$tool is not installed" >&2; exit 1; }
done

fail() { echo "FAIL: $1" >&2; exit 1; }

# The feature set this kernel's XFS reads (see xfs_qemu_test for why each
# one is off).
install -m 755 "$init" "$tmp/init"
printf 'a file beside the program\n' > "$tmp/hello.txt"
cat > "$tmp/proto" <<PROTO
dummy
0 0
d--755 0 0
init ---755 0 0 $tmp/init
hello.txt ---644 0 0 $tmp/hello.txt
\$
PROTO

dd if=/dev/zero of="$tmp/fs.img" bs=1M count=300 status=none
mkfs.xfs -q -f -m crc=1,finobt=1,reflink=0,rmapbt=0,bigtime=0,inobtcount=0 \
    -i sparse=0,size=512,nrext64=0 -n ftype=1 -b size=4096 -p "$tmp/proto" "$tmp/fs.img" ||
    fail "mkfs.xfs refused to make the image"
xfs_repair -n "$tmp/fs.img" >/dev/null 2>&1 || fail "xfs_repair rejects the image"

status=0
timeout 120 qemu-system-riscv64 -M virt -smp 4 -m 1G -nographic -bios default -kernel "$image" \
    -drive "file=$tmp/fs.img,if=none,format=raw,id=nvm0" -device nvme,serial=mkinit,drive=nvm0 \
    -append "console=sbi minikern.halt=poweroff minikern.init=1" > "$log" 2>&1 || status=$?
tr -d '\r' < "$log" > "$log.clean" && mv "$log.clean" "$log"
cat "$log"

[ "$status" -eq 0 ] || fail "qemu exited $status"
! grep -qF PANIC "$log" || fail "the kernel panicked"

# It was read off the filesystem, not embedded.
grep -qE '^init: /init is [0-9]+ bytes$' "$log" || fail "the program was not read from the disk"

# ── what the program itself found ──────────────────────────────────────────

# argv, which is the thing that was wrong on the other architecture.
grep -qF "init: argc=1 argv[0]=/init" "$log" || fail "argc or argv is wrong"

# The environment, in order.
grep -qF "init: envc=2 HOME=/ PATH=/" "$log" || fail "the environment is wrong"

# The auxiliary vector. AT_PHDR must be a **real address**, not zero: a Go
# runtime reads it to find its own segments, and zero means the program
# headers are in the file but in no segment — which is what happens when a
# linker script forgets to leave room for them.
grep -qE '^init: pagesz=4096 phnum=[0-9]+ phent=56 phdr=0x0000000000010040 entry=0x0000000000010190$' "$log" ||
    fail "the auxiliary vector is wrong (AT_PHDR of 0 means the headers are in no segment)"
grep -qF "init: execfn=/init" "$log" || fail "AT_EXECFN is wrong"

# AT_RANDOM must be sixteen bytes that are actually random-looking. All zero
# would be a kernel that supplied the tag without supplying the bytes, and a
# runtime seeds its hash tables from them.
grep -qF "init: random=16 of 16 bytes are non-zero" "$log" ||
    fail "AT_RANDOM is absent, or its bytes are not all non-zero"

# The ABI requires a 16-byte-aligned stack pointer at entry, and a runtime
# that spills a 128-bit value relative to it will fault if it is not.
grep -qE '^init: sp=0x[0-9a-f]+ \(16-byte aligned\)$' "$log" ||
    fail "the stack pointer is not 16-byte aligned at entry"

# And it ran to completion: its own last line, then the status and the
# syscall count the kernel counted.
grep -qF "init: done" "$log" || fail "the program did not reach its end"
grep -qE '^init: /init exited with 42 after [0-9]+ syscalls$' "$log" ||
    fail "the exit status or the syscall count is wrong"
echo "ok: an ELF off an XFS filesystem on NVMe, loaded, started correctly, and run to completion"
echo PASS
