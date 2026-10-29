/* Making a block of a file writable where it is: a hole filled, an
 * unwritten extent's block marked written, a shared block copied.
 *
 * **Three refusals this replaces.** A write that landed in a hole — a
 * file seeked past and written beyond, or one `truncate` had made longer —
 * was refused; so was one into an extent Linux had preallocated
 * (`fallocate`, which leaves it *unwritten*: allocated, and reading as
 * zeroes); and so was any write to a file that shares blocks with another
 * (a reflink copy), because writing a shared block in place would change
 * the other file too.
 *
 * Each is one block's mapping replaced in the file's extent map
 * (xfs_fork.c), with the reverse map and — for a shared block — the
 * reference counts following it:
 *   - a hole gets a fresh block, zeroed, since FAT-style "reads as zero"
 *     is what a hole promised;
 *   - an unwritten block keeps its place but is zeroed and loses the flag:
 *     its bytes were never the file's, and the part the write does not
 *     cover must still read as zeroes;
 *   - a shared block is **copied** to a fresh one, which the file then maps
 *     instead; the other file keeps the original, and its count drops.
 * Realtime files are not handled here (their blocks are not a group's). */
#include <stddef.h>

#include "core/bcache.h"
#include "core/errno.h"
#include "core/xfs/xfs_internal.h"

int xfs_zero_block(struct xfs *fs, uint64_t fsb) {
    struct bbuf *b;
    int rc = bcache_get_zeroed(fs->cache, xfs_fsb_to_block(fs, fsb), &b);
    if (rc == KERR_OK) {
        bcache_put(fs->cache, b);
    }
    return rc;
}

/* Copies block `from` over block `to`, or zeroes `to` when `from` is 0. */
static int fill_block(struct xfs *fs, uint64_t to, uint64_t from) {
    struct bbuf *src, *dst;
    int rc;
    if (from == 0) {
        return xfs_zero_block(fs, to);
    }
    rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, from), &src);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = bcache_get_zeroed(fs->cache, xfs_fsb_to_block(fs, to), &dst);
    if (rc == KERR_OK) {
        for (uint32_t i = 0; i < fs->blocksize; i++) {
            dst->data[i] = src->data[i];
        }
        bcache_put(fs->cache, dst);
    }
    bcache_put(fs->cache, src);
    return rc;
}

/* Maps logical block `logical` of the file to `fsb`, written, in place of
 * whatever it was mapped to (`had` says whether it was mapped at all). */
static int remap(struct xfs *fs, struct xfs_inode *ip, uint64_t logical, uint64_t fsb, bool had) {
    struct xfs_extent e = {.startoff = logical, .startblock = fsb, .blockcount = 1};
    struct xfs_fork_map m;
    uint64_t block, old = 0;
    uint32_t boffset;
    struct bbuf *b;
    int rc;
    xfs_inode_place(fs, ip->ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_fork_load(fs, b->data + boffset, &m);
    rc = rc != KERR_OK || !had ? rc : xfs_fork_map_cut(&m, logical, 1u, &old);
    rc = rc != KERR_OK ? rc : xfs_fork_map_add(&m, &e);
    if (rc == KERR_OK) {
        uint8_t *di = b->data + boffset;
        xfs_put_be64(di + XFS_DI_NBLOCKS, xfs_be64(di + XFS_DI_NBLOCKS) + (had ? 0u : 1u));
        rc = xfs_fork_store(fs, ip->ino, di, &m);
        xfs_reseal(fs, di, fs->inodesize, XFS_DI_CRC);
        bcache_dirty(b);
    }
    bcache_put(fs->cache, b);
    return rc != KERR_OK ? rc : xfs_inode_read(fs, ip->ino, ip);
}

/* A fresh block for logical block `logical`, near the file's inode. */
static int fresh_block(struct xfs *fs, const struct xfs_inode *ip, uint64_t *fsb) {
    uint32_t agno = xfs_fsb_ag(fs, ip->ino >> fs->inopblog), got = 0;
    int rc = xfs_quota_charge_all(fs, ip, 1, 0);
    rc = rc != KERR_OK ? rc : xfs_alloc_somewhere(fs, &agno, 1u, 0, &got);
    *fsb = ((uint64_t)agno << fs->agblklog) | got;
    return rc;
}

static int fill_hole(struct xfs *fs, struct xfs_inode *ip, uint64_t logical) {
    uint64_t fsb = 0;
    int rc = fresh_block(fs, ip, &fsb);
    rc = rc != KERR_OK ? rc : fill_block(fs, fsb, 0);
    rc = rc != KERR_OK ? rc : xfs_rmap_map(fs, fsb, 1u, ip->ino, logical);
    return rc != KERR_OK ? rc : remap(fs, ip, logical, fsb, false);
}

static int write_unwritten(struct xfs *fs, struct xfs_inode *ip, uint64_t logical,
                           uint64_t fsb) {
    int rc = fill_block(fs, fsb, 0);
    rc = rc != KERR_OK ? rc
                       : xfs_rmap_unmap(fs, fsb, 1u, ip->ino, logical | XFS_RMAP_OFF_UNWRITTEN);
    rc = rc != KERR_OK ? rc : xfs_rmap_map(fs, fsb, 1u, ip->ino, logical);
    return rc != KERR_OK ? rc : remap(fs, ip, logical, fsb, true);
}

/* What the other file's count does when this one stops sharing a block:
 * it drops, and a block nobody shares any longer stays the other file's.
 * Freeing is never the result — the block was shared — so reaching this
 * would mean the counts were wrong. */
static int never_last(void *self, uint64_t fsb, uint32_t count) {
    (void)self;
    (void)fsb;
    (void)count;
    return KERR_IO;
}

static int copy_shared(struct xfs *fs, struct xfs_inode *ip, uint64_t logical, uint64_t old) {
    uint64_t fsb = 0;
    int rc = fresh_block(fs, ip, &fsb);
    rc = rc != KERR_OK ? rc : fill_block(fs, fsb, old);
    rc = rc != KERR_OK ? rc : xfs_rmap_unmap(fs, old, 1u, ip->ino, logical);
    rc = rc != KERR_OK ? rc : xfs_rmap_map(fs, fsb, 1u, ip->ino, logical);
    rc = rc != KERR_OK ? rc : xfs_refcount_adjust(fs, old, 1u, -1, never_last, NULL);
    if (rc == KERR_OK) {
        (void)xfs_quota_charge_all(fs, ip, -1, 0); /* one block in, one out */
    }
    return rc != KERR_OK ? rc : remap(fs, ip, logical, fsb, true);
}

int xfs_file_prepare_block(struct xfs *fs, struct xfs_inode *ip, uint64_t logical) {
    struct xfs_extent e;
    bool shared = false;
    uint64_t fsb;
    int rc = xfs_bmap(fs, ip, logical, &e);
    if (rc != KERR_OK && rc != KERR_NOENT) {
        return rc;
    }
    if (ip->realtime && (rc == KERR_NOENT || e.unwritten)) {
        return KERR_NOTSUP; /* the realtime bitmap's business, not a group's */
    }
    if (rc == KERR_NOENT) {
        return fill_hole(fs, ip, logical);
    }
    fsb = e.startblock + (logical - e.startoff);
    if (e.unwritten) {
        return write_unwritten(fs, ip, logical, fsb);
    }
    rc = ip->shares_blocks && !ip->realtime ? xfs_refcount_shared(fs, fsb, &shared) : KERR_OK;
    return rc != KERR_OK || !shared ? rc : copy_shared(fs, ip, logical, fsb);
}
