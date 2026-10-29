/* The inode b-trees: every chunk of 64 inodes (`inobt`), and the chunks
 * that still have a free inode (`finobt`), as whole B+trees (xfs_btree.c).
 *
 * **What the single leaf could not do.** Both trees were written as lone
 * leaves, and the writer would not take a chunk's last free inode — that
 * means deleting the chunk's record from the free-inode tree, which it did
 * not do. Linux does it all the time: a chunk Linux filled has no
 * free-inode record at all, so freeing any inode in it here needs a record
 * *inserted*, and that was refused — every file Linux had copied onto a
 * disk could not be deleted by this kernel. Both are ordinary tree edits
 * now, and the trees grow past one leaf (about 250 chunks in a group).
 *
 * A record's fields have two layouts. Without sparse inode chunks the free
 * count is a 32-bit word. With them — `mkfs.xfs`'s default — the same four
 * bytes are a 16-bit **hole mask**, an 8-bit count of the inodes the chunk
 * really has and an 8-bit free count, and each hole-mask bit covers four
 * inodes that do not exist: the format marks them free, so `usable` takes
 * them out before an inode is chosen.
 *
 * The trees' blocks come from the ordinary allocator, in the group, and are
 * the inode trees' in the reverse map (`OWN_INOBT`); with `inobtcount` the
 * AGI counts them too. */
#include "core/bcache.h"
#include "core/errno.h"
#include "core/xfs/xfs_btree.h"

#define INOBT_HOLEMASK 4u /* u16, sparse only */
#define INOBT_COUNT    6u /* u8, sparse only: the inodes the chunk has */
#define INOBT_SPFREE   7u /* u8, sparse only */
#define INODES_PER_HOLE_BIT 4u
#define AGI_IBLOCKS 336u /* u32, inobtcount: inode-tree blocks */
#define AGI_FBLOCKS 340u /* u32, inobtcount: free-inode-tree blocks */

uint32_t xfs_inobt_freecount(const struct xfs *fs, const uint8_t *rec) {
    return fs->sparse_inodes ? rec[INOBT_SPFREE] : xfs_be32(rec + XFS_INOBT_REC_FREECOUNT);
}

void xfs_inobt_set_freecount(const struct xfs *fs, uint8_t *rec, uint32_t n) {
    if (fs->sparse_inodes) {
        rec[INOBT_SPFREE] = (uint8_t)n;
    } else {
        xfs_put_be32(rec + XFS_INOBT_REC_FREECOUNT, n);
    }
}

uint64_t xfs_inobt_usable(const struct xfs *fs, const uint8_t *rec) {
    uint64_t free = xfs_be64(rec + XFS_INOBT_REC_FREE);
    uint16_t holes = fs->sparse_inodes ? xfs_be16(rec + INOBT_HOLEMASK) : 0;
    for (uint32_t bit = 0; bit < 16u; bit++) {
        if ((holes & (1u << bit)) != 0) {
            free &= ~((uint64_t)0xf << (bit * INODES_PER_HOLE_BIT));
        }
    }
    return free;
}

void xfs_inobt_rec_init(const struct xfs *fs, uint8_t *rec, uint32_t startino) {
    xfs_put_be32(rec + XFS_INOBT_REC_STARTINO, startino);
    if (fs->sparse_inodes) {
        xfs_put_be16(rec + INOBT_HOLEMASK, 0);
        rec[INOBT_COUNT] = (uint8_t)XFS_INODES_PER_CHUNK;
    }
    xfs_inobt_set_freecount(fs, rec, XFS_INODES_PER_CHUNK);
    xfs_put_be64(rec + XFS_INOBT_REC_FREE, ~(uint64_t)0);
}

/* ── the two trees ────────────────────────────────────────────────────── */

static void ino_key(const uint8_t *rec, uint8_t *key) {
    xfs_copy_bytes(key, rec + XFS_INOBT_REC_STARTINO, 4u);
}

