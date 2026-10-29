/* Making a file shorter, which means giving its blocks back.
 *
 * **Why this is the last of the file syscalls to arrive.** Everything else
 * in that list reads a field or writes one. A truncate has to walk the
 * extent fork, decide which records are wholly past the new end and which
 * one straddles it, hand those blocks to the allocator, rewrite the fork in
 * place, and then correct three counters that the rest of the filesystem
 * trusts — `di_size`, `di_nblocks` and `di_nextents`. Getting any of the
 * three wrong leaves a filesystem that mounts and reads correctly and that
 * `xfs_repair` condemns.
 *
 * **Growing is not symmetric with shrinking, and must not be.** A truncate
 * past the end of a file makes a hole, and a hole in XFS is the *absence*
 * of an extent — so growing allocates nothing and writes one number. A
 * version that allocated to fill the gap would turn `truncate(f, 1 << 30)`
 * into a gigabyte of writes, which is the difference between a sparse file
 * and a full disk.
 *
 * **What it refuses.** A b-tree fork, because rewriting one in place is a
 * different piece of work from rewriting a list; and a realtime file,
 * because its blocks come from the realtime bitmap rather than from an
 * allocation group and `xfs_free_extent` would be freeing them into the
 * wrong map. Both answer `KERR_NOTSUP`, which is this filesystem's habit:
 * a refusal a caller can report beats a structure that has to be repaired.
 */
#include "core/bcache.h"
#include "core/errno.h"
#include "core/xfs/xfs_internal.h"

#include <stddef.h>

/* The blocks an extent occupies that lie at or after `from`.
 *
 * Returned as a count rather than a bool because the straddling extent is
 * the interesting case: it keeps its head and loses its tail, and the number
 * of blocks to free is what distinguishes that from dropping it whole. */
static uint64_t blocks_past(const struct xfs_extent *e, uint64_t from) {
    if (e->startoff >= from) {
        return e->blockcount;
    }
    if (e->startoff + e->blockcount <= from) {
        return 0;
    }
    return e->startoff + e->blockcount - from;
}

/* Hands the last `past` blocks of a file's extent back: out of the
 * reverse map, at the logical block they held, and to their group. */
static int give_back(struct xfs *fs, const struct xfs_inode *ip, const struct xfs_extent *e,
                     uint64_t past) {
    uint64_t keep = e->blockcount - past;
    return xfs_free_data(fs, ip, e->startblock + keep, (uint32_t)past,
                         xfs_rmap_offset_of(e, keep));
}

/* Rewrites the fork with the records that survive, and frees the rest.
 *
 * In one pass, writing survivors to the front of both copies of the fork as
 * it goes: the in-memory one the caller holds and the on-disk one. Two
 * passes would mean the two could disagree if the second failed partway,
 * and an inode whose `nextents` does not match its records is one
 * `xfs_repair` clears.
 */
static int shrink_fork(struct xfs *fs, struct xfs_inode *ip, uint8_t *di, uint64_t last_block,
                       uint64_t *freed) {
    struct xfs_fork_map m;
    int rc = xfs_fork_load(fs, di, &m);
    *freed = 0;
    for (uint32_t i = 0; rc == KERR_OK && i < m.count;) {
        uint64_t past = blocks_past(&m.e[i], last_block);
        if (past > 0) {
            rc = give_back(fs, ip, &m.e[i], past);
            *freed += rc == KERR_OK ? past : 0u;
            m.e[i].blockcount -= rc == KERR_OK ? (uint32_t)past : 0u;
        }
        if (rc == KERR_OK && m.e[i].blockcount == 0) {
            xfs_fork_map_drop(&m, i); /* wholly past the new end: the record goes away */
        } else {
            i++;
        }
    }
    /* Stored even after a failure part way, so the map says what was freed. */
    if (*freed > 0) {
        int stored = xfs_fork_store(fs, ip->ino, di, &m);
        rc = rc != KERR_OK ? rc : stored;
    }
    ip->nextents = m.count;
    return rc;
}

