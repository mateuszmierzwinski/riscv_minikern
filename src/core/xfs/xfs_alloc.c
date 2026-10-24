#include "core/bcache.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs_internal.h"
#include "lib/crc32c.h"

#include <stddef.h>

/* ── the free-space btrees ────────────────────────────────────────────────
 *
 * Two trees describe the same set of free extents, one ordered by where
 * they are and one by how big they are. Every change has to be made to
 * both, and the second ordering is the part that catches people out: a
 * record whose count changes usually has to **move**, because the tree it
 * is in is sorted by that count.
 *
 * Within a single leaf — which is all these trees are on a filesystem this
 * size — moving a record is sorting the leaf, and that is what this does.
 * A tree with branches is refused rather than half-walked.
 *
 * Both directions are here, because a filesystem needs both to be usable:
 * taking blocks for a file that is growing, and giving them back when a
 * file goes away. Giving them back **merges** with the neighbours, because
 * free space in XFS is kept as maximal extents and two adjacent free
 * records are a thing `xfs_repair` will tell you about.
 */

#define ALLOC_MAX_RECORDS 505u /* what a 4 KiB leaf holds */

static uint32_t rec_start(const uint8_t *rec) {
    return xfs_be32(rec + XFS_ALLOC_REC_STARTBLOCK);
}

static uint32_t rec_count(const uint8_t *rec) {
    return xfs_be32(rec + XFS_ALLOC_REC_BLOCKCOUNT);
}

static void rec_set(uint8_t *rec, uint32_t start, uint32_t count) {
    xfs_put_be32(rec + XFS_ALLOC_REC_STARTBLOCK, start);
    xfs_put_be32(rec + XFS_ALLOC_REC_BLOCKCOUNT, count);
}

static uint8_t *leaf_rec(const struct xfs *fs, struct bbuf *b, uint32_t i) {
    return b->data + fs->btree_hdr + i * XFS_ALLOC_REC_SIZE;
}

static uint32_t leaf_count(const struct bbuf *b) {
    return xfs_be16(b->data + XFS_BTREE_NUMRECS);
}

static void leaf_set_count(struct bbuf *b, uint32_t n) {
    xfs_put_be16(b->data + XFS_BTREE_NUMRECS, (uint16_t)n);
}

/* Takes record i out of a leaf, closing the gap. */
static void leaf_remove(const struct xfs *fs, struct bbuf *b, uint32_t i) {
    uint32_t n = leaf_count(b);
    for (uint32_t j = i; j + 1u < n; j++) {
        rec_set(leaf_rec(fs, b, j), rec_start(leaf_rec(fs, b, j + 1u)), rec_count(leaf_rec(fs, b, j + 1u)));
    }
    leaf_set_count(b, n - 1u);
}

/* Makes room at i and puts a record there. */
static int leaf_insert(const struct xfs *fs, struct bbuf *b, uint32_t i, uint32_t start, uint32_t count) {
    uint32_t n = leaf_count(b);
    if (n >= ALLOC_MAX_RECORDS) {
        return KERR_NOSPC; /* the leaf would have to split: not implemented */
    }
    for (uint32_t j = n; j > i; j--) {
        rec_set(leaf_rec(fs, b, j), rec_start(leaf_rec(fs, b, j - 1u)), rec_count(leaf_rec(fs, b, j - 1u)));
    }
    rec_set(leaf_rec(fs, b, i), start, count);
    leaf_set_count(b, n + 1u);
    return KERR_OK;
}

/* Sorts a leaf by count and then by start, which is the order the
 * by-count tree is kept in. An insertion sort over at most a few hundred
 * records, run once per allocation: the plain thing, and the right one. */
static void leaf_sort_by_count(const struct xfs *fs, struct bbuf *b) {
    uint32_t n = leaf_count(b);
    for (uint32_t i = 1; i < n; i++) {
        uint32_t start = rec_start(leaf_rec(fs, b, i)), count = rec_count(leaf_rec(fs, b, i));
        uint32_t j = i;
        while (j > 0) {
            uint32_t ps = rec_start(leaf_rec(fs, b, j - 1u)), pc = rec_count(leaf_rec(fs, b, j - 1u));
            if (pc < count || (pc == count && ps < start)) {
                break;
            }
            rec_set(leaf_rec(fs, b, j), ps, pc);
            j--;
        }
        rec_set(leaf_rec(fs, b, j), start, count);
    }
}

