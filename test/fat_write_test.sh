#!/usr/bin/env bash
# The FAT driver, judged by dosfstools and mtools on images mkfs.vfat made.
#
# For each of FAT12, FAT16 and FAT32: mtools puts files in, `fat_writecheck`
# reads them back and then writes its own, `fsck.fat -n` has to find nothing,
# and mtools has to read what the driver wrote — byte for byte, by name.
set -euo pipefail

check="$1"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
export MTOOLS_SKIP_CHECK=1
export LC_ALL=C.UTF-8 # mtools renders long names in the locale's charset

for tool in mkfs.vfat fsck.fat mcopy mmd mtype mdir cmp; do
    command -v "$tool" >/dev/null || { echo "$tool is not installed" >&2; exit 1; }
done

fail() { echo "FAIL: $1" >&2; exit 1; }

"$check" --payload 100000 > "$tmp/long.bin"
"$check" --payload 5000 > "$tmp/inner.bin"
"$check" --payload 250000 > "$tmp/kernel.bin"
"$check" --payload 70000 > "$tmp/moved.bin"
printf 'hello from mtools\n' > "$tmp/hello.txt"

# volume <label> <size> <mkfs options...>
volume() {
    local label="$1" size="$2"
    shift 2
    local img="$tmp/$label.img"
    rm -f "$img"
    truncate -s "$size" "$img"
    mkfs.vfat -n BOOT "$@" "$img" >/dev/null || fail "mkfs.vfat refused $label"
    mcopy -i "$img" "$tmp/hello.txt" ::HELLO.TXT
    mcopy -i "$img" "$tmp/long.bin" "::long name file.txt"
    mmd -i "$img" ::sub
    mcopy -i "$img" "$tmp/inner.bin" ::sub/inner.bin
    "$check" "$img" read write many || fail "the driver's steps failed on $label"
    if ! fsck.fat -n "$img" > "$tmp/$label.fsck" 2>&1; then
        cat "$tmp/$label.fsck"
        fail "fsck.fat finds $label damaged after the writes"
    fi
    mtype -i "$img" ::kernel.bin > "$tmp/out.bin"
    cmp -s "$tmp/out.bin" "$tmp/kernel.bin" || fail "mtools reads /kernel.bin wrong on $label"
    mtype -i "$img" "::boot dir/moved.txt" > "$tmp/out.bin"
    cmp -s "$tmp/out.bin" "$tmp/moved.bin" || fail "mtools reads the moved file wrong on $label"
    mdir -i "$img" -b ::deeper | grep -q "x.cfg" || fail "mtools cannot find the moved directory's file on $label"
    mdir -i "$img" -b :: | grep -q "zażółć.txt" || fail "mtools cannot find the UTF-8 name on $label"
    rm -f "$img"
    echo "ok: $label — fsck.fat finds nothing, mtools reads what was written"
}

volume fat12 4M -F 12
volume fat16 64M -F 16
volume fat32 300M -F 32
echo PASS
