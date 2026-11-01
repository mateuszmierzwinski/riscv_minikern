/* An attribute fork in blocks: one leaf at block 0, or a tree of leaves.
 *
 * **Why.** The writer kept a file's attributes in one leaf block and
 * answered `NOSPC` when it filled — about four kilobytes of names and
 * values — and the reader refused a fork in any other shape, so attributes
 * Linux had written into more than one block could not be read at all.
 * Linux turns a full leaf into **node** format: block 0 becomes a `da3`
 * node indexing leaves by hash, exactly as a large directory's index does,
 * and leaves split as they fill. This does the same, on the shared da-tree
 * (`xfs_danode.c`) through this fork's own block I/O.
 *
 * Every operation starts from the same question — which leaf holds, or
 * would hold, this hash — and then uses the leaf primitives of
 * `xfs_attr.c` on that leaf in `fs->dirblock`. A run of equal hashes may
 * continue into the next leaf, so a search that ends at a leaf's last
 * entry still matching follows `forw`.
 *
 * A full leaf is first **compacted** when it has holes (a removal leaves
 * its name and value in place), and only split when that is not enough:
 * the entries are copied aside and put again into one or two fresh
 * leaves, which is also what compaction is.
 */
#include "core/bcache.h"
#include "core/errno.h"
#include "core/xfs/xfs_format.h"
#include "core/xfs/xfs_internal.h"

#include <stddef.h>

/* A copy of the leaf being rebuilt: the buffer is the destination. Static
 * for the kernel-stack reason every block-sized buffer here is. */
static uint8_t src[XFS_DIRBLK_MAX];

/* ── the fork as a da-tree's owner ────────────────────────────────────── */

static int attr_load(struct xfs_da_tree *t, uint64_t logical, uint64_t *fsb) {
    struct xfs *fs = t->fs;
    struct xfs_extent e;
    struct bbuf *b;
    int rc = xfs_attr_bmap(fs, t->ip, logical, &e);
    if (rc != KERR_OK) {
        return rc;
    }
    *fsb = e.startblock + (logical - e.startoff);
    rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, *fsb), &b);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_copy_bytes(fs->dirblock, b->data, fs->blocksize);
    bcache_put(fs->cache, b);
    return xfs_crc_ok(fs, fs->dirblock, fs->blocksize, XFS_DA3_CRC_OFF) &&
                   xfs_uuid_ok(fs, fs->dirblock + XFS_DA3_UUID_OFF)
               ? KERR_OK
               : KERR_IO;
}

static int attr_store(struct xfs_da_tree *t, uint64_t fsb) {
    return xfs_dir_raw_store(t->fs, fsb);
}

static int attr_grow(struct xfs_da_tree *t, uint64_t *logical, uint64_t *fsb, uint64_t now_s) {
    return xfs_attr_fork_grow(t->fs, t->ip, logical, fsb, now_s);
}

static int attr_shrink(struct xfs_da_tree *t, uint64_t logical, uint64_t now_s) {
    int rc = xfs_attr_fork_remove(t->fs, t->ip, (uint32_t)logical, 1u, now_s);
    return rc != KERR_OK ? rc : xfs_inode_read(t->fs, t->ip->ino, t->ip);
}

static const struct xfs_da_ops attr_ops = {attr_load, attr_store, attr_grow, attr_shrink};

static struct xfs_da_tree attr_tree(struct xfs *fs, struct xfs_inode *ip) {
    return (struct xfs_da_tree){&attr_ops, fs, ip, 0, fs->blocksize};
}

/* ── finding leaves ───────────────────────────────────────────────────── */

static uint32_t leaf_count(const struct xfs *fs) {
    return xfs_be16(fs->dirblock + fs->attr_base);
}

/* The highest hash in the leaf in the buffer; 0 for an empty one. */
static uint32_t leaf_top(const struct xfs *fs) {
    uint32_t n = leaf_count(fs);
    return n == 0 ? 0 : xfs_be32(fs->dirblock + fs->attr_hdr + (n - 1u) * ATTR_LEAF_ENTRY);
}

