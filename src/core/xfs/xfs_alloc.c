#include "core/bcache.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs_btree.h"
#include "lib/crc32c.h"

#include <stddef.h>

/* ── the free-space btrees ────────────────────────────────────────────────
 *
 * Two trees describe the same set of free extents, one ordered by where
 * they are (`bnobt`, keyed by start) and one by how big they are (`cntbt`,
 * keyed by length then start). Every change is made to both, as a delete of
 * the records that change and an insert of what they become: in the
 * by-length tree a record whose length changes has to *move*, and a delete
 * and an insert is the one way to move a record that needs no special case.
 *
 * Both trees are whole B+trees (xfs_btree.c), growing into the group's free
 * list (xfs_agfl.c). They were single leaves, and a split was refused: on a
 * disk Linux had used that was already a refusal to write anything, and on
 * one this kernel used it arrived at the 506th free extent in a group.
 *
 * Free space is kept as maximal extents, so a freed extent is joined to the
 * free extents either side of it; two touching free records are something
 * `xfs_repair` reports.
 */

struct ext {
    uint32_t start;
    uint32_t count;
};

static void rec_pack(uint8_t *p, const struct ext *e) {
    xfs_put_be32(p + XFS_ALLOC_REC_STARTBLOCK, e->start);
    xfs_put_be32(p + XFS_ALLOC_REC_BLOCKCOUNT, e->count);
}

static void rec_unpack(const uint8_t *p, struct ext *e) {
    e->start = xfs_be32(p + XFS_ALLOC_REC_STARTBLOCK);
    e->count = xfs_be32(p + XFS_ALLOC_REC_BLOCKCOUNT);
}

static int cmp_u32(uint32_t a, uint32_t b) {
    return a < b ? -1 : (a > b ? 1 : 0);
}

/* **Both trees' keys are the record itself**, start then length
 * (`xfs_alloc_key`), whatever the tree orders by. The by-block key was four
 * bytes here and the by-length key length-first: self-consistent, so every
 * write succeeded, and a node `xfs_repair` read with its pointers four bytes
 * from where they were — it saw a tree of zeroes. */
static void alloc_key(const uint8_t *rec, uint8_t *key) {
    xfs_copy_bytes(key, rec, XFS_ALLOC_REC_SIZE);
}

/* By block: the start alone orders. */
static int bno_cmp(const uint8_t *a, const uint8_t *b) {
    return cmp_u32(xfs_be32(a + XFS_ALLOC_REC_STARTBLOCK), xfs_be32(b + XFS_ALLOC_REC_STARTBLOCK));
}

/* By length: the length, then the start. */
static int cnt_cmp(const uint8_t *a, const uint8_t *b) {
    int d = cmp_u32(xfs_be32(a + XFS_ALLOC_REC_BLOCKCOUNT), xfs_be32(b + XFS_ALLOC_REC_BLOCKCOUNT));
    return d != 0 ? d : bno_cmp(a, b);
}

static int tree_alloc(struct xfs_btree_cur *cur, uint32_t *agbno) {
    return xfs_agfl_pop(cur->fs, cur->agno, XFS_AGFL_FREESPACE, agbno);
}

static int tree_free(struct xfs_btree_cur *cur, uint32_t agbno) {
    return xfs_agfl_push(cur->fs, cur->agno, XFS_AGFL_FREESPACE, agbno);
}

static const struct xfs_btree_ops bno_ops = {
    .rec_size = XFS_ALLOC_REC_SIZE, .key_size = XFS_ALLOC_REC_SIZE, .overlapping = false,
    .root_off = XFS_AGF_ROOTS, .level_off = XFS_AGF_LEVELS,
    .low_key = alloc_key, .high_key = alloc_key, .cmp = bno_cmp,
    .alloc_block = tree_alloc, .free_block = tree_free,
};

static const struct xfs_btree_ops cnt_ops = {
    .rec_size = XFS_ALLOC_REC_SIZE, .key_size = XFS_ALLOC_REC_SIZE, .overlapping = false,
    .root_off = XFS_AGF_ROOTS + 4u, .level_off = XFS_AGF_LEVELS + 4u,
    .low_key = alloc_key, .high_key = alloc_key, .cmp = cnt_cmp,
    .alloc_block = tree_alloc, .free_block = tree_free,
};

/* Opens one of the two trees, with the magic this filesystem's version uses
 * — the trees' magics changed between v4 and v5, their shape did not. */
static int open_tree(struct xfs *fs, uint32_t agno, bool by_count, struct xfs_btree_cur *cur) {
    int rc = xfs_btree_open(cur, fs, by_count ? &cnt_ops : &bno_ops, agno);
    cur->magic = xfs_magic_for(fs, by_count ? XFS_BLK_CNTBT : XFS_BLK_BNOBT);
    return rc;
}

