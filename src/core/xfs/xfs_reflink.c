/* Files that share blocks: freeing one's share, and making one share
 * another's.
 *
 * A file Linux copied with `cp --reflink` (the default for a copy within
 * one XFS) maps the same blocks as its original, both carry
 * `XFS_DIFLAG2_REFLINK`, the reverse map holds a record for each, and the
 * reference-count tree says how many share each block. Freeing such a
 * file's extent removes *its* reverse-map record and lowers the counts;
 * only blocks nobody else holds go back to free space. Writing one is the
 * copy-on-write of xfs_fill.c.
 *
 * `xfs_reflink_clone` is the other direction, `FICLONE`'s: an empty file
 * made to share every block of another. The kernel has no caller for it
 * yet; the host checker uses it to build what a reflink copy is, so that
 * `xfs_repair` can judge the structure and then everything done to it. */
#include <stddef.h>

#include "core/bcache.h"
#include "core/errno.h"
#include "core/xfs/xfs_internal.h"

/* A block whose count reached zero: nobody holds it now, so it is free. */
static int free_last(void *self, uint64_t fsb, uint32_t count) {
    struct xfs *fs = self;
    uint64_t mask = ((uint64_t)1 << fs->agblklog) - 1u;
    return xfs_free_extent(fs, xfs_fsb_ag(fs, fsb), (uint32_t)(fsb & mask), count);
}

int xfs_free_data(struct xfs *fs, const struct xfs_inode *ip, uint64_t fsb, uint32_t count,
                  uint64_t offset) {
    int rc;
    if (!fs->has_reflink || !ip->shares_blocks) {
        return xfs_free_owned(fs, fsb, count, ip->ino, offset);
    }
    rc = xfs_rmap_unmap(fs, fsb, count, ip->ino, offset);
    return rc != KERR_OK ? rc : xfs_refcount_adjust(fs, fsb, count, -1, free_last, fs);
}

/* The extents being cloned, held while the destination's map is loaded:
 * both maps cannot be in xfs_fork.c's one buffer at once. */
static struct xfs_extent clone_buf[XFS_FORK_MAP_MAX];
static uint32_t clone_count;
static uint64_t clone_size, clone_blocks;

/* Copies the source's map out, with its size and data block count. */
static int take_source(struct xfs *fs, uint64_t ino) {
    struct xfs_fork_map m;
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    int rc;
    xfs_inode_place(fs, ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_fork_load(fs, b->data + boffset, &m);
    clone_count = m.count;
    clone_blocks = 0;
    for (uint32_t i = 0; rc == KERR_OK && i < m.count; i++) {
        clone_buf[i] = m.e[i];
        clone_blocks += m.e[i].blockcount;
    }
    clone_size = xfs_be64(b->data + boffset + XFS_DI_SIZE);
    bcache_put(fs->cache, b);
    return rc;
}

/* Marks an inode as one whose blocks may be shared. */
static int mark_reflinked(struct xfs *fs, uint64_t ino) {
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    uint8_t *di;
    int rc;
    xfs_inode_place(fs, ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    xfs_put_be64(di + XFS_DI_FLAGS2, xfs_be64(di + XFS_DI_FLAGS2) | XFS_DIFLAG2_REFLINK);
    xfs_reseal(fs, di, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* Gives the destination the source's map, size and blocks. */
static int give_destination(struct xfs *fs, uint64_t ino) {
    struct xfs_fork_map m;
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    uint8_t *di;
    int rc;
    xfs_inode_place(fs, ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    rc = xfs_fork_load(fs, di, &m);
    rc = rc == KERR_OK && m.count != 0 ? KERR_EXIST : rc; /* only into an empty file */
    for (uint32_t i = 0; rc == KERR_OK && i < clone_count; i++) {
        m.e[i] = clone_buf[i];
    }
    m.count = rc == KERR_OK ? clone_count : m.count;
    if (rc == KERR_OK) {
        xfs_put_be64(di + XFS_DI_SIZE, clone_size);
        xfs_put_be64(di + XFS_DI_NBLOCKS, xfs_be64(di + XFS_DI_NBLOCKS) + clone_blocks);
        rc = xfs_fork_store(fs, ino, di, &m);
        xfs_reseal(fs, di, fs->inodesize, XFS_DI_CRC);
        bcache_dirty(b);
    }
    bcache_put(fs->cache, b);
    return rc;
}

/* The counts and the reverse map for the destination's new share. */
static int share_extents(struct xfs *fs, uint64_t ino) {
    int rc = KERR_OK;
    for (uint32_t i = 0; rc == KERR_OK && i < clone_count; i++) {
        const struct xfs_extent *e = &clone_buf[i];
        rc = xfs_rmap_map_shared(fs, e->startblock, e->blockcount, ino, xfs_rmap_offset_of(e, 0));
        rc = rc != KERR_OK ? rc
                           : xfs_refcount_adjust(fs, e->startblock, e->blockcount, 1, NULL, NULL);
    }
    return rc;
}

int xfs_reflink_clone(struct xfs *fs, const struct xfs_inode *src, const struct xfs_inode *dst) {
    int rc = fs->has_reflink && fs->has_rmapbt ? KERR_OK : KERR_NOTSUP;
    if (rc == KERR_OK && (src->realtime || dst->realtime || !xfs_is_reg(src) || !xfs_is_reg(dst))) {
        rc = KERR_INVAL;
    }
    rc = rc != KERR_OK ? rc : take_source(fs, src->ino);
    rc = rc != KERR_OK ? rc : give_destination(fs, dst->ino);
    rc = rc != KERR_OK ? rc : share_extents(fs, dst->ino);
    rc = rc != KERR_OK ? rc : mark_reflinked(fs, src->ino);
    return rc != KERR_OK ? rc : mark_reflinked(fs, dst->ino);
}