static bool is_node(const struct xfs *fs) {
    return xfs_be16(fs->dirblock + XFS_DA3_MAGIC_OFF) == xfs_magic_for(fs, XFS_BLK_DA_NODE);
}

static bool is_leaf(const struct xfs *fs) {
    return xfs_be16(fs->dirblock + XFS_DA3_MAGIC_OFF) == xfs_magic_for(fs, XFS_BLK_ATTR_LEAF);
}

/* The leaf `hash` belongs in, loaded, at `*fsb`: block 0 itself when the
 * fork is one leaf (`p->depth` 0), or the one the index descends to. */
static int leaf_for(struct xfs_da_tree *t, uint32_t hash, struct xfs_da_path *p, uint64_t *fsb) {
    struct xfs *fs = t->fs;
    int rc = t->ops->load(t, 0, fsb);
    p->depth = 0;
    p->leaf = 0;
    if (rc == KERR_OK && is_node(fs)) {
        rc = xfs_da_descend(t, hash, p);
        rc = rc != KERR_OK ? rc : t->ops->load(t, p->leaf, fsb);
    }
    return rc != KERR_OK ? rc : (is_leaf(fs) ? KERR_OK : KERR_IO);
}

/* The leaf after the one in the buffer, loaded, and its logical block;
 * `KERR_NOENT` at the last. */
static int next_leaf(struct xfs_da_tree *t, uint64_t *logical, uint64_t *fsb) {
    struct xfs *fs = t->fs;
    uint64_t forw = xfs_be32(fs->dirblock + XFS_DA3_FORW_OFF);
    int rc = forw == 0 ? KERR_NOENT : t->ops->load(t, forw, fsb);
    *logical = forw;
    return rc != KERR_OK ? rc : (is_leaf(fs) ? KERR_OK : KERR_IO);
}

/* Whether the run of `hash` may continue past the leaf in the buffer. */
static bool run_continues(const struct xfs *fs, uint32_t hash) {
    return leaf_count(fs) > 0 && leaf_top(fs) == hash;
}

/* Where in the leaf in the buffer the entry `name` (in namespace `flags`)
 * is, or the count when it is not there. */
static uint32_t entry_named(struct xfs *fs, const char *name, uint32_t namelen, uint8_t flags) {
    uint32_t n = leaf_count(fs);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *at = NULL;
        uint32_t len = 0, vlen = 0, value_at = 0, k = 0;
        uint8_t f = 0;
        if (xfs_attr_leaf_entry(fs, fs->dirblock, i, &at, &len, &vlen, &value_at, &f) != KERR_OK ||
            len != namelen || (f & XFS_ATTR_NSP) != flags) {
            continue;
        }
        while (k < len && at[k] == (uint8_t)name[k]) {
            k++;
        }
        if (k == len) {
            return i;
        }
    }
    return n;
}

/* The leaf holding `name`, loaded: its logical block and `fsb`, and the
 * entry's index. `KERR_NOENT` when no leaf has it. */
static int leaf_holding(struct xfs_da_tree *t, const char *name, uint32_t namelen, uint8_t flags,
                        uint64_t *logical, uint64_t *fsb, uint32_t *index) {
    struct xfs *fs = t->fs;
    uint32_t hash = xfs_dir_hashname(name, namelen);
    struct xfs_da_path p;
    int rc = leaf_for(t, hash, &p, fsb);
    *logical = p.leaf;
    for (uint32_t hops = 0; rc == KERR_OK && hops < 1024u; hops++) {
        *index = entry_named(fs, name, namelen, flags);
        if (*index < leaf_count(fs)) {
            return KERR_OK;
        }
        rc = run_continues(fs, hash) ? next_leaf(t, logical, fsb) : KERR_NOENT;
    }
    return rc == KERR_OK ? KERR_IO : rc;
}

/* ── reading ──────────────────────────────────────────────────────────── */