/* Writes just the size, for the two cases that free nothing: growing, and a
 * file small enough to live inside its inode.
 *
 * Separate from the shrink because it touches one field and must not look
 * like it might touch more — and because the write path has its own private
 * version of this, which is a duplication worth noting rather than
 * compounding: two functions that write `di_size` are one more than there
 * should be, and merging them belongs with the next change to either. */
static int set_size_only(struct xfs *fs, struct xfs_inode *ip, uint64_t size, uint64_t now_s) {
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    uint8_t *di;
    int rc;
    xfs_inode_place(fs, ip->ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    xfs_put_be64(di + XFS_DI_SIZE, size);
    xfs_put_be32(di + XFS_DI_MTIME, (uint32_t)now_s);
    xfs_put_be32(di + XFS_DI_CTIME, (uint32_t)now_s);
    xfs_reseal(fs, di, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    ip->size = size;
    return KERR_OK;
}

/* The refusals that hold whatever the new size: a filesystem that cannot be
 * written, something that is not a regular file, and a b-tree fork. */
static int refuse_truncate(const struct xfs *fs, const struct xfs_inode *ip) {
    if (!xfs_writable(fs)) {
        return KERR_ROFS;
    }
    if (!xfs_is_reg(ip)) {
        return KERR_INVAL; /* a directory is shortened by removing names */
    }
    return KERR_OK;
}

/* Frees what lies past the new end and records the size, the block count
 * and the extent count together. */
static int shrink_file(struct xfs *fs, struct xfs_inode *ip, uint64_t size, uint64_t now_s) {
    uint64_t block, freed = 0;
    uint32_t boffset;
    struct bbuf *b;
    uint8_t *di;
    int rc;
    xfs_inode_place(fs, ip->ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    /* **Rounded up**: a file of 100 bytes with a 4 KiB block keeps its only
     * block, and truncating to 1 byte must not free the block holding it.
     * Rounding down would free the block the surviving data is in, which
     * reads back as zeroes — a silent loss of the data the caller kept. */
    rc = shrink_fork(fs, ip, di, (size + fs->blocksize - 1u) >> fs->blocklog, &freed);
    if (rc == KERR_OK) {
        xfs_put_be64(di + XFS_DI_SIZE, size);
        xfs_put_be64(di + XFS_DI_NBLOCKS, xfs_be64(di + XFS_DI_NBLOCKS) - freed);
        xfs_dinode_set_data_extents(fs, di, ip->nextents);
        xfs_put_be32(di + XFS_DI_MTIME, (uint32_t)now_s);
        xfs_put_be32(di + XFS_DI_CTIME, (uint32_t)now_s);
        xfs_reseal(fs, di, fs->inodesize, XFS_DI_CRC);
        bcache_dirty(b);
        ip->size = size;
    }
    bcache_put(fs->cache, b);
    /* The caller's copy follows the disk: the fork may have changed shape. */
    return rc == KERR_OK ? xfs_inode_read(fs, ip->ino, ip) : rc;
}

int xfs_truncate(struct xfs *fs, struct xfs_inode *ip, uint64_t size, uint64_t now_s) {
    int rc = ip == NULL ? KERR_INVAL : refuse_truncate(fs, ip);
    if (rc != KERR_OK) {
        return rc;
    }
    /* A file small enough to live inside its inode owns no blocks, so there
     * is nothing to free and the size is the whole change; and **growing
     * makes a hole, not an allocation**. */
    if (ip->format == XFS_DINODE_FMT_LOCAL || size >= ip->size) {
        return set_size_only(fs, ip, size, now_s);
    }
    if (ip->realtime) {
        /* A realtime block would be freed into a group it never belonged to
         * — which this did, until the header's promise was checked. A
         * shared block is not freed from under the other file: its count
         * drops instead (xfs_free_data). */
        return KERR_NOTSUP;
    }
    return shrink_file(fs, ip, size, now_s);
}