/* Moves the cursor to the last record at or below `e` in the tree's order. */
static int seek(struct xfs_btree_cur *cur, const struct ext *e) {
    uint8_t rec[XFS_ALLOC_REC_SIZE], key[8];
    rec_pack(rec, e);
    cur->ops->low_key(rec, key);
    return xfs_btree_lookup_le(cur, key);
}

static int here(struct xfs_btree_cur *cur, struct ext *e) {
    uint8_t rec[XFS_ALLOC_REC_SIZE];
    int rc = xfs_btree_get(cur, rec);
    if (rc == KERR_OK) {
        rec_unpack(rec, e);
    }
    return rc;
}

static int after(struct xfs_btree_cur *cur, struct ext *e) {
    uint8_t rec[XFS_ALLOC_REC_SIZE];
    int rc = xfs_btree_peek_next(cur, rec);
    if (rc == KERR_OK) {
        rec_unpack(rec, e);
    }
    return rc;
}

/* Puts `e` into one tree, or takes exactly `e` out of it. */
static int tree_insert(struct xfs *fs, uint32_t agno, bool by_count, const struct ext *e) {
    struct xfs_btree_cur cur;
    uint8_t rec[XFS_ALLOC_REC_SIZE];
    int rc = open_tree(fs, agno, by_count, &cur);
    rc = rc != KERR_OK ? rc : seek(&cur, e);
    rec_pack(rec, e);
    return rc != KERR_OK ? rc : xfs_btree_insert(&cur, rec);
}

static int tree_delete(struct xfs *fs, uint32_t agno, bool by_count, const struct ext *e) {
    struct xfs_btree_cur cur;
    struct ext found = {0, 0};
    int rc = open_tree(fs, agno, by_count, &cur);
    rc = rc != KERR_OK ? rc : seek(&cur, e);
    rc = rc != KERR_OK ? rc : here(&cur, &found);
    if (rc != KERR_OK || found.start != e->start || found.count != e->count) {
        kprintf("xfs: group %u's free-space trees disagree at block %u\n", agno, e->start);
        return KERR_IO;
    }
    return xfs_btree_delete(&cur);
}

/* The same change to both trees. */
static int both_insert(struct xfs *fs, uint32_t agno, const struct ext *e) {
    int rc = tree_insert(fs, agno, false, e);
    return rc != KERR_OK ? rc : tree_insert(fs, agno, true, e);
}

static int both_delete(struct xfs *fs, uint32_t agno, const struct ext *e) {
    int rc = tree_delete(fs, agno, false, e);
    return rc != KERR_OK ? rc : tree_delete(fs, agno, true, e);
}

/* ── the group header ─────────────────────────────────────────────────── */