int xfs_attr_tree_get(struct xfs *fs, struct xfs_inode *ip, const char *name, uint32_t namelen,
                      uint8_t flags, uint8_t *out, uint32_t room, uint32_t *valuelen) {
    struct xfs_da_tree t = attr_tree(fs, ip);
    const uint8_t *at = NULL;
    uint64_t logical = 0, fsb = 0;
    uint32_t i = 0, len = 0, value_at = 0;
    uint8_t f = 0;
    int rc = leaf_holding(&t, name, namelen, flags, &logical, &fsb, &i);
    rc = rc != KERR_OK ? rc
                       : xfs_attr_leaf_entry(fs, fs->dirblock, i, &at, &len, valuelen, &value_at,
                                             &f);
    if (rc != KERR_OK || (f & XFS_ATTR_INCOMPLETE) != 0) {
        return rc != KERR_OK ? rc : KERR_NOENT;
    }
    if ((f & XFS_ATTR_LOCAL) == 0) {
        uint32_t idx = xfs_be16(fs->dirblock + fs->attr_hdr + i * ATTR_LEAF_ENTRY +
                                ATTR_LEAF_NAMEIDX);
        return xfs_attr_rmt_read(fs, ip, xfs_be32(fs->dirblock + idx + ATTR_REMOTE_VALUEBLK),
                                 *valuelen, out, room);
    }
    if (*valuelen > room) {
        return KERR_RANGE;
    }
    xfs_copy_bytes(out, fs->dirblock + value_at, *valuelen);
    return KERR_OK;
}

/* Reports every complete entry of the leaf in the buffer. */
static int list_leaf(struct xfs *fs, xfs_attr_fn fn, void *self) {
    for (uint32_t i = 0; i < leaf_count(fs); i++) {
        const uint8_t *at = NULL;
        uint32_t len = 0, vlen = 0, value_at = 0;
        uint8_t f = 0;
        int rc = xfs_attr_leaf_entry(fs, fs->dirblock, i, &at, &len, &vlen, &value_at, &f);
        rc = rc != KERR_OK || (f & XFS_ATTR_INCOMPLETE) != 0
                 ? rc
                 : fn(self, at, len, (uint8_t)(f & XFS_ATTR_NSP), vlen);
        if (rc != KERR_OK) {
            return rc;
        }
    }
    return KERR_OK;
}

/* Every leaf from the first along `forw`. The callback must not use the
 * filesystem: the leaf it is reading from is the buffer. */
int xfs_attr_tree_list(struct xfs *fs, struct xfs_inode *ip, xfs_attr_fn fn, void *self) {
    struct xfs_da_tree t = attr_tree(fs, ip);
    struct xfs_da_path p;
    uint64_t logical = 0, fsb = 0;
    int rc = leaf_for(&t, 0, &p, &fsb);
    for (uint32_t hops = 0; rc == KERR_OK && hops < 1u << 20; hops++) {
        rc = list_leaf(fs, fn, self);
        rc = rc != KERR_OK ? rc : next_leaf(&t, &logical, &fsb);
    }
    return rc == KERR_NOENT ? KERR_OK : rc;
}

/* ── rebuilding a leaf: compaction and splitting ──────────────────────── */

/* Puts entry `i` of the leaf copy in `src` into the leaf in the buffer. */
static int reput(struct xfs *fs, uint32_t i) {
    const uint8_t *name = NULL;
    uint32_t len = 0, vlen = 0, value_at = 0, idx;
    uint8_t f = 0;
    int rc = xfs_attr_leaf_entry(fs, src, i, &name, &len, &vlen, &value_at, &f);
    if (rc != KERR_OK) {
        return rc;
    }
    if ((f & XFS_ATTR_LOCAL) != 0) {
        return xfs_attr_leaf_put(fs, (const char *)name, len, src + value_at, vlen,
                                 (uint8_t)(f & ~XFS_ATTR_LOCAL));
    }
    idx = xfs_be16(src + fs->attr_hdr + i * ATTR_LEAF_ENTRY + ATTR_LEAF_NAMEIDX);
    return xfs_attr_leaf_put_remote(fs, (const char *)name, len,
                                    xfs_be32(src + idx + ATTR_REMOTE_VALUEBLK), vlen, f);
}

