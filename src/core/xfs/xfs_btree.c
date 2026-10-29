/* The short-pointer B+tree, written (xfs_btree.h says what it keeps true).
 *
 * Blocks are got and put around each step rather than held along the path:
 * the cache keeps them, and a step that holds one block while it gets
 * another never has more than two pinned. An entry is a leaf record or a
 * node's key slot with its pointer, and the two halves of a node — the key
 * array and the pointer array after it — are moved together by the entry
 * helpers, so nothing above them knows a node is two arrays. */
#include "core/bcache.h"
#include "core/errno.h"
#include "core/xfs/xfs_btree.h"

#define SLOT_MAX (2u * XFS_BTREE_KEY_MAX)
#define ENTRY_MAX (SLOT_MAX > XFS_BTREE_REC_MAX ? SLOT_MAX : XFS_BTREE_REC_MAX)

/* One leaf record, or one node key slot and the child it names. */
struct entry {
    uint8_t bytes[ENTRY_MAX];
    uint32_t ptr;
};

/* ── the block's shape ─────────────────────────────────────────────────── */

static uint32_t slot_size(const struct xfs_btree_cur *cur) {
    return cur->ops->key_size * (cur->ops->overlapping ? 2u : 1u);
}

static uint32_t entry_size(const struct xfs_btree_cur *cur, uint32_t level) {
    return level == 0 ? cur->ops->rec_size : slot_size(cur);
}

static uint32_t max_entries(const struct xfs_btree_cur *cur, uint32_t level) {
    uint32_t room = cur->fs->blocksize - cur->fs->btree_hdr;
    return level == 0 ? room / cur->ops->rec_size : room / (slot_size(cur) + 4u);
}

static uint32_t min_entries(const struct xfs_btree_cur *cur, uint32_t level) {
    return max_entries(cur, level) / 2u;
}

static uint32_t count_of(const struct bbuf *b) {
    return xfs_be16(b->data + XFS_BTREE_NUMRECS);
}

static void set_count(struct bbuf *b, uint32_t n) {
    xfs_put_be16(b->data + XFS_BTREE_NUMRECS, (uint16_t)n);
}

static uint8_t *entry_at(const struct xfs_btree_cur *cur, const struct bbuf *b, uint32_t level,
                         uint32_t i) {
    return b->data + cur->fs->btree_hdr + i * entry_size(cur, level);
}

static uint8_t *ptr_at(const struct xfs_btree_cur *cur, const struct bbuf *b, uint32_t i) {
    return b->data + cur->fs->btree_hdr + max_entries(cur, 1u) * slot_size(cur) + i * 4u;
}

static void entry_read(const struct xfs_btree_cur *cur, const struct bbuf *b, uint32_t level,
                       uint32_t i, struct entry *e) {
    xfs_copy_bytes(e->bytes, entry_at(cur, b, level, i), entry_size(cur, level));
    e->ptr = level == 0 ? 0 : xfs_be32(ptr_at(cur, b, i));
}

static void entry_write(const struct xfs_btree_cur *cur, struct bbuf *b, uint32_t level,
                        uint32_t i, const struct entry *e) {
    xfs_copy_bytes(entry_at(cur, b, level, i), e->bytes, entry_size(cur, level));
    if (level > 0) {
        xfs_put_be32(ptr_at(cur, b, i), e->ptr);
    }
}

/* Opens a gap at `pos` and fills it; the caller has checked for room. */
static void entry_insert(const struct xfs_btree_cur *cur, struct bbuf *b, uint32_t level,
                         uint32_t pos, const struct entry *e) {
    struct entry move;
    for (uint32_t i = count_of(b); i > pos; i--) {
        entry_read(cur, b, level, i - 1u, &move);
        entry_write(cur, b, level, i, &move);
    }
    entry_write(cur, b, level, pos, e);
    set_count(b, count_of(b) + 1u);
}

