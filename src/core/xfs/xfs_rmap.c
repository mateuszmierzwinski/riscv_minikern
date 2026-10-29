/* The reverse map: for every block in use, which inode or structure owns it
 * (`rmapbt`, on by default in `mkfs.xfs`).
 *
 * Reading never needs it, which is why the reader ignored it. Writing must
 * keep it exact: `xfs_repair` rebuilds the map from every inode's forks and
 * every header and tree, then requires each mapping it found to lie inside
 * a record on disk with the same owner, offset and flags. So every block
 * the writer gives an inode is mapped here, every block it frees is
 * unmapped, and adjacent records are merged by **the same rule** both
 * Linux and the checker use — a mapping the checker merged that sits in two
 * records on disk is reported as missing.
 *
 * The tree's own blocks come from the group's free list (xfs_agfl.c),
 * which is made deep enough before each operation. Blocks moving between
 * the list and the tree change owner in no way the map records: both are
 * the group's (`OWN_AG`). */
#include "core/bcache.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs_btree.h"

#define REC_START  0u
#define REC_COUNT  4u
#define REC_OWNER  8u
#define REC_OFFSET 16u
#define KEY_START  0u
#define KEY_OWNER  4u
#define KEY_OFFSET 12u
#define FLAGS (XFS_RMAP_OFF_ATTR | XFS_RMAP_OFF_BMBT | XFS_RMAP_OFF_UNWRITTEN)

struct rmap {
    uint32_t start;
    uint32_t count;
    uint64_t owner;
    uint64_t offset; /* the logical block, with the flag bits above it */
};

static bool owned_by_structure(uint64_t owner) {
    return owner >= XFS_RMAP_OWN_MIN;
}

/* ── the format ───────────────────────────────────────────────────────── */

static void rec_pack(uint8_t *p, const struct rmap *r) {
    xfs_put_be32(p + REC_START, r->start);
    xfs_put_be32(p + REC_COUNT, r->count);
    xfs_put_be64(p + REC_OWNER, r->owner);
    xfs_put_be64(p + REC_OFFSET, r->offset);
}

static void rec_unpack(const uint8_t *p, struct rmap *r) {
    r->start = xfs_be32(p + REC_START);
    r->count = xfs_be32(p + REC_COUNT);
    r->owner = xfs_be64(p + REC_OWNER);
    r->offset = xfs_be64(p + REC_OFFSET);
}

static void key_pack(uint8_t *k, uint32_t start, uint64_t owner, uint64_t offset) {
    xfs_put_be32(k + KEY_START, start);
    xfs_put_be64(k + KEY_OWNER, owner);
    xfs_put_be64(k + KEY_OFFSET, offset & ~XFS_RMAP_OFF_UNWRITTEN);
}

static void low_key(const uint8_t *rec, uint8_t *key) {
    struct rmap r;
    rec_unpack(rec, &r);
    key_pack(key, r.start, r.owner, r.offset);
}

/* The last block a record covers, and for an inode's data the last logical
 * block too; a structure's offset, and a b-tree block's, do not advance. */
static void high_key(const uint8_t *rec, uint8_t *key) {
    struct rmap r;
    uint32_t adj;
    rec_unpack(rec, &r);
    adj = r.count - 1u;
    if (!owned_by_structure(r.owner) && (r.offset & XFS_RMAP_OFF_BMBT) == 0) {
        r.offset = ((r.offset & XFS_RMAP_OFF_MASK) + adj) | (r.offset & ~XFS_RMAP_OFF_MASK);
    }
    key_pack(key, r.start + adj, r.owner, r.offset);
}

static int cmp_u64(uint64_t a, uint64_t b) {
    return a < b ? -1 : (a > b ? 1 : 0);
}

static int key_cmp(const uint8_t *a, const uint8_t *b) {
    int d = cmp_u64(xfs_be32(a + KEY_START), xfs_be32(b + KEY_START));
    d = d != 0 ? d : cmp_u64(xfs_be64(a + KEY_OWNER), xfs_be64(b + KEY_OWNER));
    return d != 0 ? d : cmp_u64(xfs_be64(a + KEY_OFFSET), xfs_be64(b + KEY_OFFSET));
}