/* A fresh leaf in the buffer holding entries [from, to) of the copy, with
 * its siblings, written to `fsb`. *top is its highest hash. */
static int rebuild(struct xfs_da_tree *t, uint32_t from, uint32_t to, uint64_t forw,
                   uint64_t back, uint64_t fsb, uint32_t *top) {
    struct xfs *fs = t->fs;
    int rc = KERR_OK;
    xfs_attr_leaf_empty(fs, t->ip->ino);
    xfs_put_be32(fs->dirblock + XFS_DA3_FORW_OFF, (uint32_t)forw);
    xfs_put_be32(fs->dirblock + XFS_DA3_BACK_OFF, (uint32_t)back);
    for (uint32_t i = from; rc == KERR_OK && i < to; i++) {
        rc = reput(fs, i);
    }
    *top = leaf_top(fs);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_reseal(fs, fs->dirblock, fs->blocksize, XFS_DA3_CRC_OFF);
    return t->ops->store(t, fsb);
}

/* Whether the leaf in the buffer has holes a rebuild would give back. */
static bool has_holes(const struct xfs *fs) {
    return fs->dirblock[fs->attr_base + 6u] != 0;
}

/* The leaf in the buffer, at `fsb`, rebuilt in place without its holes. */
static int compact(struct xfs_da_tree *t, uint64_t fsb) {
    struct xfs *fs = t->fs;
    uint32_t top = 0;
    xfs_copy_bytes(src, fs->dirblock, fs->blocksize);
    return rebuild(t, 0, leaf_count(fs), xfs_be32(src + XFS_DA3_FORW_OFF),
                   xfs_be32(src + XFS_DA3_BACK_OFF), fsb, &top);
}

/* Splits the path's leaf, in the buffer at `fsb`: the lower half of its
 * entries stays, the upper half goes to a new leaf after it, and the index
 * gains the new leaf. One entry cannot be split (`KERR_NOSPC`).
 *
 * Runs to 28 lines: both halves, both sibling links and the index entry
 * are one change that no part of leaves consistent (Rule 11, Single
 * derivation). */
static int split_leaf(struct xfs_da_tree *t, struct xfs_da_path *p, uint64_t fsb,
                      uint64_t now_s) {
    struct xfs *fs = t->fs;
    uint32_t n = leaf_count(fs), half = n / 2u, lower = 0, upper = 0;
    uint64_t at = 0, new_fsb = 0, forw;
    int rc;
    if (n < 2u) {
        return KERR_NOSPC;
    }
    xfs_copy_bytes(src, fs->dirblock, fs->blocksize);
    forw = xfs_be32(src + XFS_DA3_FORW_OFF);
    rc = t->ops->grow(t, &at, &new_fsb, now_s);
    rc = rc != KERR_OK ? rc
                       : rebuild(t, 0, half, at, xfs_be32(src + XFS_DA3_BACK_OFF), fsb, &lower);
    rc = rc != KERR_OK ? rc : rebuild(t, half, n, forw, p->leaf, new_fsb, &upper);
    rc = rc != KERR_OK ? rc : xfs_da_set_sibling(t, forw, XFS_DA3_BACK_OFF, (uint32_t)at);
    return rc != KERR_OK ? rc
                         : xfs_da_add_child(t, p,
                                            &(struct xfs_da_split){lower, at, upper}, now_s);
}

/* The fork's one leaf, full, becomes node format: its contents move to a
 * new block, and block 0 becomes a node naming it — the root stays at
 * block 0 because that is where every search starts. */