static int ino_cmp(const uint8_t *a, const uint8_t *b) {
    uint32_t x = xfs_be32(a), y = xfs_be32(b);
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int tree_alloc(struct xfs_btree_cur *cur, uint32_t *agbno);
static int tree_free(struct xfs_btree_cur *cur, uint32_t agbno);

static const struct xfs_btree_ops inobt_ops = {
    .rec_size = XFS_INOBT_REC_SIZE, .key_size = 4u, .in_agi = true,
    .root_off = XFS_AGI_ROOT, .level_off = XFS_AGI_LEVEL,
    .low_key = ino_key, .high_key = ino_key, .cmp = ino_cmp,
    .alloc_block = tree_alloc, .free_block = tree_free,
};

static const struct xfs_btree_ops finobt_ops = {
    .rec_size = XFS_INOBT_REC_SIZE, .key_size = 4u, .in_agi = true,
    .root_off = XFS_AGI_FREE_ROOT, .level_off = XFS_AGI_FREE_LEVEL,
    .low_key = ino_key, .high_key = ino_key, .cmp = ino_cmp,
    .alloc_block = tree_alloc, .free_block = tree_free,
};

/* The AGI's count of one tree's blocks, kept with `inobtcount`. */
static int count_tree_block(struct xfs_btree_cur *cur, int32_t delta) {
    struct bbuf *b;
    uint8_t *agi;
    uint32_t off = cur->ops == &finobt_ops ? AGI_FBLOCKS : AGI_IBLOCKS;
    int rc = cur->fs->inobt_counts ? xfs_agi_get(cur->fs, cur->agno, &b, &agi) : KERR_OK;
    if (rc != KERR_OK || !cur->fs->inobt_counts) {
        return rc;
    }
    xfs_put_be32(agi + off, xfs_be32(agi + off) + (uint32_t)delta);
    xfs_reseal(cur->fs, agi, cur->fs->sectsize, XFS_AGI_CRC);
    bcache_dirty(b);
    bcache_put(cur->fs->cache, b);
    return KERR_OK;
}

static int tree_alloc(struct xfs_btree_cur *cur, uint32_t *agbno) {
    int rc = xfs_alloc_extent(cur->fs, cur->agno, 1u, 0, agbno);
    rc = rc != KERR_OK ? rc
                       : xfs_rmap_map(cur->fs, ((uint64_t)cur->agno << cur->fs->agblklog) | *agbno,
                                      1u, XFS_RMAP_OWN_INOBT, 0);
    return rc != KERR_OK ? rc : count_tree_block(cur, 1);
}

static int tree_free(struct xfs_btree_cur *cur, uint32_t agbno) {
    int rc = xfs_free_owned(cur->fs, ((uint64_t)cur->agno << cur->fs->agblklog) | agbno, 1u,
                            XFS_RMAP_OWN_INOBT, 0);
    return rc != KERR_OK ? rc : count_tree_block(cur, -1);
}

static int open_tree(struct xfs *fs, uint32_t agno, bool free_tree, struct xfs_btree_cur *cur) {
    int rc = xfs_btree_open(cur, fs, free_tree ? &finobt_ops : &inobt_ops, agno);
    cur->magic = xfs_magic_for(fs, free_tree ? XFS_BLK_FINOBT : XFS_BLK_INOBT);
    return rc;
}

static int seek(struct xfs_btree_cur *cur, uint32_t agino) {
    uint8_t key[4];
    xfs_put_be32(key, agino);
    return xfs_btree_lookup_le(cur, key);
}

/* The record whose chunk holds `agino`, in one tree; the cursor is left on
 * it. KERR_NOENT when no chunk does. */
static int record_for(struct xfs *fs, uint32_t agno, bool free_tree, uint32_t agino,
                      struct xfs_btree_cur *cur, uint8_t *rec) {
    int rc = open_tree(fs, agno, free_tree, cur);
    rc = rc != KERR_OK ? rc : seek(cur, agino);
    rc = rc != KERR_OK ? rc : xfs_btree_get(cur, rec);
    return rc == KERR_OK && agino - xfs_be32(rec + XFS_INOBT_REC_STARTINO) >= XFS_INODES_PER_CHUNK
               ? KERR_NOENT
               : rc;
}

/* The first record of a tree at or after `from` that has a usable free
 * inode. The free-inode tree's every record does; without one, the inode
 * tree is walked. */
static int first_with_free(struct xfs *fs, uint32_t agno, bool free_tree, uint8_t *rec) {
    struct xfs_btree_cur cur;
    int rc = open_tree(fs, agno, free_tree, &cur);
    rc = rc != KERR_OK ? rc : seek(&cur, 0);
    rc = rc != KERR_OK ? rc : xfs_btree_get(&cur, rec);
    if (rc == KERR_NOENT) {
        rc = xfs_btree_peek_next(&cur, rec);
    }
    while (rc == KERR_OK && xfs_inobt_usable(fs, rec) == 0) {
        rc = seek(&cur, xfs_be32(rec + XFS_INOBT_REC_STARTINO));
        rc = rc != KERR_OK ? rc : xfs_btree_peek_next(&cur, rec);
    }
    return rc;
}

/* Writes `rec` over the record with its start in one tree — or, with
 * `drop`, takes it out; or, when the tree has none, puts it in. */
static int put_record(struct xfs *fs, uint32_t agno, bool free_tree, const uint8_t *rec,
                      bool drop) {
    struct xfs_btree_cur cur;
    uint8_t have[XFS_INOBT_REC_SIZE];
    uint32_t start = xfs_be32(rec + XFS_INOBT_REC_STARTINO);
    int rc = record_for(fs, agno, free_tree, start, &cur, have);
    if (rc == KERR_OK && xfs_be32(have + XFS_INOBT_REC_STARTINO) == start) {
        return drop ? xfs_btree_delete(&cur) : xfs_btree_update(&cur, rec);
    }
    if (rc != KERR_OK && rc != KERR_NOENT) {
        return rc;
    }
    return drop ? KERR_OK : xfs_btree_insert(&cur, rec);
}

static int agi_free_count(struct xfs *fs, uint32_t agno, int32_t delta) {
    struct bbuf *b;
    uint8_t *agi;
    int rc = xfs_agi_get(fs, agno, &b, &agi);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_put_be32(agi + XFS_AGI_FREECOUNT, xfs_be32(agi + XFS_AGI_FREECOUNT) + (uint32_t)delta);
    xfs_reseal(fs, agi, fs->sectsize, XFS_AGI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* The lowest usable free inode of a record, as a bit number. */
static uint32_t lowest_free(const struct xfs *fs, const uint8_t *rec) {
    uint64_t free = xfs_inobt_usable(fs, rec);
    uint32_t bit = 0;
    while (bit < 63u && (free & ((uint64_t)1 << bit)) == 0) {
        bit++;
    }
    return bit;
}

int xfs_inobt_take(struct xfs *fs, uint32_t agno, uint32_t *agino) {
    uint8_t rec[XFS_INOBT_REC_SIZE];
    uint32_t bit, left;
    int rc = xfs_agfl_ensure(fs, agno);
    rc = rc != KERR_OK ? rc : first_with_free(fs, agno, fs->has_finobt, rec);
    if (rc != KERR_OK) {
        return rc; /* KERR_NOENT: every chunk is full, and the caller adds one */
    }
    bit = lowest_free(fs, rec);
    left = xfs_inobt_freecount(fs, rec) - 1u;
    xfs_put_be64(rec + XFS_INOBT_REC_FREE,
                 xfs_be64(rec + XFS_INOBT_REC_FREE) & ~((uint64_t)1 << bit));
    xfs_inobt_set_freecount(fs, rec, left);
    rc = put_record(fs, agno, false, rec, false);
    rc = rc != KERR_OK || !fs->has_finobt ? rc : put_record(fs, agno, true, rec, left == 0);
    *agino = xfs_be32(rec + XFS_INOBT_REC_STARTINO) + bit;
    return rc != KERR_OK ? rc : agi_free_count(fs, agno, -1);
}

int xfs_inobt_release(struct xfs *fs, uint32_t agno, uint32_t agino) {
    struct xfs_btree_cur cur;
    uint8_t rec[XFS_INOBT_REC_SIZE];
    uint32_t bit;
    int rc = xfs_agfl_ensure(fs, agno);
    rc = rc != KERR_OK ? rc : record_for(fs, agno, false, agino, &cur, rec);
    if (rc != KERR_OK) {
        return rc == KERR_NOENT ? KERR_IO : rc; /* an inode no chunk holds */
    }
    bit = agino - xfs_be32(rec + XFS_INOBT_REC_STARTINO);
    xfs_put_be64(rec + XFS_INOBT_REC_FREE,
                 xfs_be64(rec + XFS_INOBT_REC_FREE) | ((uint64_t)1 << bit));
    xfs_inobt_set_freecount(fs, rec, xfs_inobt_freecount(fs, rec) + 1u);
    rc = xfs_btree_update(&cur, rec);
    /* A chunk that was full has no free-inode record — Linux takes it out
     * when its last inode goes — so this one is put back in. */
    rc = rc != KERR_OK || !fs->has_finobt ? rc : put_record(fs, agno, true, rec, false);
    return rc != KERR_OK ? rc : agi_free_count(fs, agno, 1);
}

int xfs_inobt_add_chunk(struct xfs *fs, uint32_t agno, uint32_t startino) {
    uint8_t rec[XFS_INOBT_REC_SIZE];
    int rc = xfs_agfl_ensure(fs, agno);
    xfs_inobt_rec_init(fs, rec, startino);
    rc = rc != KERR_OK ? rc : put_record(fs, agno, false, rec, false);
    return rc != KERR_OK || !fs->has_finobt ? rc : put_record(fs, agno, true, rec, false);
}
