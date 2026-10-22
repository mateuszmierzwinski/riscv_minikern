/* blockscale — a device with larger blocks, over one with smaller sectors.
 *
 * A filesystem's block size and a device's sector size are different facts
 * about different things, and something has to bridge them. XFS asks its
 * cache for **filesystem blocks** — 4 KiB, usually — while an NVMe drive, an
 * SD card and a virtio disk all present 512-byte sectors. Without a bridge,
 * every metadata read returns an eighth of a block: the filesystem mounts,
 * because the superblock's first 512 bytes are enough to describe it, and
 * then the first directory read fails with an I/O error and nothing else
 * visibly wrong. That is exactly how this module came to exist.
 *
 * The ARM kernel this migrated from never needed it: its only block device
 * was a network one whose block size the caller chose, so it simply asked
 * for 4 KiB. A real controller does not offer the choice.
 *
 * Pure, and host-tested against a fake device, because it is address
 * arithmetic — the kind that is either exactly right or quietly reads the
 * wrong part of a disk.
 */
#ifndef MINIKERN_CORE_BLOCKSCALE_H
#define MINIKERN_CORE_BLOCKSCALE_H

#include "core/blockdev.h"

/* The state a scaled device carries: what it sits on, and the ratio. */
struct blockscale {
    const struct blockdev *under;
    uint32_t factor;  /* underlying sectors per logical block */
};

/* Presents `under` with blocks of `logical_block` bytes.
 *
 * KERR_INVAL unless the logical block is a multiple of the device's sector
 * size and a power of two — a ratio that is not a whole number cannot be a
 * block device, and rounding one into place is how a filesystem comes to
 * write over the sector after the one it meant. */
int blockscale_wrap(const struct blockdev *under, uint32_t logical_block, struct blockscale *state,
                    struct blockdev *out);

#endif