static int to_node(struct xfs_da_tree *t, uint64_t root_fsb, uint64_t now_s) {
    struct xfs *fs = t->fs;
    uint64_t at = 0, fsb = 0;
    uint32_t top = leaf_top(fs);
    int rc;
    xfs_copy_bytes(src, fs->dirblock, fs->blocksize);
    rc = t->ops->grow(t, &at, &fsb, now_s);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_copy_bytes(fs->dirblock, src, fs->blocksize);
    rc = t->ops->store(t, fsb); /* the block number is stamped for its new place */
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_da_header(fs, fs->blocksize, xfs_magic_for(fs, XFS_BLK_DA_NODE), t->ip->ino, 1u, 0, 0);
    xfs_put_be16(fs->dirblock + fs->da_level, 1u);
    xfs_put_be32(fs->dirblock + fs->da_hdr, top);
    xfs_put_be32(fs->dirblock + fs->da_hdr + 4u, (uint32_t)at);
    xfs_reseal(fs, fs->dirblock, fs->blocksize, XFS_DA3_CRC_OFF);
    return t->ops->store(t, root_fsb);
}

/* ── writing ──────────────────────────────────────────────────────────── */

static int put_entry(struct xfs *fs, const struct xfs_attr_new *n) {
    return n->remote ? xfs_attr_leaf_put_remote(fs, n->name, n->namelen, n->valueblk,
                                                n->valuelen, n->flags)
                     : xfs_attr_leaf_put(fs, n->name, n->namelen, n->value, n->valuelen,
                                         n->flags);
}

/* Makes room in the leaf in the buffer, which refused an entry: compacts it
 * when it has holes, otherwise splits it, or — one leaf — makes the fork a
 * node. */
static int make_room(struct xfs_da_tree *t, struct xfs_da_path *p, uint64_t fsb, uint64_t now_s) {
    if (has_holes(t->fs)) {
        return compact(t, fsb);
    }
    return p->depth == 0 ? to_node(t, fsb, now_s) : split_leaf(t, p, fsb, now_s);
}

/* Puts `n` into the leaf its hash belongs in, after making room as often as
 * it takes (holes, then a split or the conversion, then a split of the
 * half it lands in). A new highest hash in a leaf moves the keys above. */
static int put_in_tree(struct xfs_da_tree *t, const struct xfs_attr_new *n, uint64_t now_s) {
    struct xfs *fs = t->fs;
    uint32_t hash = xfs_dir_hashname(n->name, n->namelen);
    for (uint32_t tries = 0; tries < 4u; tries++) {
        struct xfs_da_path p;
        uint64_t fsb = 0;
        uint32_t top;
        int rc = leaf_for(t, hash, &p, &fsb);
        top = rc == KERR_OK ? leaf_top(fs) : 0;
        rc = rc != KERR_OK ? rc : put_entry(fs, n);
        if (rc == KERR_OK) {
            xfs_reseal(fs, fs->dirblock, fs->blocksize, XFS_DA3_CRC_OFF);
            rc = t->ops->store(t, fsb);
            return rc == KERR_OK && p.depth > 0 && (hash > top || leaf_count(fs) == 1u)
                       ? xfs_da_set_top(t, &p, hash)
                       : rc;
        }
        rc = rc != KERR_NOSPC ? rc : t->ops->load(t, p.leaf, &fsb);
        rc = rc != KERR_OK ? rc : make_room(t, &p, fsb, now_s);
        if (rc != KERR_OK) {
            return rc;
        }
    }
    return KERR_NOSPC;
}

int xfs_attr_tree_put(struct xfs *fs, struct xfs_inode *ip, const struct xfs_attr_new *n,
                      uint64_t now_s) {
    struct xfs_da_tree t = attr_tree(fs, ip);
    int rc = xfs_attr_tree_remove(fs, ip, n->name, n->namelen, n->flags, now_s);
    if (rc != KERR_OK && rc != KERR_NOENT) {
        return rc;
    }
    return put_in_tree(&t, n, now_s);
}

/* The blocks a remote value of `len` bytes takes. */
static uint32_t remote_blocks(const struct xfs *fs, uint32_t len) {
    uint32_t per = fs->blocksize - (fs->v5 ? ATTR_RMT_HDR : 0u);
    return (len + per - 1u) / per;
}

/* A leaf left empty leaves the tree (not the fork's only one): out of the
 * index, out of its sibling chain, out of the fork. In the buffer. */
