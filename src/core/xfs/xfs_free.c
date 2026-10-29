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
    sf = di + fs->dinode_size;
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
    xfs_reseal(fs, di, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* Returns an inode to both trees and to the counters.
 *
 * The free-inode tree must already describe this chunk. It will, for any
 * inode this kernel allocated — it never takes a chunk's last free one, so
 * the record never goes away — and refusing otherwise is better than
 * inserting a btree record badly. */
static int release_inode(struct xfs *fs, uint32_t agno, uint64_t ino) {
    /* The records are keyed by the **group-relative** inode number, not the
     * whole one: the group is in the high bits and must come off first, or
     * no record matches and a remove fails with EIO for no reason the
     * caller can act on. */
    uint64_t agino = ino & (((uint64_t)1 << (fs->agblklog + fs->inopblog)) - 1u);
    return xfs_inobt_release(fs, agno, (uint32_t)agino);
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
    di[XFS_DI_VERSION] = fs->v5 ? XFS_DI_VERSION_3 : XFS_DI_VERSION_2;
    xfs_put_be32(di + XFS_DI_GEN, gen);
    xfs_put_be32(di + XFS_DI_NEXT_UNLINKED, XFS_NULL_AGINO);
    /* The last two fields a v3 core has and a v2 one does not. On v4 both
     * offsets are inside the literal area, so writing them puts an inode
     * number and a UUID into the fork of an inode that has just been
     * cleared — harmless to this reader, and exactly the sort of thing
     * `xfs_repair` reports as corruption. */
    if (fs->v5) {
        xfs_put_be64(di + XFS_DI_INO, ino);
        for (uint32_t i = 0; i < 16; i++) {
            di[XFS_DI_UUID + i] = fs->uuid[i];
        }
    }
    xfs_reseal(fs, di, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* Gives back one extent of a file's data: to the realtime bitmap for a
 * realtime file, whose blocks never belonged to a group, and otherwise out
 * of the reverse map and to the group it came from. */
static int free_data_extent(struct xfs *fs, const struct xfs_inode *ip,
                            const struct xfs_extent *e) {
    if (ip->realtime) {
        return xfs_rt_release(fs, e->startblock / fs->rextsize, e->blockcount / fs->rextsize);
    }
    return xfs_free_data(fs, ip, e->startblock, e->blockcount, xfs_rmap_offset_of(e, 0));
}

/* Whether every block the file owns is one this can give back, asked before
 * any is: a refusal part way through would leave a file that owns half of
 * what it did.
 *
 * Refused: an attribute fork that is a b-tree; a realtime extent that is
 * not whole realtime extents; and a file that may share blocks with
 * another, where freeing one frees it from under the other. Each used to be
 * freed anyway — a b-tree root walked as if it were extents, a realtime
 * file's blocks put into a group's free space, an attribute fork's blocks
 * not freed at all. A data fork that is a b-tree is freed like any other
 * now: its map is loaded whole (xfs_fork.c). */
static int frees_cleanly(const struct xfs *fs, const struct xfs_inode *ip,
                         const struct xfs_fork_map *m) {
    if (ip->aformat == XFS_DINODE_FMT_BTREE || (ip->shares_blocks && !fs->has_rmapbt)) {
        return KERR_NOTSUP; /* sharing without a reverse map: not a shape this frees */
    }
    for (uint32_t i = 0; ip->realtime && i < m->count; i++) {
        if (fs->rextsize == 0 || m->e[i].startblock % fs->rextsize != 0 ||
            m->e[i].blockcount % fs->rextsize != 0) {
            return KERR_NOTSUP;
        }
    }
    return KERR_OK;
}

/* Frees every extent of the map, then stores it empty — which gives back
 * the tree's leaves when it had any. */
static int free_map(struct xfs *fs, const struct xfs_inode *ip, uint8_t *di,
                    struct xfs_fork_map *m) {
    int rc = frees_cleanly(fs, ip, m);
    for (uint32_t i = 0; rc == KERR_OK && i < m->count; i++) {
        rc = m->e[i].blockcount == 0 ? KERR_OK : free_data_extent(fs, ip, &m->e[i]);
    }
    if (rc == KERR_OK) {
        m->count = 0;
        rc = xfs_fork_store(fs, ip->ino, di, m);
    }
    return rc;
}

/* Gives a file's blocks back: its data, then its attributes' blocks.
 *
 * It happens **before** the directory entry goes, so that a failure half
 * way leaves a file that still exists and still owns what it owns — a leak
 * is worse than a file that would not delete. */
/* `noinline`: its caller already holds an inode, and folding this frame into
 * that one is what crosses the 1 KiB limit. */
__attribute__((noinline)) static int free_file_blocks(struct xfs *fs, const struct xfs_inode *ip) {
    struct xfs_fork_map m;
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    int rc;
    if (ip->nblocks == 0 || ip->format == XFS_DINODE_FMT_LOCAL) {
        return KERR_OK;
    }
    xfs_inode_place(fs, ip->ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_fork_load(fs, b->data + boffset, &m);
    rc = rc != KERR_OK ? rc : free_map(fs, ip, b->data + boffset, &m);
    xfs_reseal(fs, b->data + boffset, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return rc != KERR_OK ? rc : xfs_attr_free_blocks(fs, ip);
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

/* Takes one name away from a file whose inode this reads.
 *
 * **Split out so the inode has a frame to itself**, and `noinline` so it
 * keeps one: an inode is the largest object this code puts on a stack — a
 * 1024-byte inode's literal area is 924 bytes of it — and folded into
 * `xfs_unlink`'s frame the pair crosses the 1 KiB limit every function here
 * is held to. That limit is what stops a kernel stack being overrun
 * silently, so it is not something to raise for one caller.
 *
 * The division is honest rather than mechanical: everything above this needs
 * only the *directory*, and everything in here needs the file. */
/* Removes one name from a file that has others: the entry goes, the link
 * count drops, and the file stays.
 *
 * **Its own frame, so its caller's holds only the inode.** The caller reads
 * the inode to find out which of the two cases this is, and anything else on
 * that frame crosses the limit — this path was 32 bytes over on its own. */
__attribute__((noinline)) static int drop_one_of_several_names(struct xfs *fs,
                                                                const struct xfs_inode *dir,
                                                                const char *name,
                                                                uint32_t namelen, uint64_t now_s) {
    uint64_t ino = 0;
    int rc = xfs_dir_sf_remove(fs, dir, name, namelen, now_s, &ino);
    rc = rc != KERR_OK ? rc : xfs_inode_adjust_nlink(fs, ino, -1);
    return rc != KERR_OK ? rc : log_unlink(fs, dir->ino);
}

/* Removes the **last** name of a file, which also removes the file: its
 * blocks, its inode, the group's counts and the owner's quota.
 *
 * Takes the inode rather than reading it, so that the frame holding the
 * inode does nothing else — this is the second split in this path and the
 * reason is the same each time: an inode is 1 KiB-ish on its own and the
 * limit is per frame, so every function that holds one must hold *only* it
 * and a few words. */
__attribute__((noinline)) static int remove_the_last_name(struct xfs *fs,
                                                           const struct xfs_inode *dir,
                                                           const struct xfs_inode *ip,
                                                           const char *name, uint32_t namelen,
                                                           uint64_t now_s) {
    uint64_t ino = 0;
    int rc = free_file_blocks(fs, ip);
    rc = rc != KERR_OK ? rc : xfs_dir_remove(fs, dir, name, namelen, now_s, &ino);
    rc = rc != KERR_OK ? rc : release_inode(fs, xfs_fsb_ag(fs, ino >> fs->inopblog), ino);
    rc = rc != KERR_OK ? rc : xfs_count_inodes_free(fs, 1);
    /* **The refund, after the file is actually gone.** Charging is done
     * before taking so that a refusal costs nothing; giving back is done
     * after releasing so that a failure part way through does not credit an
     * owner for a file they still have. The blocks go back too: the inode's
     * own count is what they were charged as. */
    if (rc == KERR_OK) {
        (void)xfs_quota_charge_all(fs, ip, -(int64_t)ip->nblocks, -1);
    }
    rc = rc != KERR_OK ? rc : clear_inode(fs, ino);
    return rc != KERR_OK ? rc : log_unlink(fs, dir->ino);
}

__attribute__((noinline)) static int take_one_name_away(struct xfs *fs,
                                                         const struct xfs_inode *dir,
                                                         const char *name, uint32_t namelen,
                                                         uint64_t ino, uint64_t now_s) {
    struct xfs_inode ip;
    int rc = xfs_inode_read(fs, ino, &ip);
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfs_is_dir(&ip)) {
        return KERR_ISDIR; /* removing a directory is rmdir, and is not this */
    }
    if (ip.format != XFS_DINODE_FMT_EXTENTS && ip.format != XFS_DINODE_FMT_BTREE &&
        ip.nblocks != 0) {
        return KERR_NOTSUP; /* a fork shape whose blocks this cannot find */
    }
    /* **A name, not a file.** `unlink` removes one link; the file goes
     * when the last one does. This refused anything with a count above one
     * because nothing could make such a file — hard links did not exist —
     * and now that they do, removing one name from a file with two would
     * have freed the inode under the other name. */
    if (ip.nlink > 1u) {
        return drop_one_of_several_names(fs, dir, name, namelen, now_s);
    }
    return remove_the_last_name(fs, dir, &ip, name, namelen, now_s);
}

int xfs_unlink(struct xfs *fs, const struct xfs_inode *dir, const char *name, uint32_t namelen,
               uint64_t now_s) {
    uint64_t ino = 0;
    int rc;
    if (!xfs_writable(fs)) {
        return KERR_ROFS;
    }
    if (!xfs_is_dir(dir)) {
        return KERR_NOTDIR;
    }
    if (dir->format != XFS_DINODE_FMT_LOCAL && dir->format != XFS_DINODE_FMT_EXTENTS &&
        dir->format != XFS_DINODE_FMT_BTREE) {
        return KERR_NOTSUP; /* a fork shape the write path cannot edit */
    }
    rc = xfs_dir_lookup(fs, dir, name, namelen, &ino);
    return rc != KERR_OK ? rc : take_one_name_away(fs, dir, name, namelen, ino, now_s);
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
        if (shape == xfs_magic_for(fs, XFS_BLK_DA_NODE)) {
            return xfs_dir_node_remove(fs, dir, name, namelen, now_s, ino);
        }
        if (shape == xfs_magic_for(fs, XFS_BLK_DIR_LEAF1)) {
            return xfs_dir_leaf_remove(fs, dir, name, namelen, now_s, ino);
        }
    }
    return xfs_dir_block_remove(fs, dir, name, namelen, now_s, ino);
}