/* The group's free-space header, verified like every other structure. */
static int agf_get(struct xfs *fs, uint32_t agno, struct bbuf **out, uint8_t **agf) {
    uint8_t *at;
    int rc = bcache_get(fs->cache, xfs_group_block(fs, agno), out);
    if (rc != KERR_OK) {
        return rc;
    }
    at = (*out)->data + XFS_AGF_SECTOR * fs->sectsize;
    if (xfs_be32(at) != XFS_AGF_MAGIC || !xfs_crc_ok(fs, at, fs->sectsize, XFS_AGF_CRC) ||
        !xfs_uuid_ok(fs, at + XFS_AGF_UUID)) {
        kprintf("xfs: group %u's AGF is unusable: magic %08x, crc %s, uuid %s\n", agno,
                xfs_be32(at), xfs_crc_ok(fs, at, fs->sectsize, XFS_AGF_CRC) ? "ok" : "bad",
                xfs_uuid_ok(fs, at + XFS_AGF_UUID) ? "ok" : "bad");
        bcache_put(fs->cache, *out);
        return KERR_IO;
    }
    *agf = at;
    return KERR_OK;
}

/* The largest free extent a leaf describes, which is what the header's
 * "longest" field must say. Recomputed rather than adjusted: adjusting it
 * is how it drifts, and a scan of one leaf costs nothing. */
static uint32_t longest_in(const struct xfs *fs, const struct bbuf *b) {
    uint32_t n = xfs_be16(b->data + XFS_BTREE_NUMRECS), best = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *rec = b->data + fs->btree_hdr + i * XFS_ALLOC_REC_SIZE;
        uint32_t count = xfs_be32(rec + XFS_ALLOC_REC_BLOCKCOUNT);
        best = count > best ? count : best;
    }
    return best;
}

