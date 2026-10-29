/* The reference-count tree (`refcountbt`, with reflink): how many files
 * share each block that more than one file maps.
 *
 * **A block with no record is held once.** Records exist only for ranges
 * shared two or more times, so raising a range's count makes records where
 * there were none, and lowering a count of two removes the record — the
 * block is then one file's again. Lowering a range that has no record takes
 * its count from one to none, and those blocks are given back: that is a
 * free of an unshared extent, the same as without reflink.
 *
 * Records are **maximal**: a run of blocks with one count is one record,
 * as `xfs_repair` computes them from the reverse map and then compares
 * exactly. So every insert is joined to neighbours it touches with the same
 * count, and a change to the middle of a record splits it into the pieces
 * either side. Only shared records are written here (the "CoW staging"
 * domain Linux uses for writes in flight is left alone).
 *
 * The tree's blocks come from the ordinary allocator, in the group, and
 * are the tree's in the reverse map (`OWN_REFC`); the AGF counts them. */
#include "core/bcache.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs_btree.h"

#define REFC_MAGIC 0x52334643u /* "R3FC" */
#define REFC_REC   12u         /* start u32, count u32, refcount u32 */
#define REFC_COW   0x80000000u /* in the start: a CoW staging extent, not shared */

struct refc {
    uint32_t start;
    uint32_t count;
    uint32_t refs;
};

static void pack(uint8_t *p, const struct refc *r) {
    xfs_put_be32(p, r->start);
    xfs_put_be32(p + 4u, r->count);
    xfs_put_be32(p + 8u, r->refs);
}

static void unpack(const uint8_t *p, struct refc *r) {
    r->start = xfs_be32(p);
    r->count = xfs_be32(p + 4u);
    r->refs = xfs_be32(p + 8u);
}

static void refc_key(const uint8_t *rec, uint8_t *key) {
    xfs_copy_bytes(key, rec, 4u);
}