/* ── the tree's supply: the group's free list (xfs_agfl.c) ─────────────── */

static int tree_alloc(struct xfs_btree_cur *cur, uint32_t *agbno) {
    return xfs_agfl_pop(cur->fs, cur->agno, XFS_AGFL_RMAP, agbno);
}

static int tree_free(struct xfs_btree_cur *cur, uint32_t agbno) {
    return xfs_agfl_push(cur->fs, cur->agno, XFS_AGFL_RMAP, agbno);
}

static const struct xfs_btree_ops rmap_ops = {
    .magic = XFS_RMAP_MAGIC,
    .rec_size = XFS_RMAP_REC_SIZE,
    .key_size = XFS_RMAP_KEY_SIZE,
    .overlapping = true,
    .root_off = XFS_AGF_ROOTS + 8u,
    .level_off = XFS_AGF_LEVELS + 8u,
    .low_key = low_key,
    .high_key = high_key,
    .cmp = key_cmp,
    .alloc_block = tree_alloc,
    .free_block = tree_free,
};

/* ── the tree, by record ──────────────────────────────────────────────── */

static int find(struct xfs_btree_cur *cur, const struct rmap *r) {
    uint8_t key[XFS_RMAP_KEY_SIZE];
    key_pack(key, r->start, r->owner, r->offset);
    return xfs_btree_lookup_le(cur, key);
}

/* The record at the cursor, or KERR_NOENT. */
static int here(struct xfs_btree_cur *cur, struct rmap *r) {
    uint8_t rec[XFS_RMAP_REC_SIZE];
    int rc = xfs_btree_get(cur, rec);
    if (rc == KERR_OK) {
        rec_unpack(rec, r);
    }
    return rc;
}

static int next(struct xfs_btree_cur *cur, struct rmap *r) {
    uint8_t rec[XFS_RMAP_REC_SIZE];
    int rc = xfs_btree_peek_next(cur, rec);
    if (rc == KERR_OK) {
        rec_unpack(rec, r);
    }
    return rc;
}

static int insert(struct xfs_btree_cur *cur, const struct rmap *r) {
    uint8_t rec[XFS_RMAP_REC_SIZE];
    int rc = find(cur, r);
    rec_pack(rec, r);
    return rc != KERR_OK ? rc : xfs_btree_insert(cur, rec);
}

static bool same_record(const struct rmap *a, const struct rmap *b) {
    return a->start == b->start && a->owner == b->owner &&
           (a->offset & ~XFS_RMAP_OFF_UNWRITTEN) == (b->offset & ~XFS_RMAP_OFF_UNWRITTEN);
}

/* Finds the record `old` names, which must be there exactly. */
static int find_exact(struct xfs_btree_cur *cur, const struct rmap *old) {
    struct rmap r;
    int rc = find(cur, old);
    rc = rc != KERR_OK ? rc : here(cur, &r);
    return rc != KERR_OK ? rc : (same_record(&r, old) ? KERR_OK : KERR_IO);
}

static int delete(struct xfs_btree_cur *cur, const struct rmap *old) {
    int rc = find_exact(cur, old);
    return rc != KERR_OK ? rc : xfs_btree_delete(cur);
}

/* Rewrites `old` as `now`, which starts where it does. */
static int rewrite(struct xfs_btree_cur *cur, const struct rmap *old, const struct rmap *now) {
    uint8_t rec[XFS_RMAP_REC_SIZE];
    int rc = find_exact(cur, old);
    rec_pack(rec, now);
    return rc != KERR_OK ? rc : xfs_btree_update(cur, rec);
}

/* ── merging ──────────────────────────────────────────────────────────── */

/* Whether `b` continues `a`: the same owner, the next block, and — for an
 * inode's data — the same flags and the next logical block. The checker's
 * rule, `rmaps_are_mergeable`, exactly. */
