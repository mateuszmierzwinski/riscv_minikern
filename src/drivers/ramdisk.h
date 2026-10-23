/* ramdisk — a block device over memory somebody else put there.
 *
 * **The fixture that makes filesystem work testable on real hardware.**
 * This kernel reads XFS, and until now it could only read the *one*
 * filesystem on the operator's SD card — so every gap in the reader (a
 * b-tree extent fork, a directory too big for one block, an unusual
 * `-n size=`) could be described but not exercised, because producing a
 * filesystem with those shapes means writing a card, and that is the
 * operator's to do.
 *
 * The way round it is the one `minikern.loadat` already uses for programs:
 * U-Boot has a network, so it can fetch an image into DRAM, and a block
 * device over a buffer is twenty lines. `mkfs.xfs` on the host then builds
 * whatever layout a test needs — deliberately fragmented files, thousands
 * of directory entries, a different block size — and the board mounts it in
 * one boot with the card untouched.
 *
 * It presents **512-byte sectors**, like the controllers do, so everything
 * above it (the partition scanner, the 4 KiB `blockscale` bridge, the block
 * cache) sees exactly what it sees on a real disk. An image with no
 * partition table is used whole, which is what `mkfs.xfs` on a plain file
 * produces.
 */
#ifndef MINIKERN_DRIVERS_RAMDISK_H
#define MINIKERN_DRIVERS_RAMDISK_H

#include <stdint.h>

#include "core/blockdev.h"
#include "core/types.h"

/* Presents `bytes` of memory at `at` as a writable block device.
 *
 * KERR_INVAL when the range is empty or not a whole number of sectors: a
 * filesystem image that has been truncated is better refused here than
 * half read later. The memory is **not** copied — the device reads and
 * writes it in place, so the caller must have kept the allocator off it. */
int ramdisk_init(phys_t at, uint64_t bytes, struct blockdev *out);

#endif