static int refc_cmp(const uint8_t *a, const uint8_t *b) {
    uint32_t x = xfs_be32(a), y = xfs_be32(b);
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int count_tree_block(struct xfs *fs, uint32_t agno, int32_t delta) {
    struct bbuf *b;
    uint8_t *agf;
    int rc = xfs_agf_get(fs, agno, &b, &agf);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_put_be32(agf + XFS_AGF_REFC_BLOCKS, xfs_be32(agf + XFS_AGF_REFC_BLOCKS) + (uint32_t)delta);
    xfs_reseal(fs, agf, fs->sectsize, XFS_AGF_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

static int tree_alloc(struct xfs_btree_cur *cur, uint32_t *agbno) {
    int rc = xfs_alloc_extent(cur->fs, cur->agno, 1u, 0, agbno);
    rc = rc != KERR_OK ? rc
                       : xfs_rmap_map(cur->fs, ((uint64_t)cur->agno << cur->fs->agblklog) | *agbno,
                                      1u, XFS_RMAP_OWN_REFC, 0);
    return rc != KERR_OK ? rc : count_tree_block(cur->fs, cur->agno, 1);
}

static int tree_free(struct xfs_btree_cur *cur, uint32_t agbno) {
    int rc = xfs_free_owned(cur->fs, ((uint64_t)cur->agno << cur->fs->agblklog) | agbno, 1u,
                            XFS_RMAP_OWN_REFC, 0);
    return rc != KERR_OK ? rc : count_tree_block(cur->fs, cur->agno, -1);
}

static const struct xfs_btree_ops refc_ops = {
    .magic = REFC_MAGIC, .rec_size = REFC_REC, .key_size = 4u,
    .root_off = XFS_AGF_REFC_ROOT, .level_off = XFS_AGF_REFC_LEVEL,
    .low_key = refc_key, .high_key = refc_key, .cmp = refc_cmp,
    .alloc_block = tree_alloc, .free_block = tree_free,
};

/* ── the tree, by record ──────────────────────────────────────────────── */

static int seek(struct xfs_btree_cur *cur, struct xfs *fs, uint32_t agno, uint32_t at) {
    uint8_t key[4];
    int rc = xfs_btree_open(cur, fs, &refc_ops, agno);
    xfs_put_be32(key, at);
    return rc != KERR_OK ? rc : xfs_btree_lookup_le(cur, key);
}

static int here(struct xfs_btree_cur *cur, struct refc *r) {
    uint8_t rec[REFC_REC];
    int rc = xfs_btree_get(cur, rec);
    if (rc == KERR_OK) {
        unpack(rec, r);
    }
    return rc;
}

static int after(struct xfs_btree_cur *cur, struct refc *r) {
    uint8_t rec[REFC_REC];
    int rc = xfs_btree_peek_next(cur, rec);
    if (rc == KERR_OK) {
        unpack(rec, r);
    }
    return rc;
}

static int delete(struct xfs *fs, uint32_t agno, const struct refc *r) {
    struct xfs_btree_cur cur;
    struct refc found;
    int rc = seek(&cur, fs, agno, r->start);
    rc = rc != KERR_OK ? rc : here(&cur, &found);
    if (rc != KERR_OK || found.start != r->start || found.count != r->count) {
        return rc != KERR_OK && rc != KERR_NOENT ? rc : KERR_IO;
    }
    return xfs_btree_delete(&cur);
}

/* The shared record holding block `at`, or the next one after it: one of
 * *has_cover and *has_next says which, and neither for none. */
static int around(struct xfs *fs, uint32_t agno, uint32_t at, struct refc *r, bool *has_cover,
                  bool *has_next) {
    struct xfs_btree_cur cur;
    int rc = seek(&cur, fs, agno, at);
    *has_cover = false;
    *has_next = false;
    rc = rc != KERR_OK ? rc : here(&cur, r);
    if (rc == KERR_OK && (r->start & REFC_COW) == 0 && at - r->start < r->count) {
        *has_cover = true;
        return KERR_OK;
    }
    if (rc != KERR_OK && rc != KERR_NOENT) {
        return rc;
    }
    rc = after(&cur, r);
    *has_next = rc == KERR_OK && (r->start & REFC_COW) == 0;
    return rc == KERR_NOENT ? KERR_OK : rc;
}

/* Inserts `r`, joined to the records either side that touch it with the
 * same count: records are maximal, as `xfs_repair` compares them. */
static int insert(struct xfs *fs, uint32_t agno, struct refc r) {
    struct xfs_btree_cur cur;
    struct refc left, right;
    bool cover = false, next = false;
    uint8_t rec[REFC_REC];
    int rc = r.start > 0 ? around(fs, agno, r.start - 1u, &left, &cover, &next) : KERR_OK;
    if (rc == KERR_OK && cover && left.refs == r.refs && left.start + left.count == r.start) {
        rc = delete(fs, agno, &left);
        r.count += left.count;
        r.start = left.start;
    }
    rc = rc != KERR_OK ? rc : around(fs, agno, r.start + r.count, &right, &cover, &next);
    if (rc == KERR_OK && cover && right.refs == r.refs && right.start == r.start + r.count) {
        rc = delete(fs, agno, &right);
        r.count += right.count;
    }
    rc = rc != KERR_OK ? rc : seek(&cur, fs, agno, r.start);
    pack(rec, &r);
    return rc != KERR_OK ? rc : xfs_btree_insert(&cur, rec);
}

/* ── changing counts ──────────────────────────────────────────────────── */

/* Moves the count of [at, end) — inside the shared record `r` — by
 * `delta`, splitting `r` around it; a count that falls to one leaves no
 * record. */
static int adjust_shared(struct xfs *fs, uint32_t agno, const struct refc *r, uint32_t at,
                         uint32_t end, int32_t delta) {
    struct refc head = {r->start, at - r->start, r->refs};
    struct refc mid = {at, end - at, (uint32_t)((int32_t)r->refs + delta)};
    struct refc tail = {end, r->start + r->count - end, r->refs};
    int rc = delete(fs, agno, r);
    rc = rc != KERR_OK || head.count == 0 ? rc : insert(fs, agno, head);
    rc = rc != KERR_OK || mid.refs < 2u ? rc : insert(fs, agno, mid);
    return rc != KERR_OK || tail.count == 0 ? rc : insert(fs, agno, tail);
}

int xfs_refcount_adjust(struct xfs *fs, uint64_t fsb, uint32_t count, int32_t delta,
                        xfs_refcount_free_fn on_last, void *self) {
    uint64_t mask = ((uint64_t)1 << fs->agblklog) - 1u;
    uint32_t agno = xfs_fsb_ag(fs, fsb), at = (uint32_t)(fsb & mask), end = at + count;
    int rc = xfs_agfl_ensure(fs, agno);
    while (rc == KERR_OK && at < end) {
        struct refc r;
        bool cover = false, next = false;
        uint32_t stop;
        rc = around(fs, agno, at, &r, &cover, &next);
        if (rc != KERR_OK) {
            break;
        }
        if (cover) {
            stop = r.start + r.count < end ? r.start + r.count : end;
            rc = adjust_shared(fs, agno, &r, at, stop, delta);
        } else {
            /* Held once: raising makes a shared record, lowering frees it. */
            stop = next && r.start < end ? r.start : end;
            rc = delta > 0 ? insert(fs, agno, (struct refc){at, stop - at, 2u})
                           : on_last(self, ((uint64_t)agno << fs->agblklog) | at, stop - at);
        }
        at = stop;
    }
    return rc;
}

int xfs_refcount_shared(struct xfs *fs, uint64_t fsb, bool *shared) {
    uint64_t mask = ((uint64_t)1 << fs->agblklog) - 1u;
    struct refc r;
    bool next = false;
    *shared = false;
    return fs->has_reflink ? around(fs, xfs_fsb_ag(fs, fsb), (uint32_t)(fsb & mask), &r, shared,
                                    &next)
                           : KERR_OK;
}