/* The superblock's free-block counter, moved by delta. */
static int count_blocks_free(struct xfs *fs, int64_t delta) {
    struct bbuf *b;
    int rc = bcache_get(fs->cache, 0, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_put_be64(b->data + XFS_SB_FDBLOCKS, xfs_be64(b->data + XFS_SB_FDBLOCKS) + (uint64_t)delta);
    xfs_reseal(fs, b->data, fs->sectsize, XFS_SB_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* Everything an allocation or a free touches at once, so that the three
 * buffers are got and put in one place rather than in every operation. */
struct alloc_ctx {
    struct bbuf *agfb, *bnob, *cntb;
    uint8_t *agf;
};

static void ctx_put(struct xfs *fs, struct alloc_ctx *c, bool dirty) {
    if (dirty) {
        xfs_put_be32(c->agf + XFS_AGF_LONGEST, longest_in(fs, c->bnob));
        xfs_reseal(fs, c->bnob->data, fs->blocksize, XFS_BTREE_CRC);
        xfs_reseal(fs, c->cntb->data, fs->blocksize, XFS_BTREE_CRC);
        xfs_reseal(fs, c->agf, fs->sectsize, XFS_AGF_CRC);
        bcache_dirty(c->bnob);
        bcache_dirty(c->cntb);
        bcache_dirty(c->agfb);
    }
    bcache_put(fs->cache, c->cntb);
    bcache_put(fs->cache, c->bnob);
    bcache_put(fs->cache, c->agfb);
}

/* Opens a group's free space: its header and both trees, each checked, and
 * each refused if it has branches. */
static int ctx_get(struct xfs *fs, uint32_t agno, struct alloc_ctx *c) {
    int rc = agf_get(fs, agno, &c->agfb, &c->agf);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_tree_get(fs, agno, xfs_be32(c->agf + XFS_AGF_ROOTS), xfs_magic_for(fs, XFS_BLK_BNOBT), &c->bnob);
    if (rc != KERR_OK) {
        bcache_put(fs->cache, c->agfb);
        return rc;
    }
    rc = xfs_tree_get(fs, agno, xfs_be32(c->agf + XFS_AGF_ROOTS + 4u), xfs_magic_for(fs, XFS_BLK_CNTBT), &c->cntb);
    if (rc != KERR_OK) {
        bcache_put(fs->cache, c->bnob);
        bcache_put(fs->cache, c->agfb);
        return rc;
    }
    return KERR_OK;
}

/* Takes one block off the front of the record at i, in whichever tree. */
/* Takes `n` blocks off the front of the record at i, removing it when that
 * is all of them. */
static void take_front_n(const struct xfs *fs, struct bbuf *b, uint32_t i, uint32_t n) {
    uint8_t *rec = leaf_rec(fs, b, i);
    if (rec_count(rec) == n) {
        leaf_remove(fs, b, i);
        return;
    }
    rec_set(rec, rec_start(rec) + n, rec_count(rec) - n);
}

/* The smallest free extent that holds `n` blocks, by its start.
 *
 * The by-count tree is kept sorted by count, so the first record big enough
 * is the **best fit** — which is what XFS does, and what leaves the large
 * extents intact for the next caller that needs one. Returns 0 when nothing
 * fits, which is not a block number any allocation can return: block 0 of a
 * group holds its superblock. */
static uint32_t smallest_fit(const struct xfs *fs, struct bbuf *cnt, uint32_t n) {
    uint32_t records = leaf_count(cnt);
    for (uint32_t i = 0; i < records; i++) {
        if (rec_count(leaf_rec(fs, cnt, i)) >= n) {
            return rec_start(leaf_rec(fs, cnt, i));
        }
    }
    return 0;
}

/* The index of the record starting exactly here, or -1. */
static int find_start(const struct xfs *fs, struct bbuf *b, uint32_t start) {
    for (uint32_t i = 0; i < leaf_count(b); i++) {
        if (rec_start(leaf_rec(fs, b, i)) == start) {
            return (int)i;
        }
    }
    return -1;
}

/* Takes the one block at `want`, if it is free; otherwise the first block
 * of the largest free extent there is.
 *
 * Preferring a particular block is what lets a growing file keep one
 * extent instead of collecting them: ask for the block after the file's
 * last one and, when it is free, the file's extent simply gets longer. */
/* Whether the free extent starting at `at` is long enough for `n`. */
static bool long_enough(const struct xfs *fs, struct bbuf *bno, int at, uint32_t n) {
    return at >= 0 && rec_count(leaf_rec(fs, bno, (uint32_t)at)) >= n;
}

int xfs_alloc_extent(struct xfs *fs, uint32_t agno, uint32_t count, uint32_t want,
                     uint32_t *got) {
    struct alloc_ctx c;
    int rc;
    int at;
    if (count == 0) {
        return KERR_INVAL;
    }
    rc = ctx_get(fs, agno, &c);
    if (rc != KERR_OK) {
        return rc;
    }
    at = want != 0 ? find_start(fs, &*c.bnob, want) : -1;
    if (!long_enough(fs, c.bnob, at, count)) {
        /* **The best fit rather than the largest.** A single block used to
         * take the largest extent there was, which is harmless when every
         * allocation is one block and wasteful the moment one asks for
         * eight: it breaks up the one run a later caller may need whole. */
        want = smallest_fit(fs, c.cntb, count);
        at = want != 0 ? find_start(fs, &*c.bnob, want) : -1;
        if (!long_enough(fs, c.bnob, at, count)) {
            kprintf("xfs: group %u has no run of %u block(s) (best offer %u)\n", agno, count,
                    want);
            ctx_put(fs, &c, false);
            return want == 0 ? KERR_NOSPC : KERR_IO;
        }
    }
    take_front_n(fs, c.bnob, (uint32_t)at, count);
    at = find_start(fs, &*c.cntb, want);
    if (at < 0) {
        ctx_put(fs, &c, false); /* the two trees disagree: not ours to fix */
        return KERR_IO;
    }
    take_front_n(fs, c.cntb, (uint32_t)at, count);
    leaf_sort_by_count(fs, c.cntb);
    xfs_put_be32(c.agf + XFS_AGF_FREEBLKS, xfs_be32(c.agf + XFS_AGF_FREEBLKS) - count);
    ctx_put(fs, &c, true);
    *got = want;
    return count_blocks_free(fs, -(int32_t)count);
}

int xfs_alloc_block(struct xfs *fs, uint32_t agno, uint32_t want, uint32_t *got) {
    return xfs_alloc_extent(fs, agno, 1u, want, got);
}

int xfs_alloc_somewhere(struct xfs *fs, uint32_t *agno, uint32_t count, uint32_t want,
                        uint32_t *got) {
    uint32_t first = *agno < fs->agcount ? *agno : 0;
    int rc = xfs_alloc_extent(fs, first, count, want, got);
    if (rc != KERR_NOSPC || fs->single_group) {
        *agno = first;
        return rc;
    }
    /* **Every other group, in order.** A filesystem is several groups and
     * an allocator that only ever asked the one it started in would refuse
     * a write with most of the disk free: four groups is `mkfs`'s default
     * for a small filesystem, so that refusal arrives at about a quarter
     * full. The hint is dropped — it was an address inside the group that
     * had no room, and it means nothing in any other one. */
    for (uint32_t i = 0; i < fs->agcount; i++) {
        if (i == first) {
            continue;
        }
        rc = xfs_alloc_extent(fs, i, count, 0, got);
        if (rc != KERR_NOSPC) {
            *agno = i;
            fs->alloc_other_group += rc == KERR_OK ? 1u : 0u;
            return rc;
        }
    }
    return KERR_NOSPC;
}

/* Puts an extent back, joined to whatever it touches.
 *
 * Free space is kept as maximal extents, so an extent that abuts the one
 * before or after it becomes part of that record rather than a new one —
 * both of them when it fills a gap exactly. Leaving two adjacent free
 * records instead is something `xfs_repair` notices. */
static int bno_give_back(const struct xfs *fs, struct bbuf *b, uint32_t start, uint32_t count) {
    uint32_t n = leaf_count(b), i = 0;
    while (i < n && rec_start(leaf_rec(fs, b, i)) < start) {
        i++;
    }
    bool after_prev = i > 0 && rec_start(leaf_rec(fs, b, i - 1u)) + rec_count(leaf_rec(fs, b, i - 1u)) == start;
    bool before_next = i < n && start + count == rec_start(leaf_rec(fs, b, i));
    if (after_prev && before_next) {
        rec_set(leaf_rec(fs, b, i - 1u), rec_start(leaf_rec(fs, b, i - 1u)),
                rec_count(leaf_rec(fs, b, i - 1u)) + count + rec_count(leaf_rec(fs, b, i)));
        leaf_remove(fs, b, i);
        return KERR_OK;
    }
    if (after_prev) {
        rec_set(leaf_rec(fs, b, i - 1u), rec_start(leaf_rec(fs, b, i - 1u)), rec_count(leaf_rec(fs, b, i - 1u)) + count);
        return KERR_OK;
    }
    if (before_next) {
        rec_set(leaf_rec(fs, b, i), start, count + rec_count(leaf_rec(fs, b, i)));
        return KERR_OK;
    }
    return leaf_insert(fs, b, i, start, count);
}

/* The same extent in the by-count tree, which is rebuilt from the other
 * one: after a merge the by-count tree's records are no longer a simple
 * function of what was added, and copying the by-block tree is both
 * shorter and impossible to get subtly wrong. */
static void cnt_rebuild(const struct xfs *fs, struct bbuf *cnt, const struct bbuf *bno) {
    uint32_t n = xfs_be16(bno->data + XFS_BTREE_NUMRECS);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *src = bno->data + fs->btree_hdr + i * XFS_ALLOC_REC_SIZE;
        rec_set(leaf_rec(fs, cnt, i), xfs_be32(src + XFS_ALLOC_REC_STARTBLOCK),
                xfs_be32(src + XFS_ALLOC_REC_BLOCKCOUNT));
    }
    leaf_set_count(cnt, n);
    leaf_sort_by_count(fs, cnt);
}

int xfs_free_extent(struct xfs *fs, uint32_t agno, uint32_t start, uint32_t count) {
    struct alloc_ctx c;
    int rc = count == 0 ? KERR_INVAL : ctx_get(fs, agno, &c);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = bno_give_back(fs, c.bnob, start, count);
    if (rc != KERR_OK) {
        ctx_put(fs, &c, false);
        return rc;
    }
    cnt_rebuild(fs, c.cntb, c.bnob);
    xfs_put_be32(c.agf + XFS_AGF_FREEBLKS, xfs_be32(c.agf + XFS_AGF_FREEBLKS) + count);
    ctx_put(fs, &c, true);
    return count_blocks_free(fs, (int64_t)count);
}
