/* partition — the filesystem is not always at the start of the disk.
 *
 * A card formatted whole has its superblock in sector 0, and every disk this
 * project had seen was formatted that way. A drive partitioned the ordinary
 * way does not: its GPT puts the first partition at sector 2048, and a
 * filesystem reader pointed at sector 0 finds a partition table where it
 * expects a superblock and reports that there is no filesystem.
 *
 * So this is an adapter in the proper sense: it wraps a block device and
 * presents one partition of it as a block device, with every address
 * shifted and every access past the end refused. The layers above cannot
 * tell the difference, which is the point — XFS does not learn what a
 * partition is.
 *
 * Both table formats, because a disk carries whichever it was given:
 *
 *   - **GPT**, which is the modern one: a header in LBA 1 identified by
 *     "EFI PART", and an array of 128-byte entries.
 *   - **MBR**, four 16-byte entries at offset 446 of sector 0. A GPT disk
 *     also carries a protective MBR whose single entry has type 0xEE, and
 *     taking that at face value would present the whole disk as a partition
 *     and hide the real table — so it is recognised and rejected.
 *
 * A device with no table at all is not an error: it is a disk whose
 * filesystem starts at sector 0, which is how every card this project has
 * read so far was made.
 */
#ifndef MINIKERN_CORE_PARTITION_H
#define MINIKERN_CORE_PARTITION_H

#include "core/blockdev.h"

/* One partition found on a device. */
struct partition {
    uint64_t first;   /* its first sector on the underlying device */
    uint64_t sectors; /* how many it spans */
    bool present;
};

/* The state the adapter keeps. Opaque to callers but declared here so it can
 * live in static storage: a kernel's disk layer has no allocator at the
 * point it attaches a disk. */
struct partdev {
    struct blockdev *under;
    uint64_t first;
    uint64_t sectors;
};

/* How many partitions a table is read for. Four is the MBR's limit and more
 * than any disk this kernel mounts from needs; a GPT may declare 128, and
 * the ones past this are listed but not offered. */
#define PARTITION_MAX 4u

/* Reads `dev`'s partition table into `out`, newest format first.
 *
 * Returns the number of partitions found, which is 0 for a device that has
 * no table — not an error, and the caller then uses the device whole. A
 * table that is present but malformed also reports 0 rather than a
 * half-trusted entry, because a wrong offset reads a valid-looking
 * filesystem somewhere else on the disk. */
uint32_t partition_scan(struct blockdev *dev, struct partition *out, uint32_t cap);

/* `partition_scan`, telling a table that could not be **read** from one that
 * is not there: `KERR_IO` when a read failed and nothing was found, so the
 * caller can try again rather than use a partitioned disk whole. */
int partition_scan_checked(struct blockdev *dev, struct partition *out, uint32_t cap,
                           uint32_t *found);

/* Presents `p` of `dev` as a block device of its own. The partition must lie
 * inside the device; `KERR_INVAL` when it does not, which is what a corrupt
 * table looks like. */
int partition_wrap(struct blockdev *dev, const struct partition *p, struct partdev *state,
                   struct blockdev *out);

#endif