static bool continues(const struct rmap *a, const struct rmap *b) {
    if (a->owner != b->owner || a->start + a->count != b->start ||
        (uint64_t)a->count + b->count > 0xffffffffu) {
        return false;
    }
    if (owned_by_structure(a->owner)) {
        return true;
    }
    if ((a->offset & FLAGS) != (b->offset & FLAGS)) {
        return false;
    }
    return (a->offset & XFS_RMAP_OFF_BMBT) != 0 ||
           (a->offset & XFS_RMAP_OFF_MASK) + a->count == (b->offset & XFS_RMAP_OFF_MASK);
}

static bool overlaps(const struct rmap *a, const struct rmap *b) {
    return a->start < b->start + b->count && b->start < a->start + a->count;
}

/* The records either side of where `r` goes, each flagged absent. */
static int neighbours(struct xfs_btree_cur *cur, const struct rmap *r, struct rmap *left,
                      bool *has_left, struct rmap *right, bool *has_right) {
    int rc = find(cur, r);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = here(cur, left);
    *has_left = rc == KERR_OK;
    if (rc != KERR_OK && rc != KERR_NOENT) {
        return rc;
    }
    rc = next(cur, right);
    *has_right = rc == KERR_OK;
    return rc == KERR_NOENT ? KERR_OK : rc;
}

/* Adds `r`, joined to whichever neighbours it continues. A block may be
 * mapped twice only when it is `shared` — a reflink — and then to another
 * owner; otherwise a second mapping is a bug this refuses to record. */
static int map_record(struct xfs_btree_cur *cur, const struct rmap *r, bool shared) {
    struct rmap left, right, joined;
    bool has_left, has_right, to_left, to_right;
    int rc = neighbours(cur, r, &left, &has_left, &right, &has_right);
    if (rc != KERR_OK) {
        return rc;
    }
    if (!shared && ((has_left && overlaps(&left, r)) || (has_right && overlaps(&right, r)))) {
        kprintf("xfs: group %u block %u is mapped already\n", cur->agno, r->start);
        return KERR_IO; /* this writer never shares a block */
    }
    to_left = has_left && continues(&left, r);
    to_right = has_right && continues(r, &right);
    joined = to_left ? left : *r;
    joined.count = (to_left ? left.count : 0u) + r->count + (to_right ? right.count : 0u);
    rc = to_right ? delete(cur, &right) : KERR_OK;
    if (rc != KERR_OK) {
        return rc;
    }
    return to_left ? rewrite(cur, &left, &joined) : insert(cur, &joined);
}

/* Whether `r` names blocks inside `whole`, owned and placed as it says. */
static bool covers(const struct rmap *whole, const struct rmap *r) {
    if (whole->owner != r->owner || whole->start > r->start ||
        r->start + r->count > whole->start + whole->count ||
        (whole->offset & FLAGS) != (r->offset & FLAGS)) {
        return false;
    }
    return owned_by_structure(r->owner) || (r->offset & XFS_RMAP_OFF_BMBT) != 0 ||
           (whole->offset & XFS_RMAP_OFF_MASK) + (r->start - whole->start) ==
               (r->offset & XFS_RMAP_OFF_MASK);
}

/* `whole` from block `from` on, with its logical offset moved to match. */
static struct rmap tail_of(const struct rmap *whole, uint32_t from) {
    struct rmap t = *whole;
    t.start = from;
    t.count = whole->start + whole->count - from;
    if (!owned_by_structure(t.owner) && (t.offset & XFS_RMAP_OFF_BMBT) == 0) {
        t.offset += from - whole->start;
    }
    return t;
}

/* How far back past other owners' records the search for one owner's goes.
 * Only a shared block has them, and each sharer adds one. */
#define SHARED_LOOKBACK 256u

/* The record holding `r`: the last one at or below its key, or — where
 * blocks are shared and other owners' records sort between — one of those
 * before it. */
