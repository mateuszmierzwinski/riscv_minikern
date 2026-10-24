#include "core/bcache.h"
#include "core/errno.h"
#include "core/xfs/xfs_internal.h"
#include "lib/crc32c.h"

#include <stddef.h>

/* ── removing a file ──────────────────────────────────────────────────────
 *
 * The reverse of creating one, and the same four structures: the entry
 * leaves the parent's directory, the inode's bit goes back in both inode
 * btrees, the group header and superblock count one more free, and the
 * inode itself is cleared so that nothing mistakes it for a live one.
 *
 * What it refuses, and why:
 *
 *   - a file that **owns blocks**, until the free-space side exists: the
 *     blocks would be lost, which `xfs_repair` would rightly call a leak;
 *   - a chunk with no record in the free-inode tree, which would need that
 *     record inserted — a btree operation this does not implement;
 *   - a directory, a parent that has outgrown its inode, a name that is
 *     not there.
 *
 * So it removes the files this kernel can make, which is what the tests
 * create, and says which of those it hit when it cannot.
 */

#define XFS_DIR_SF_COUNT_OFF   0u
#define XFS_DIR_SF_I8COUNT_OFF 1u
#define XFS_DIR_SF_HEADER      6u

static uint32_t sf_entry_bytes(uint32_t namelen) {
    return 1u + 2u + namelen + 1u + 4u;
}

/* The entry with this name, or NULL, and its inode number. */
static uint8_t *sf_find(uint8_t *sf, uint32_t count, const char *name, uint32_t namelen, uint64_t *ino) {
    uint8_t *e = sf + XFS_DIR_SF_HEADER;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t len = e[0];
        bool same = len == namelen;
        for (uint32_t c = 0; same && c < len; c++) {
            same = e[3 + c] == (uint8_t)name[c];
        }
        if (same) {
            *ino = xfs_be32(e + 4u + len);
            return e;
        }
        e += sf_entry_bytes(len);
    }
    return NULL;
}

/* The end of the entries, which is the directory's size. */
static uint32_t sf_used(const uint8_t *sf, uint32_t count) {
    const uint8_t *e = sf + XFS_DIR_SF_HEADER;
    for (uint32_t i = 0; i < count; i++) {
        e += sf_entry_bytes(e[0]);
    }
    return (uint32_t)(e - sf);
}

/* Takes one entry out of a shortform directory: the bytes after it move
 * down over it, which is all a shortform directory is — a list with no
 * gaps. The entries that remain keep their offsets, which stay in order
 * because removing one cannot reorder the others. */
