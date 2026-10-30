/* A directory giving back a block it no longer uses.
 *
 * **Not an optimisation.** A data block of a leaf or node directory that
 * holds no entry at all is something `xfs_repair` junks — the format has no
 * such thing as an empty data block, only a hole where one was — and an
 * emptied hash leaf left in a node directory keeps a key for hashes it no
 * longer has. Removing names one at a time reaches both, so the writer has
 * to do what Linux does: take the block out of the fork, give it back, and
 * leave a hole the next growth can fill.
 *
 * The directory's size follows its highest remaining data block, as Linux
 * keeps it: a hole in the middle leaves it, a block freed from the top
 * shrinks it. */
#include "core/bcache.h"
#include "core/errno.h"
#include "core/xfs/xfs_internal.h"

bool xfs_dir_block_is_empty(const struct xfs *fs, const uint8_t *block) {
    return xfs_dir_largest_free(fs, block) >= fs->dirblocksize - fs->dir_data_hdr;
}

/* Where the directory's data ends: past its highest data block. */
static uint64_t data_end(const struct xfs *fs, const struct xfs_fork_map *m) {
    uint64_t end = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        uint64_t last = m->e[i].startoff + m->e[i].blockcount;
        end = m->e[i].startoff < fs->dir_leaf_block && last > end ? last : end;
    }
    return end << fs->blocklog;
}

int xfs_dir_fork_remove(struct xfs *fs, const struct xfs_inode *dir, uint64_t logical,
                        uint64_t now_s) {
    struct xfs_fork_map m;
    uint64_t block, fsb = 0;
    uint32_t boffset;
    struct bbuf *b;
    uint8_t *di;
    int rc;
    xfs_inode_place(fs, dir->ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    rc = xfs_fork_load(fs, di, &m);
    rc = rc != KERR_OK ? rc : xfs_fork_map_cut(&m, logical, fs->dirblocks, &fsb);
    if (rc == KERR_OK) {
        xfs_put_be64(di + XFS_DI_NBLOCKS, xfs_be64(di + XFS_DI_NBLOCKS) - fs->dirblocks);
        xfs_put_be64(di + XFS_DI_SIZE, data_end(fs, &m));
        rc = xfs_fork_store(fs, dir->ino, di, &m);
        xfs_put_time(di, XFS_DI_MTIME, now_s);
        xfs_put_time(di, XFS_DI_CTIME, now_s);
        xfs_reseal(fs, di, fs->inodesize, XFS_DI_CRC);
        bcache_dirty(b);
    }
    bcache_put(fs->cache, b);
    return rc != KERR_OK ? rc : xfs_free_owned(fs, fsb, fs->dirblocks, dir->ino, logical);
}