static int holder(struct xfs_btree_cur *cur, const struct rmap *r, struct rmap *whole) {
    uint8_t rec[XFS_RMAP_REC_SIZE];
    uint32_t leaf;
    int32_t idx;
    int rc = find(cur, r);
    rc = rc != KERR_OK ? rc : here(cur, whole);
    leaf = cur->bno[0];
    idx = cur->idx[0];
    for (uint32_t back = 0; rc == KERR_OK && !covers(whole, r) && back < SHARED_LOOKBACK; back++) {
        rc = xfs_btree_get_prev(cur, &leaf, &idx, rec);
        if (rc == KERR_OK) {
            rec_unpack(rec, whole);
        }
    }
    return rc;
}

/* Takes `r` out of the record that holds it: all of it, its head, its tail
 * or its middle, the last leaving a record either side. */
static int unmap_record(struct xfs_btree_cur *cur, const struct rmap *r) {
    struct rmap whole, head;
    uint32_t end = r->start + r->count;
    int rc = holder(cur, r, &whole);
    if (rc != KERR_OK || !covers(&whole, r)) {
        kprintf("xfs: group %u block %u is not mapped to owner %llx\n", cur->agno, r->start,
                (unsigned long long)r->owner);
        return rc != KERR_OK && rc != KERR_NOENT ? rc : KERR_IO;
    }
    head = whole;
    head.count = r->start - whole.start;
    rc = head.count > 0 ? rewrite(cur, &whole, &head) : delete(cur, &whole);
    if (rc != KERR_OK || end == whole.start + whole.count) {
        return rc;
    }
    head = tail_of(&whole, end);
    return insert(cur, &head);
}

/* ── what the rest of the filesystem calls ────────────────────────────── */

/* Splits a filesystem block into its group and the record for it, with the
 * group's free list made deep enough first — before the tree is opened,
 * because topping the list up maps blocks and can change the tree's height. */
static int open_for(struct xfs *fs, uint64_t fsb, uint32_t count, uint64_t owner,
                    uint64_t offset, struct xfs_btree_cur *cur, struct rmap *r) {
    uint64_t mask = ((uint64_t)1 << fs->agblklog) - 1u;
    int rc = xfs_agfl_ensure(fs, xfs_fsb_ag(fs, fsb));
    *r = (struct rmap){.start = (uint32_t)(fsb & mask), .count = count, .owner = owner,
                       .offset = offset};
    return rc != KERR_OK ? rc : xfs_btree_open(cur, fs, &rmap_ops, xfs_fsb_ag(fs, fsb));
}

/* Maps a range, shared with other owners or not. */
static int map_range(struct xfs *fs, uint64_t fsb, uint32_t count, uint64_t owner,
                     uint64_t offset, bool shared) {
    struct xfs_btree_cur cur;
    struct rmap r;
    int rc;
    if (!fs->has_rmapbt || count == 0) {
        return KERR_OK;
    }
    rc = open_for(fs, fsb, count, owner, offset, &cur, &r);
    return rc != KERR_OK ? rc : map_record(&cur, &r, shared);
}

int xfs_rmap_map(struct xfs *fs, uint64_t fsb, uint32_t count, uint64_t owner, uint64_t offset) {
    return map_range(fs, fsb, count, owner, offset, false);
}

int xfs_rmap_map_shared(struct xfs *fs, uint64_t fsb, uint32_t count, uint64_t owner,
                        uint64_t offset) {
    return map_range(fs, fsb, count, owner, offset, true);
}

int xfs_rmap_unmap(struct xfs *fs, uint64_t fsb, uint32_t count, uint64_t owner,
                   uint64_t offset) {
    struct xfs_btree_cur cur;
    struct rmap r;
    int rc;
    if (!fs->has_rmapbt || count == 0) {
        return KERR_OK;
    }
    rc = open_for(fs, fsb, count, owner, offset, &cur, &r);
    return rc != KERR_OK ? rc : unmap_record(&cur, &r);
}