static void entry_remove(const struct xfs_btree_cur *cur, struct bbuf *b, uint32_t level,
                         uint32_t pos) {
    struct entry move;
    for (uint32_t i = pos; i + 1u < count_of(b); i++) {
        entry_read(cur, b, level, i + 1u, &move);
        entry_write(cur, b, level, i, &move);
    }
    set_count(b, count_of(b) - 1u);
}

/* ── blocks in and out of the cache ────────────────────────────────────── */

static uint64_t fsb_of(const struct xfs_btree_cur *cur, uint32_t agbno) {
    return ((uint64_t)cur->agno << cur->fs->agblklog) | agbno;
}

static int blk_get(const struct xfs_btree_cur *cur, uint32_t agbno, struct bbuf **b) {
    if (agbno == XFS_NULL_AGBNO || agbno >= cur->fs->agblocks) {
        return KERR_IO;
    }
    return xfs_meta_block(cur->fs, fsb_of(cur, agbno), cur->magic, 0, XFS_BTREE_CRC,
                          XFS_BTREE_UUID, b);
}

static void blk_done(const struct xfs_btree_cur *cur, struct bbuf *b) {
    xfs_reseal(cur->fs, b->data, cur->fs->blocksize, XFS_BTREE_CRC);
    bcache_dirty(b);
    bcache_put(cur->fs->cache, b);
}

static void blk_header(const struct xfs_btree_cur *cur, struct bbuf *b, uint32_t level,
                       uint32_t agbno) {
    for (uint32_t i = 0; i < cur->fs->blocksize; i++) {
        b->data[i] = 0;
    }
    xfs_put_be32(b->data, cur->magic);
    xfs_put_be16(b->data + XFS_BTREE_LEVEL, (uint16_t)level);
    xfs_put_be32(b->data + XFS_BTREE_LEFTSIB, XFS_NULL_AGBNO);
    xfs_put_be32(b->data + XFS_BTREE_RIGHTSIB, XFS_NULL_AGBNO);
    if (cur->fs->v5) {
        xfs_put_be64(b->data + XFS_BTREE_BLKNO, xfs_fsb_to_daddr(cur->fs, fsb_of(cur, agbno)));
        xfs_copy_bytes(b->data + XFS_BTREE_UUID, cur->fs->uuid, 16u);
        xfs_put_be32(b->data + XFS_BTREE_OWNER, cur->agno);
    }
}

/* A fresh, empty block at `level`, from the tree's supply. */
static int blk_new(struct xfs_btree_cur *cur, uint32_t level, uint32_t *agbno, struct bbuf **b) {
    int rc = cur->ops->alloc_block(cur, agbno);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = bcache_get(cur->fs->cache, xfs_fsb_to_block(cur->fs, fsb_of(cur, *agbno)), b);
    if (rc == KERR_OK) {
        blk_header(cur, *b, level, *agbno);
    }
    return rc;
}

/* ── the AGF's view of the tree ────────────────────────────────────────── */

/* The header that holds this tree's root: the AGF, or the AGI for the inode
 * trees, checked either way. */
static int header_get(struct xfs *fs, const struct xfs_btree_ops *ops, uint32_t agno,
                      struct bbuf **b, uint8_t **at) {
    return ops->in_agi ? xfs_agi_get(fs, agno, b, at) : xfs_agf_get(fs, agno, b, at);
}

static int root_write(struct xfs_btree_cur *cur, uint32_t root, uint32_t levels) {
    struct bbuf *b;
    uint8_t *agf;
    int rc = header_get(cur->fs, cur->ops, cur->agno, &b, &agf);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_put_be32(agf + cur->ops->root_off, root);
    xfs_put_be32(agf + cur->ops->level_off, levels);
    xfs_reseal(cur->fs, agf, cur->fs->sectsize, cur->ops->in_agi ? XFS_AGI_CRC : XFS_AGF_CRC);
    bcache_dirty(b);
    bcache_put(cur->fs->cache, b);
    cur->root = root;
    cur->levels = levels;
    return KERR_OK;
}