static int drop_leaf(struct xfs_da_tree *t, struct xfs_da_path *p, uint64_t now_s) {
    uint32_t forw = xfs_be32(t->fs->dirblock + XFS_DA3_FORW_OFF);
    uint32_t back = xfs_be32(t->fs->dirblock + XFS_DA3_BACK_OFF);
    int rc = xfs_da_drop_child(t, p, now_s);
    rc = rc != KERR_OK ? rc : xfs_da_set_sibling(t, back, XFS_DA3_FORW_OFF, forw);
    rc = rc != KERR_OK ? rc : xfs_da_set_sibling(t, forw, XFS_DA3_BACK_OFF, back);
    return rc != KERR_OK ? rc : t->ops->shrink(t, p->leaf, now_s);
}

/* The removed entry's remote value, if it had one: where and how long. */
static void remote_of(struct xfs *fs, uint32_t i, uint32_t *valueblk, uint32_t *valuelen) {
    uint32_t e = fs->attr_hdr + i * ATTR_LEAF_ENTRY;
    uint32_t idx = xfs_be16(fs->dirblock + e + ATTR_LEAF_NAMEIDX);
    bool local = (fs->dirblock[e + ATTR_LEAF_FLAGS] & XFS_ATTR_LOCAL) != 0;
    *valueblk = local ? 0 : xfs_be32(fs->dirblock + idx + ATTR_REMOTE_VALUEBLK);
    *valuelen = local ? 0 : xfs_be32(fs->dirblock + idx + ATTR_REMOTE_VALUELEN);
}

/* After the entry left the leaf in the buffer (`was_top`: it had the
 * highest hash): an empty leaf leaves a node-format fork, a new highest
 * hash moves the keys above, and otherwise the leaf is just stored. */
static int after_take_out(struct xfs_da_tree *t, struct xfs_da_path *p, uint64_t fsb,
                          bool was_top, uint64_t now_s) {
    struct xfs *fs = t->fs;
    int rc;
    if (p->depth > 0 && leaf_count(fs) == 0 && !xfs_da_only_leaf(p)) {
        return drop_leaf(t, p, now_s);
    }
    xfs_reseal(fs, fs->dirblock, fs->blocksize, XFS_DA3_CRC_OFF);
    rc = t->ops->store(t, fsb);
    return rc == KERR_OK && p->depth > 0 && was_top && leaf_count(fs) > 0
               ? xfs_da_set_top(t, p, leaf_top(fs))
               : rc;
}

int xfs_attr_tree_remove(struct xfs *fs, struct xfs_inode *ip, const char *name,
                         uint32_t namelen, uint8_t flags, uint64_t now_s) {
    struct xfs_da_tree t = attr_tree(fs, ip);
    struct xfs_da_path p = {.depth = 0};
    uint64_t logical = 0, fsb = 0;
    uint32_t i = 0, valueblk = 0, valuelen = 0, hash = xfs_dir_hashname(name, namelen);
    bool was_top;
    int rc = leaf_holding(&t, name, namelen, flags, &logical, &fsb, &i);
    rc = rc != KERR_OK || logical == 0 ? rc : xfs_da_find(&t, hash, logical, &p);
    rc = rc != KERR_OK ? rc : t.ops->load(&t, logical, &fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    i = entry_named(fs, name, namelen, flags);
    was_top = i + 1u == leaf_count(fs);
    remote_of(fs, i, &valueblk, &valuelen);
    if (!xfs_attr_leaf_take_out(fs, name, namelen, flags)) {
        return KERR_IO;
    }
    if (leaf_count(fs) == 0 && (p.depth == 0 || xfs_da_only_leaf(&p))) {
        return xfs_attr_fork_empty(fs, ip, now_s); /* the last one: its value goes too */
    }
    rc = after_take_out(&t, &p, fsb, was_top, now_s);
    return rc != KERR_OK || valuelen == 0
               ? rc
               : xfs_attr_fork_remove(fs, ip, valueblk, remote_blocks(fs, valuelen), now_s);
}