int xfs_dir_sf_remove(struct xfs *fs, const struct xfs_inode *dir, const char *name, uint32_t namelen,
                     uint64_t now_s, uint64_t *ino) {
    uint64_t block;
    uint32_t boffset, count, used, gap;
    struct bbuf *b;
    uint8_t *di, *sf, *e;
    int rc;
    xfs_inode_place(fs, dir->ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    sf = di + XFS_DI_V3_SIZE;
    count = sf[XFS_DIR_SF_COUNT_OFF];
    used = sf_used(sf, count);
    e = sf[XFS_DIR_SF_I8COUNT_OFF] != 0 ? NULL : sf_find(sf, count, name, namelen, ino);
    if (e == NULL) {
        bcache_put(fs->cache, b);
        return sf[XFS_DIR_SF_I8COUNT_OFF] != 0 ? KERR_NOTSUP : KERR_NOENT;
    }
    gap = sf_entry_bytes(e[0]);
    for (uint8_t *at = e; at + gap < sf + used; at++) {
        at[0] = at[gap];
    }
    sf[XFS_DIR_SF_COUNT_OFF] = (uint8_t)(count - 1u);
    xfs_put_be64(di + XFS_DI_SIZE, used - gap);
    xfs_put_be32(di + XFS_DI_MTIME, (uint32_t)now_s);
    xfs_put_be32(di + XFS_DI_CTIME, (uint32_t)now_s);
    xfs_reseal(di, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* Puts one inode back in a record: its bit set, its count up. */
static void give_to_record(uint8_t *rec, uint32_t bit) {
    uint64_t free = xfs_be64(rec + XFS_INOBT_REC_FREE) | ((uint64_t)1 << bit);
    xfs_put_be32(rec + XFS_INOBT_REC_FREECOUNT, xfs_be32(rec + XFS_INOBT_REC_FREECOUNT) + 1u);
    xfs_put_be64(rec + XFS_INOBT_REC_FREE, free);
}

/* Returns an inode to both trees and to the counters.
 *
 * The free-inode tree must already describe this chunk. It will, for any
 * inode this kernel allocated — it never takes a chunk's last free one, so
 * the record never goes away — and refusing otherwise is better than
 * inserting a btree record badly. */
static int release_inode(struct xfs *fs, uint32_t agno, uint64_t ino) {
    struct bbuf *agib, *inob, *finob;
    uint8_t *agi, *rec, *frec;
    /* The records are keyed by the **group-relative** inode number, not the
     * whole one: the group is in the high bits of an inode number and must
     * come off before the chunk is looked for, or no record matches and a
     * remove fails with EIO for no reason the caller can act on. */
    uint64_t agino = ino & (((uint64_t)1 << (fs->agblklog + fs->inopblog)) - 1u);
    uint32_t bit;
    int rc = xfs_agi_get(fs, agno, &agib, &agi);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_tree_get(fs, agno, xfs_be32(agi + XFS_AGI_ROOT), XFS_INOBT_MAGIC, &inob);
    if (rc != KERR_OK) {
        bcache_put(fs->cache, agib);
        return rc;
    }
    rc = xfs_tree_get(fs, agno, xfs_be32(agi + XFS_AGI_FREE_ROOT), XFS_FINOBT_MAGIC, &finob);
    if (rc != KERR_OK) {
        bcache_put(fs->cache, inob);
        bcache_put(fs->cache, agib);
        return rc;
    }
    rec = xfs_tree_record_for(inob, (uint32_t)agino);
    frec = rec == NULL ? NULL : xfs_tree_record_for(finob, (uint32_t)agino);
    if (rec == NULL || frec == NULL) {
        bcache_put(fs->cache, finob);
        bcache_put(fs->cache, inob);
        bcache_put(fs->cache, agib);
        return rec == NULL ? KERR_IO : KERR_NOTSUP;
    }
    bit = (uint32_t)agino - xfs_be32(rec + XFS_INOBT_REC_STARTINO);
    give_to_record(rec, bit);
    give_to_record(frec, bit);
    xfs_put_be32(agi + XFS_AGI_FREECOUNT, xfs_be32(agi + XFS_AGI_FREECOUNT) + 1u);
    xfs_reseal(inob->data, fs->blocksize, XFS_BTREE_CRC);
    xfs_reseal(finob->data, fs->blocksize, XFS_BTREE_CRC);
    xfs_reseal(agi, fs->sectsize, XFS_AGI_CRC);
    bcache_dirty(inob);
    bcache_dirty(finob);
    bcache_dirty(agib);
    bcache_put(fs->cache, finob);
    bcache_put(fs->cache, inob);
    bcache_put(fs->cache, agib);
    return KERR_OK;
}

/* Clears the inode on disk.
 *
 * The mode is what says an inode is in use, so zeroing it is what makes
 * this one free; the generation count goes up so that a stale reference to
 * the old file cannot be mistaken for the next one to use this slot. The
 * rest is zeroed because a free inode holding the remains of a file is how
 * data outlives its deletion. */
static int clear_inode(struct xfs *fs, uint64_t ino) {
    uint64_t block;
    uint32_t boffset, gen;
    struct bbuf *b;
    uint8_t *di;
    int rc;
    xfs_inode_place(fs, ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    gen = xfs_be32(di + XFS_DI_GEN) + 1u;
    for (uint32_t i = 0; i < fs->inodesize; i++) {
        di[i] = 0;
    }
    xfs_put_be16(di + XFS_DI_MAGIC, XFS_DINODE_MAGIC);
    di[XFS_DI_VERSION] = XFS_DI_VERSION_3;
    xfs_put_be32(di + XFS_DI_GEN, gen);
    xfs_put_be32(di + XFS_DI_NEXT_UNLINKED, XFS_NULL_AGINO);
    xfs_put_be64(di + XFS_DI_INO, ino);
    for (uint32_t i = 0; i < 16; i++) {
        di[XFS_DI_UUID + i] = fs->uuid[i];
    }
    xfs_reseal(di, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* Gives a file's blocks back to the group they came from.
 *
 * Every extent, in turn, joined to whatever it abuts (xfs_alloc.c). It
 * happens **before** the directory entry goes, so that a failure half way
 * leaves a file that still exists and still owns what it owns — a leak is
 * worse than a file that would not delete. */
static int free_file_blocks(struct xfs *fs, const struct xfs_inode *ip) {
    uint32_t n = ip->nextents;
    if (ip->nblocks == 0) {
        return KERR_OK;
    }
    for (uint32_t i = 0; i < n; i++) {
        struct xfs_extent e;
        uint64_t mask = ((uint64_t)1 << fs->agblklog) - 1u;
        int rc;
        xfs_extent_unpack(ip->fork + i * XFS_EXTENT_BYTES, &e);
        if (e.blockcount == 0) {
            continue;
        }
        rc = xfs_free_extent(fs, xfs_fsb_ag(fs, e.startblock), (uint32_t)(e.startblock & mask),
                             e.blockcount);
        if (rc != KERR_OK) {
            return rc;
        }
    }
    return KERR_OK;
}

/* Logs the parent, whose directory has changed. The inode that has gone is
 * not logged: there is nothing left of it to describe. */
static int log_unlink(struct xfs *fs, uint64_t dirino) {
    struct xfs_log_inode_item item;
    struct bbuf *b;
    uint64_t block;
    uint32_t boffset;
    int rc;
    xfs_inode_place(fs, dirino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    item.ino = dirino;
    item.chunk_block = block;
    item.boffset = boffset;
    item.core = b->data + boffset;
    rc = xfs_log_inodes(fs, &item, 1);
    bcache_put(fs->cache, b);
    return rc;
}

int xfs_unlink(struct xfs *fs, const struct xfs_inode *dir, const char *name, uint32_t namelen,
               uint64_t now_s) {
    struct xfs_inode ip;
    uint64_t ino = 0;
    int rc;
    if (!xfs_writable(fs)) {
        return KERR_ROFS;
    }
    if (!xfs_is_dir(dir)) {
        return KERR_NOTDIR;
    }
    if (dir->format != XFS_DINODE_FMT_LOCAL && dir->format != XFS_DINODE_FMT_EXTENTS) {
        return KERR_NOTSUP; /* a fork shape the write path cannot edit */
    }
    rc = xfs_dir_lookup(fs, dir, name, namelen, &ino);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_inode_read(fs, ino, &ip);
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfs_is_dir(&ip)) {
        return KERR_ISDIR; /* removing a directory is rmdir, and is not this */
    }
    if (ip.format != XFS_DINODE_FMT_EXTENTS && ip.nblocks != 0) {
        return KERR_NOTSUP; /* a fork shape whose blocks this cannot find */
    }
    /* **A name, not a file.** `unlink` removes one link; the file goes
     * when the last one does. This refused anything with a count above one
     * because nothing could make such a file — hard links did not exist —
     * and now that they do, removing one name from a file with two would
     * have freed the inode under the other name. */
    if (ip.nlink > 1u) {
        rc = xfs_dir_sf_remove(fs, dir, name, namelen, now_s, &ino);
        rc = rc != KERR_OK ? rc : xfs_inode_adjust_nlink(fs, ino, -1);
        return rc != KERR_OK ? rc : log_unlink(fs, dir->ino);
    }
    rc = free_file_blocks(fs, &ip);
    rc = rc != KERR_OK ? rc : xfs_dir_remove(fs, dir, name, namelen, now_s, &ino);
    rc = rc != KERR_OK ? rc : release_inode(fs, xfs_fsb_ag(fs, ino >> fs->inopblog), ino);
    rc = rc != KERR_OK ? rc : xfs_count_inodes_free(fs, 1);
    rc = rc != KERR_OK ? rc : clear_inode(fs, ino);
    return rc != KERR_OK ? rc : log_unlink(fs, dir->ino);
}

/* Removes an entry from a directory of whatever shape.
 *
 * The counterpart of `xfs_dir_add`, and for the same reason: unlink and
 * rename want a name gone and should not have to know whether the
 * directory still lives in its inode. A block directory is **not** shrunk
 * back into its inode when it empties — the space stays allocated and the
 * format stays as it is, which is what Linux does too, and what keeps a
 * directory that is repeatedly filled and emptied from converting twice per
 * entry. */
int xfs_dir_remove(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                   uint32_t namelen, uint64_t now_s, uint64_t *ino) {
    if (dir->format == XFS_DINODE_FMT_LOCAL) {
        return xfs_dir_sf_remove(fs, dir, name, namelen, now_s, ino);
    }
    {
        uint16_t shape = xfs_dir_index_shape(fs, dir);
        if (shape == XFS_DA3_NODE_MAGIC) {
            return xfs_dir_node_remove(fs, dir, name, namelen, now_s, ino);
        }
        if (shape == XFS_DIR3_LEAF1_MAGIC) {
            return xfs_dir_leaf_remove(fs, dir, name, namelen, now_s, ino);
        }
    }
    return xfs_dir_block_remove(fs, dir, name, namelen, now_s, ino);
}