int xfs_btree_open(struct xfs_btree_cur *cur, struct xfs *fs, const struct xfs_btree_ops *ops,
                   uint32_t agno) {
    struct bbuf *b;
    uint8_t *agf;
    int rc = header_get(fs, ops, agno, &b, &agf);
    if (rc != KERR_OK) {
        return rc;
    }
    *cur = (struct xfs_btree_cur){.fs = fs, .ops = ops, .agno = agno, .magic = ops->magic};
    cur->root = xfs_be32(agf + ops->root_off);
    cur->levels = xfs_be32(agf + ops->level_off);
    bcache_put(fs->cache, b);
    return cur->levels == 0 || cur->levels > XFS_BTREE_MAXLEVELS ? KERR_IO : KERR_OK;
}

uint32_t xfs_btree_worst_case(const struct xfs_btree_cur *cur) {
    return cur->levels + 1u;
}

/* ── keys ──────────────────────────────────────────────────────────────── */

/* The larger of two keys into `into`. */
static void keep_higher(const struct xfs_btree_cur *cur, uint8_t *into, const uint8_t *k) {
    if (cur->ops->cmp(k, into) > 0) {
        xfs_copy_bytes(into, k, cur->ops->key_size);
    }
}

/* A block's key slot for its parent: its lowest key, and in an overlapping
 * tree the highest key anything beneath it reaches. */
static void block_keys(const struct xfs_btree_cur *cur, const struct bbuf *b, uint32_t level,
                       uint8_t *slot) {
    uint32_t ks = cur->ops->key_size;
    uint8_t high[XFS_BTREE_KEY_MAX];
    for (uint32_t i = 0; i < count_of(b); i++) {
        const uint8_t *e = entry_at(cur, b, level, i);
        if (level == 0) {
            cur->ops->high_key(e, high);
        } else {
            xfs_copy_bytes(high, e + (cur->ops->overlapping ? ks : 0u), ks);
        }
        if (i == 0) {
            if (level == 0) {
                cur->ops->low_key(e, slot);
            } else {
                xfs_copy_bytes(slot, e, ks);
            }
            xfs_copy_bytes(slot + ks, high, ks);
        } else {
            keep_higher(cur, slot + ks, high);
        }
    }
}

/* Writes the keys of the block at `level` of the path into its parent. */
static int parent_keys(struct xfs_btree_cur *cur, uint32_t level) {
    uint8_t slot[SLOT_MAX];
    struct bbuf *b;
    int rc = blk_get(cur, cur->bno[level], &b);
    if (rc != KERR_OK) {
        return rc;
    }
    block_keys(cur, b, level, slot);
    bcache_put(cur->fs->cache, b);
    rc = blk_get(cur, cur->bno[level + 1u], &b);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_copy_bytes(entry_at(cur, b, level + 1u, (uint32_t)cur->idx[level + 1u]), slot,
                   slot_size(cur));
    blk_done(cur, b);
    return KERR_OK;
}

/* Carries a changed block's keys all the way to the root. */
static int keys_up(struct xfs_btree_cur *cur, uint32_t level) {
    int rc = KERR_OK;
    for (uint32_t l = level; rc == KERR_OK && l + 1u < cur->levels; l++) {
        rc = parent_keys(cur, l);
    }
    return rc;
}