int xfs_agf_get(struct xfs *fs, uint32_t agno, struct bbuf **out, uint8_t **agf) {
    uint8_t *at;
    int rc = xfs_group_header(fs, agno, XFS_AGF_SECTOR, out, &at);
    if (rc != KERR_OK) {
        return rc;
    }
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

/* The largest free extent: the last record of the by-length tree. Found
 * rather than adjusted, because adjusting it is how it drifts. */
static int longest(struct xfs *fs, uint32_t agno, uint32_t *out) {
    struct xfs_btree_cur cur;
    struct ext top = {0xffffffffu, 0xffffffffu}, found = {0, 0};
    int rc = open_tree(fs, agno, true, &cur);
    rc = rc != KERR_OK ? rc : seek(&cur, &top);
    rc = rc != KERR_OK ? rc : here(&cur, &found);
    *out = found.count;
    return rc == KERR_NOENT ? KERR_OK : rc;
}

/* Moves the group's free count by `delta` and restates its longest run. */
static int agf_after(struct xfs *fs, uint32_t agno, int32_t delta) {
    struct bbuf *b;
    uint8_t *agf;
    uint32_t most = 0;
    int rc = longest(fs, agno, &most);
    rc = rc != KERR_OK ? rc : xfs_agf_get(fs, agno, &b, &agf);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_put_be32(agf + XFS_AGF_FREEBLKS, xfs_be32(agf + XFS_AGF_FREEBLKS) + (uint32_t)delta);
    xfs_put_be32(agf + XFS_AGF_LONGEST, most);
    xfs_reseal(fs, agf, fs->sectsize, XFS_AGF_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

int xfs_count_blocks_free(struct xfs *fs, int64_t delta) {
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

/* ── finding room ─────────────────────────────────────────────────────── */

/* The free extent holding block `at`, if one does. */
static int covering(struct xfs *fs, uint32_t agno, uint32_t at, struct ext *e) {
    struct xfs_btree_cur cur;
    struct ext key = {at, 0};
    int rc = open_tree(fs, agno, false, &cur);
    rc = rc != KERR_OK ? rc : seek(&cur, &key);
    rc = rc != KERR_OK ? rc : here(&cur, e);
    return rc == KERR_OK && at - e->start >= e->count ? KERR_NOENT : rc;
}

/* Where a run of `count` starting at a multiple of `align` fits in `e`, or
 * false. An alignment of one asks only for the length. */
static bool fits(const struct ext *e, uint32_t count, uint32_t align, uint32_t *at) {
    uint64_t a = ((uint64_t)e->start + align - 1u) / align * align;
    *at = (uint32_t)a;
    return a + count <= (uint64_t)e->start + e->count;
}

/* The smallest free extent that can hold the run — the **best fit**, which
 * is what XFS does and what leaves large extents whole for whoever needs
 * one — walking up the by-length tree from the first that is long enough
 * until an alignment, when there is one, can be met. */
static int best_fit(struct xfs *fs, uint32_t agno, uint32_t count, uint32_t align,
                    struct ext *e, uint32_t *at) {
    struct xfs_btree_cur cur;
    struct ext key = {0, count};
    int rc = open_tree(fs, agno, true, &cur);
    rc = rc != KERR_OK ? rc : seek(&cur, &key);
    rc = rc != KERR_OK ? rc : here(&cur, e);
    if (rc == KERR_NOENT || (rc == KERR_OK && !fits(e, count, align, at))) {
        for (rc = after(&cur, e); rc == KERR_OK && !fits(e, count, align, at);) {
            rc = seek(&cur, e);
            rc = rc != KERR_OK ? rc : after(&cur, e);
        }
    }
    return rc == KERR_NOENT ? KERR_NOSPC : rc;
}

/* Cuts [at, at+count) out of the free extent `e`, leaving whatever is free
 * either side of it as extents of their own. */
static int take(struct xfs *fs, uint32_t agno, const struct ext *e, uint32_t at,
                uint32_t count) {
    struct ext head = {e->start, at - e->start};
    struct ext tail = {at + count, e->start + e->count - (at + count)};
    int rc = both_delete(fs, agno, e);
    rc = rc != KERR_OK || head.count == 0 ? rc : both_insert(fs, agno, &head);
    rc = rc != KERR_OK || tail.count == 0 ? rc : both_insert(fs, agno, &tail);
    return rc != KERR_OK ? rc : agf_after(fs, agno, -(int32_t)count);
}

/* An allocation in one group with nothing asked of the free list first —
 * the path the free list's own refill takes (xfs_agfl.c). `want` is a block
 * to start at if it is free, and `align` what a start must be a multiple of. */
static int alloc_here(struct xfs *fs, uint32_t agno, uint32_t count, uint32_t want,
                      uint32_t align, uint32_t *got) {
    struct ext e = {0, 0};
    uint32_t at = 0;
    int rc = KERR_NOENT;
    if (count == 0) {
        return KERR_INVAL;
    }
    if (align <= 1u && want != 0) {
        rc = covering(fs, agno, want, &e);
        at = want;
        rc = rc == KERR_OK && want + count > e.start + e.count ? KERR_NOENT : rc;
    }
    if (rc == KERR_NOENT) {
        rc = best_fit(fs, agno, count, align, &e, &at);
    }
    rc = rc != KERR_OK ? rc : take(fs, agno, &e, at, count);
    *got = at;
    return rc;
}

int xfs_alloc_raw(struct xfs *fs, uint32_t agno, uint32_t count, uint32_t *got) {
    return alloc_here(fs, agno, count, 0, 1u, got);
}

/* An allocation with the free list made ready first, and the superblock's
 * free count moved by what was taken. */
static int alloc_in(struct xfs *fs, uint32_t agno, uint32_t count, uint32_t want,
                    uint32_t align, uint32_t *got) {
    int rc = xfs_agfl_ensure(fs, agno);
    rc = rc != KERR_OK ? rc : alloc_here(fs, agno, count, want, align, got);
    return rc != KERR_OK ? rc : xfs_count_blocks_free(fs, -(int64_t)count);
}

int xfs_alloc_extent(struct xfs *fs, uint32_t agno, uint32_t count, uint32_t want,
                     uint32_t *got) {
    return alloc_in(fs, agno, count, want, 1u, got);
}

int xfs_alloc_block(struct xfs *fs, uint32_t agno, uint32_t want, uint32_t *got) {
    return xfs_alloc_extent(fs, agno, 1u, want, got);
}

/* Asks the preferred group, then every other one. */
static int alloc_search(struct xfs *fs, uint32_t *agno, uint32_t count, uint32_t want,
                        uint32_t align, uint32_t *got) {
    uint32_t first = *agno < fs->agcount ? *agno : 0;
    int rc = alloc_in(fs, first, count, want, align, got);
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
        rc = alloc_in(fs, i, count, 0, align, got);
        if (rc != KERR_NOSPC) {
            *agno = i;
            fs->alloc_other_group += rc == KERR_OK ? 1u : 0u;
            return rc;
        }
    }
    return KERR_NOSPC;
}

int xfs_alloc_somewhere(struct xfs *fs, uint32_t *agno, uint32_t count, uint32_t want,
                        uint32_t *got) {
    return alloc_search(fs, agno, count, want, 1u, got);
}

int xfs_alloc_chunk_somewhere(struct xfs *fs, uint32_t *agno, uint32_t count, uint32_t *got) {
    return alloc_search(fs, agno, count, 0, fs->inoalign, got);
}

/* ── giving back ──────────────────────────────────────────────────────── */

static bool overlaps(const struct ext *a, uint32_t start, uint32_t count) {
    return a->start < start + count && start < a->start + a->count;
}

/* The free extents either side of [start, start+count), each flagged absent;
 * KERR_IO when either already covers part of it — a block freed twice. */
static int neighbours(struct xfs *fs, uint32_t agno, uint32_t start, uint32_t count,
                      struct ext *left, bool *has_left, struct ext *right, bool *has_right) {
    struct xfs_btree_cur cur;
    struct ext key = {start, 0};
    int rc = open_tree(fs, agno, false, &cur);
    rc = rc != KERR_OK ? rc : seek(&cur, &key);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = here(&cur, left);
    *has_left = rc == KERR_OK;
    rc = rc == KERR_OK || rc == KERR_NOENT ? after(&cur, right) : rc;
    *has_right = rc == KERR_OK;
    if ((*has_left && overlaps(left, start, count)) ||
        (*has_right && overlaps(right, start, count))) {
        kprintf("xfs: group %u block %u is being freed but is free already\n", agno, start);
        return KERR_IO;
    }
    return rc == KERR_NOENT ? KERR_OK : rc;
}

/* Puts an extent back, joined to whatever it touches: free space is kept as
 * maximal extents, so an extent that abuts the one before or after it
 * becomes part of that record rather than a new one. */
static int give_back(struct xfs *fs, uint32_t agno, uint32_t start, uint32_t count) {
    struct ext left, right, joined = {start, count};
    bool has_left = false, has_right = false;
    int rc = neighbours(fs, agno, start, count, &left, &has_left, &right, &has_right);
    if (rc == KERR_OK && has_left && left.start + left.count == start) {
        rc = both_delete(fs, agno, &left);
        joined.start = left.start;
        joined.count += left.count;
    }
    if (rc == KERR_OK && has_right && start + count == right.start) {
        rc = both_delete(fs, agno, &right);
        joined.count += right.count;
    }
    rc = rc != KERR_OK ? rc : both_insert(fs, agno, &joined);
    return rc != KERR_OK ? rc : agf_after(fs, agno, (int32_t)count);
}

int xfs_free_owned(struct xfs *fs, uint64_t fsb, uint32_t count, uint64_t owner,
                   uint64_t offset) {
    uint64_t mask = ((uint64_t)1 << fs->agblklog) - 1u;
    int rc = xfs_rmap_unmap(fs, fsb, count, owner, offset);
    return rc != KERR_OK ? rc
                         : xfs_free_extent(fs, xfs_fsb_ag(fs, fsb), (uint32_t)(fsb & mask), count);
}

int xfs_free_extent(struct xfs *fs, uint32_t agno, uint32_t start, uint32_t count) {
    int rc = count == 0 ? KERR_INVAL : xfs_agfl_ensure(fs, agno);
    rc = rc != KERR_OK ? rc : give_back(fs, agno, start, count);
    return rc != KERR_OK ? rc : xfs_count_blocks_free(fs, (int64_t)count);
}

/* The extent after `e` in block order, or the first when `e` is NULL. */
static int free_after(struct xfs_btree_cur *cur, const struct ext *e, struct ext *out) {
    const struct ext origin = {0, 0};
    int rc = seek(cur, e != NULL ? e : &origin);
    if (e == NULL) {
        rc = rc != KERR_OK ? rc : here(cur, out);
        if (rc == KERR_OK) {
            return KERR_OK; /* a free extent at block 0 */
        }
    }
    return rc != KERR_OK && rc != KERR_NOENT ? rc : after(cur, out);
}

int xfs_free_extents_each(struct xfs *fs, uint32_t agno, xfs_free_visit_fn visit, void *self) {
    struct xfs_btree_cur cur;
    struct ext e;
    int rc = open_tree(fs, agno, false, &cur);
    rc = rc != KERR_OK ? rc : free_after(&cur, NULL, &e);
    while (rc == KERR_OK) {
        rc = visit(self, agno, e.start, e.count);
        rc = rc != KERR_OK ? rc : free_after(&cur, &e, &e);
    }
    return rc == KERR_NOENT ? KERR_OK : rc;
}