/* An entry naming the block `agbno`, for its parent. */
static int entry_for(struct xfs_btree_cur *cur, uint32_t agbno, uint32_t level, struct entry *e) {
    struct bbuf *b;
    int rc = blk_get(cur, agbno, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    block_keys(cur, b, level, e->bytes);
    e->ptr = agbno;
    bcache_put(cur->fs->cache, b);
    return KERR_OK;
}

/* ── lookup and reading ────────────────────────────────────────────────── */

/* The last entry in a block whose low key is at or below `key`, or -1. */
static int32_t last_at_or_below(const struct xfs_btree_cur *cur, const struct bbuf *b,
                                uint32_t level, const uint8_t *key) {
    uint8_t low[XFS_BTREE_KEY_MAX];
    int32_t found = -1;
    for (uint32_t i = 0; i < count_of(b); i++) {
        const uint8_t *e = entry_at(cur, b, level, i);
        if (level == 0) {
            cur->ops->low_key(e, low);
        } else {
            xfs_copy_bytes(low, e, cur->ops->key_size);
        }
        if (cur->ops->cmp(low, key) > 0) {
            break;
        }
        found = (int32_t)i;
    }
    return found;
}

int xfs_btree_lookup_le(struct xfs_btree_cur *cur, const uint8_t *key) {
    uint32_t at = cur->root;
    for (uint32_t l = cur->levels; l-- > 0;) {
        struct bbuf *b;
        int rc = blk_get(cur, at, &b);
        if (rc != KERR_OK) {
            return rc;
        }
        if (xfs_be16(b->data + XFS_BTREE_LEVEL) != l || (l > 0 && count_of(b) == 0)) {
            bcache_put(cur->fs->cache, b);
            return KERR_IO; /* a block at the wrong height, or an empty node */
        }
        cur->bno[l] = at;
        cur->idx[l] = last_at_or_below(cur, b, l, key);
        if (l > 0) {
            cur->idx[l] = cur->idx[l] < 0 ? 0 : cur->idx[l];
            at = xfs_be32(ptr_at(cur, b, (uint32_t)cur->idx[l]));
        }
        bcache_put(cur->fs->cache, b);
    }
    return KERR_OK;
}

/* Copies leaf record `i` of block `agbno`, if it has one. */
static int record_of(struct xfs_btree_cur *cur, uint32_t agbno, uint32_t i, uint8_t *rec) {
    struct bbuf *b;
    int rc = blk_get(cur, agbno, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = i < count_of(b) ? KERR_OK : KERR_NOENT;
    if (rc == KERR_OK) {
        xfs_copy_bytes(rec, entry_at(cur, b, 0, i), cur->ops->rec_size);
    }
    bcache_put(cur->fs->cache, b);
    return rc;
}

int xfs_btree_get(struct xfs_btree_cur *cur, uint8_t *rec) {
    return cur->idx[0] < 0 ? KERR_NOENT : record_of(cur, cur->bno[0], (uint32_t)cur->idx[0], rec);
}

int xfs_btree_peek_next(struct xfs_btree_cur *cur, uint8_t *rec) {
    struct bbuf *b;
    uint32_t right;
    int rc = record_of(cur, cur->bno[0], (uint32_t)(cur->idx[0] + 1), rec);
    if (rc != KERR_NOENT) {
        return rc;
    }
    rc = blk_get(cur, cur->bno[0], &b);
    if (rc != KERR_OK) {
        return rc;
    }
    right = xfs_be32(b->data + XFS_BTREE_RIGHTSIB);
    bcache_put(cur->fs->cache, b);
    return right == XFS_NULL_AGBNO ? KERR_NOENT : record_of(cur, right, 0, rec);
}

int xfs_btree_get_prev(struct xfs_btree_cur *cur, uint32_t *leaf, int32_t *idx, uint8_t *rec) {
    struct bbuf *b;
    int rc;
    if (*idx > 0) {
        (*idx)--;
        return record_of(cur, *leaf, (uint32_t)*idx, rec);
    }
    rc = blk_get(cur, *leaf, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    *leaf = xfs_be32(b->data + XFS_BTREE_LEFTSIB);
    bcache_put(cur->fs->cache, b);
    if (*leaf == XFS_NULL_AGBNO) {
        return KERR_NOENT;
    }
    rc = blk_get(cur, *leaf, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    *idx = (int32_t)count_of(b) - 1;
    bcache_put(cur->fs->cache, b);
    return *idx < 0 ? KERR_IO : record_of(cur, *leaf, (uint32_t)*idx, rec);
}

int xfs_btree_update(struct xfs_btree_cur *cur, const uint8_t *rec) {
    struct bbuf *b;
    int rc = cur->idx[0] < 0 ? KERR_NOENT : blk_get(cur, cur->bno[0], &b);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_copy_bytes(entry_at(cur, b, 0, (uint32_t)cur->idx[0]), rec, cur->ops->rec_size);
    blk_done(cur, b);
    return keys_up(cur, 0);
}

/* ── insert ────────────────────────────────────────────────────────────── */

/* Points the block after `agbno`'s old right sibling back at `agbno`. */
static int relink_right(struct xfs_btree_cur *cur, uint32_t right, uint32_t agbno) {
    struct bbuf *b;
    int rc;
    if (right == XFS_NULL_AGBNO) {
        return KERR_OK;
    }
    rc = blk_get(cur, right, &b);
    if (rc == KERR_OK) {
        xfs_put_be32(b->data + XFS_BTREE_LEFTSIB, agbno);
        blk_done(cur, b);
    }
    return rc;
}

/* Moves the upper half of a full block into a new right sibling, puts the
 * entry in whichever half `pos` falls in, and says where the new block is.
 * The order Linux uses — the lower half stays — so the left block keeps
 * its place in its parent and only the new one needs an entry there. */
static int split(struct xfs_btree_cur *cur, uint32_t level, struct bbuf *b, uint32_t pos,
                 const struct entry *e, uint32_t *right) {
    struct bbuf *nb;
    struct entry move;
    uint32_t n = count_of(b), keep = n / 2u;
    int rc = blk_new(cur, level, right, &nb);
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint32_t i = keep; i < n; i++) {
        entry_read(cur, b, level, i, &move);
        entry_write(cur, nb, level, i - keep, &move);
    }
    set_count(nb, n - keep);
    set_count(b, keep);
    entry_insert(cur, pos <= keep ? b : nb, level, pos <= keep ? pos : pos - keep, e);
    xfs_put_be32(nb->data + XFS_BTREE_LEFTSIB, cur->bno[level]);
    xfs_put_be32(nb->data + XFS_BTREE_RIGHTSIB, xfs_be32(b->data + XFS_BTREE_RIGHTSIB));
    xfs_put_be32(b->data + XFS_BTREE_RIGHTSIB, *right);
    rc = relink_right(cur, xfs_be32(nb->data + XFS_BTREE_RIGHTSIB), *right);
    blk_done(cur, nb);
    return rc;
}

/* Puts an entry into the path's block at `level`, splitting it when it is
 * full; *right is the new block, or XFS_NULL_AGBNO when there was room. */
static int place(struct xfs_btree_cur *cur, uint32_t level, uint32_t pos, const struct entry *e,
                 uint32_t *right) {
    struct bbuf *b;
    int rc = blk_get(cur, cur->bno[level], &b);
    if (rc != KERR_OK) {
        return rc;
    }
    *right = XFS_NULL_AGBNO;
    if (count_of(b) < max_entries(cur, level)) {
        entry_insert(cur, b, level, pos, e);
    } else {
        rc = split(cur, level, b, pos, e, right);
    }
    blk_done(cur, b);
    return rc;
}

/* A root above the old one, naming it and the block it split into. */
static int new_root(struct xfs_btree_cur *cur, uint32_t right) {
    struct entry left_e, right_e;
    struct bbuf *b;
    uint32_t level = cur->levels, agbno;
    int rc = entry_for(cur, cur->root, level - 1u, &left_e);
    rc = rc != KERR_OK ? rc : entry_for(cur, right, level - 1u, &right_e);
    if (rc != KERR_OK || level >= XFS_BTREE_MAXLEVELS) {
        return rc != KERR_OK ? rc : KERR_NOSPC;
    }
    rc = blk_new(cur, level, &agbno, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    entry_insert(cur, b, level, 0, &left_e);
    entry_insert(cur, b, level, 1, &right_e);
    blk_done(cur, b);
    return root_write(cur, agbno, level + 1u);
}

int xfs_btree_insert(struct xfs_btree_cur *cur, const uint8_t *rec) {
    struct entry e;
    uint32_t pos = (uint32_t)(cur->idx[0] + 1), right;
    xfs_copy_bytes(e.bytes, rec, cur->ops->rec_size);
    e.ptr = 0;
    for (uint32_t level = 0;; level++) {
        int rc = place(cur, level, pos, &e, &right);
        if (rc != KERR_OK) {
            return rc;
        }
        if (level + 1u == cur->levels) {
            return right == XFS_NULL_AGBNO ? KERR_OK : new_root(cur, right);
        }
        rc = parent_keys(cur, level);
        if (rc != KERR_OK || right == XFS_NULL_AGBNO) {
            return rc != KERR_OK ? rc : keys_up(cur, level + 1u);
        }
        rc = entry_for(cur, right, level, &e);
        if (rc != KERR_OK) {
            return rc;
        }
        pos = (uint32_t)cur->idx[level + 1u] + 1u;
    }
}

/* ── delete ────────────────────────────────────────────────────────────── */

/* Takes the path's entry out of its block at `level`; *left is what remains. */
static int take_out(struct xfs_btree_cur *cur, uint32_t level, uint32_t *left) {
    struct bbuf *b;
    int rc = blk_get(cur, cur->bno[level], &b);
    if (rc != KERR_OK) {
        return rc;
    }
    if ((uint32_t)cur->idx[level] >= count_of(b)) {
        bcache_put(cur->fs->cache, b);
        return KERR_IO;
    }
    entry_remove(cur, b, level, (uint32_t)cur->idx[level]);
    *left = count_of(b);
    blk_done(cur, b);
    return KERR_OK;
}

/* A root node left with one child hands the tree to that child. */
static int collapse_root(struct xfs_btree_cur *cur) {
    struct bbuf *b;
    uint32_t old = cur->root, child;
    int rc = blk_get(cur, old, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    child = xfs_be32(ptr_at(cur, b, 0));
    bcache_put(cur->fs->cache, b);
    rc = root_write(cur, child, cur->levels - 1u);
    return rc != KERR_OK ? rc : cur->ops->free_block(cur, old);
}

/* Moves one entry from a sibling that can spare it: the last of a left
 * sibling to the front, or the first of a right one to the end. */
static int borrow(struct xfs_btree_cur *cur, uint32_t level, uint32_t sib, bool sib_left) {
    struct bbuf *b, *s;
    struct entry e;
    int rc = blk_get(cur, sib, &s);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = blk_get(cur, cur->bno[level], &b);
    if (rc != KERR_OK) {
        bcache_put(cur->fs->cache, s);
        return rc;
    }
    entry_read(cur, s, level, sib_left ? count_of(s) - 1u : 0, &e);
    entry_remove(cur, s, level, sib_left ? count_of(s) - 1u : 0);
    entry_insert(cur, b, level, sib_left ? 0 : count_of(b), &e);
    blk_done(cur, b);
    blk_done(cur, s);
    return KERR_OK;
}

/* Appends every entry of `right` to `left`, takes `right` out of the
 * sibling chain and gives its block back. */
static int join(struct xfs_btree_cur *cur, uint32_t level, uint32_t left, uint32_t right) {
    struct bbuf *l, *r;
    struct entry e;
    uint32_t far;
    int rc = blk_get(cur, left, &l);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = blk_get(cur, right, &r);
    if (rc != KERR_OK) {
        bcache_put(cur->fs->cache, l);
        return rc;
    }
    for (uint32_t i = 0; i < count_of(r); i++) {
        entry_read(cur, r, level, i, &e);
        entry_insert(cur, l, level, count_of(l), &e);
    }
    far = xfs_be32(r->data + XFS_BTREE_RIGHTSIB);
    xfs_put_be32(l->data + XFS_BTREE_RIGHTSIB, far);
    bcache_put(cur->fs->cache, r);
    blk_done(cur, l);
    rc = relink_right(cur, far, left);
    return rc != KERR_OK ? rc : cur->ops->free_block(cur, right);
}

/* Writes the keys of the child in parent slot `slot` into that slot. */
static int slot_keys(struct xfs_btree_cur *cur, uint32_t level, int32_t slot) {
    int32_t saved = cur->idx[level + 1u];
    uint32_t saved_bno = cur->bno[level];
    struct entry e;
    int rc = KERR_OK;
    struct bbuf *p;
    rc = blk_get(cur, cur->bno[level + 1u], &p);
    if (rc != KERR_OK) {
        return rc;
    }
    entry_read(cur, p, level + 1u, (uint32_t)slot, &e);
    bcache_put(cur->fs->cache, p);
    cur->bno[level] = e.ptr;
    cur->idx[level + 1u] = slot;
    rc = parent_keys(cur, level);
    cur->bno[level] = saved_bno;
    cur->idx[level + 1u] = saved;
    return rc;
}

/* The sibling an underfull block at `level` balances against — the one to
 * its left in the same parent when there is one — and how full it is. */
static int sibling_of(struct xfs_btree_cur *cur, uint32_t level, uint32_t *sib, bool *sib_left,
                      uint32_t *sib_count) {
    struct bbuf *p, *s;
    int32_t i = cur->idx[level + 1u];
    int rc = blk_get(cur, cur->bno[level + 1u], &p);
    if (rc != KERR_OK) {
        return rc;
    }
    *sib_left = i > 0;
    rc = *sib_left || (uint32_t)i + 1u < count_of(p) ? KERR_OK : KERR_IO;
    if (rc == KERR_OK) {
        *sib = xfs_be32(ptr_at(cur, p, (uint32_t)(*sib_left ? i - 1 : i + 1)));
    }
    bcache_put(cur->fs->cache, p);
    rc = rc != KERR_OK ? rc : blk_get(cur, *sib, &s);
    if (rc == KERR_OK) {
        *sib_count = count_of(s);
        bcache_put(cur->fs->cache, s);
    }
    return rc;
}

/* Restores the half-full rule at `level`. Either it borrows, and the parent
 * only needs its keys rewritten, or it joins two blocks into one and the
 * parent loses the slot left pointing at the block that went: *gone is that
 * slot, or -1 after a borrow. */
static int rebalance(struct xfs_btree_cur *cur, uint32_t level, int32_t *gone) {
    uint32_t sib = 0, sib_count = 0;
    bool sib_left = false;
    int32_t i = cur->idx[level + 1u];
    int rc = sibling_of(cur, level, &sib, &sib_left, &sib_count);
    if (rc != KERR_OK) {
        return rc;
    }
    if (sib_count > min_entries(cur, level)) {
        *gone = -1;
        rc = borrow(cur, level, sib, sib_left);
        rc = rc != KERR_OK ? rc : slot_keys(cur, level, sib_left ? i - 1 : i + 1);
        return rc != KERR_OK ? rc : parent_keys(cur, level);
    }
    *gone = sib_left ? i : i + 1;
    rc = sib_left ? join(cur, level, sib, cur->bno[level]) : join(cur, level, cur->bno[level], sib);
    return rc != KERR_OK ? rc : slot_keys(cur, level, *gone - 1);
}

int xfs_btree_delete(struct xfs_btree_cur *cur) {
    if (cur->idx[0] < 0) {
        return KERR_NOENT;
    }
    for (uint32_t level = 0;; level++) {
        uint32_t left = 0;
        int32_t gone = -1;
        int rc = take_out(cur, level, &left);
        if (rc != KERR_OK) {
            return rc;
        }
        if (level + 1u == cur->levels) {
            return level > 0 && left == 1u ? collapse_root(cur) : KERR_OK;
        }
        if (left >= min_entries(cur, level)) {
            return keys_up(cur, level);
        }
        rc = rebalance(cur, level, &gone);
        if (rc != KERR_OK || gone < 0) {
            return rc != KERR_OK ? rc : keys_up(cur, level + 1u);
        }
        cur->idx[level + 1u] = gone;
    }
}
