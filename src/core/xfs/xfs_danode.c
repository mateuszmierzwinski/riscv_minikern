/* A node directory's hash index above its leaves, at any height.
 *
 * **Why a file of its own.** The node format began with one node — the
 * root at `dir_leaf_block` naming every `LEAFN` block — and that root holds
 * about five hundred children: roughly 177,000 names with leaves half full
 * after splitting. Past that the index needs nodes under the root, which is
 * a B-tree with the operations every B-tree has, and none of them is about
 * directories: find the path to a leaf, change a key up that path, add a
 * child and split what overflows, drop a child and remove what empties.
 * `xfs_dirnode.c` keeps what is about directories — leaves of hashes, the
 * free index, the conversion — and calls these.
 *
 * Every node, the root included, is a `da3` node block: entries of
 * `{hashval, before}`, where `hashval` is the highest hash under that child
 * (`xfs_repair` checks equality, not a bound), and `before` the child's
 * logical block. Nodes on one level are chained by `forw`/`back`, as the
 * leaves are. The root never moves: a search starts at `dir_leaf_block`, so
 * when it fills, its contents move down into a new node and it becomes the
 * parent of that one, a level higher (Linux's `xfs_da3_root_split`).
 *
 * There is one block buffer (`fs->dirblock`), so each step loads, edits and
 * stores one block before the next is read, and nothing holds an entry's
 * address across a call that reloads the buffer.
 */
#include "core/errno.h"
#include "core/xfs/xfs_format.h"
#include "core/xfs/xfs_internal.h"

/* How many entries a node holds (the root included). */
static uint32_t capacity(const struct xfs_da_tree *t) {
    return (t->bsize - t->fs->da_hdr) / XFS_DA3_ENTRY_SIZE;
}

static uint8_t *entry(struct xfs *fs, uint32_t i) {
    return fs->dirblock + fs->da_hdr + i * XFS_DA3_ENTRY_SIZE;
}

static uint32_t count_of(const struct xfs *fs) {
    return xfs_be16(fs->dirblock + fs->da_count);
}

void xfs_da_header(struct xfs *fs, uint32_t bsize, uint16_t magic, uint64_t ino, uint32_t count,
                   uint64_t forw, uint64_t back) {
    for (uint32_t i = 0; i < bsize; i++) {
        fs->dirblock[i] = 0;
    }
    xfs_put_be32(fs->dirblock + XFS_DA3_FORW_OFF, (uint32_t)forw);
    xfs_put_be32(fs->dirblock + XFS_DA3_BACK_OFF, (uint32_t)back);
    xfs_put_be16(fs->dirblock + XFS_DA3_MAGIC_OFF, magic);
    xfs_put_be16(fs->dirblock + fs->da_count, (uint16_t)count);
    xfs_stamp_owner(fs, fs->dirblock, 0, XFS_DA3_UUID_OFF, XFS_DA3_OWNER_OFF, 0, ino);
}

int xfs_da_set_sibling(struct xfs_da_tree *t, uint64_t logical,
                       uint32_t off, uint32_t value) {
    struct xfs *fs = t->fs;
    uint64_t fsb = 0;
    int rc;
    if (logical == 0) {
        return KERR_OK;
    }
    rc = t->ops->load(t, logical, &fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_put_be32(fs->dirblock + off, value);
    xfs_reseal(fs, fs->dirblock, t->bsize, XFS_DA3_CRC_OFF);
    return t->ops->store(t, fsb);
}


/* ── finding a path ────────────────────────────────────────────────────── */

/* Loads the node at `logical`, checked: a node's magic, a count that fits
 * and is not zero, and the level the walk expects (any, for the root). */
static int load_node(struct xfs_da_tree *t, uint64_t logical,
                     uint32_t *level, uint64_t *fsb) {
    struct xfs *fs = t->fs;
    int rc = t->ops->load(t, logical, fsb);
    uint32_t n;
    if (rc != KERR_OK) {
        return rc;
    }
    n = count_of(fs);
    if (xfs_be16(fs->dirblock + XFS_DA3_MAGIC_OFF) != xfs_magic_for(fs, XFS_BLK_DA_NODE) ||
        n == 0 || n > capacity(t)) {
        return KERR_IO;
    }
    *level = xfs_be16(fs->dirblock + fs->da_level);
    return *level >= 1u && *level < XFS_DA3_MAX_LEVELS ? KERR_OK : KERR_IO;
}

/* The first entry whose hash is at or above `hash`; the last when none is,
 * because the last child takes everything above every key. */
static uint32_t first_at_or_above(struct xfs *fs, uint32_t hash) {
    uint32_t n = count_of(fs);
    for (uint32_t i = 0; i < n; i++) {
        if (xfs_be32(entry(fs, i)) >= hash) {
            return i;
        }
    }
    return n - 1u;
}

/* Sets level `d` of a path: the node, the entry taken, its count. */
static void put_level(struct xfs_da_path *p, uint32_t d, uint64_t node, uint32_t index,
                      uint32_t count) {
    p->node[d] = node;
    p->index[d] = index;
    p->count[d] = count;
}

/* Records level `d` of a path being walked down: it is the deepest yet. */
static void record(struct xfs_da_path *p, uint32_t d, uint64_t node, uint32_t index,
                   uint32_t count) {
    put_level(p, d, node, index, count);
    p->depth = d + 1u;
}

int xfs_da_descend(struct xfs_da_tree *t, uint32_t hash,
                   struct xfs_da_path *p) {
    struct xfs *fs = t->fs;
    uint64_t logical = t->root, fsb = 0;
    uint32_t level = 0, last = XFS_DA3_MAX_LEVELS;
    for (uint32_t d = 0; d < XFS_DA3_MAX_LEVELS; d++) {
        int rc = load_node(t, logical, &level, &fsb);
        uint32_t i;
        if (rc != KERR_OK || level >= last) {
            return rc != KERR_OK ? rc : KERR_IO; /* levels must fall by one */
        }
        i = first_at_or_above(fs, hash);
        record(p, d, logical, i, count_of(fs));
        logical = xfs_be32(entry(fs, i) + 4u);
        last = level;
        if (level == 1u) {
            p->leaf = logical;
            return KERR_OK;
        }
    }
    return KERR_IO;
}

/* Depth-first search below the node at `logical` for the leaf `target`,
 * trying only children whose range can hold `hash`: from the first whose
 * key reaches it, through the run of keys equal to it, and the one after —
 * a run of equal hashes can cross leaves, and the leaf a name's hash went
 * into is one of those. The node is reloaded for each entry, because the
 * search below it uses the buffer. */
static int find_under(struct xfs_da_tree *t, uint64_t logical,
                      uint32_t hash, uint64_t target, struct xfs_da_path *p, uint32_t d) {
    struct xfs *fs = t->fs;
    uint64_t fsb = 0;
    uint32_t level = 0, n, key;
    int rc = d < XFS_DA3_MAX_LEVELS ? load_node(t, logical, &level, &fsb) : KERR_IO;
    for (uint32_t i = rc == KERR_OK ? first_at_or_above(fs, hash) : 0; rc == KERR_OK;) {
        uint64_t child = xfs_be32(entry(fs, i) + 4u);
        n = count_of(fs);
        key = xfs_be32(entry(fs, i));
        record(p, d, logical, i, n);
        if (level == 1u && child == target) {
            p->leaf = target;
            return KERR_OK;
        }
        rc = level == 1u ? KERR_NOENT : find_under(t, child, hash, target, p, d + 1u);
        if (rc != KERR_NOENT || key > hash || ++i >= n) {
            return rc;
        }
        rc = load_node(t, logical, &level, &fsb);
    }
    return rc;
}

int xfs_da_find(struct xfs_da_tree *t, uint32_t hash, uint64_t leaf,
                struct xfs_da_path *p) {
    return find_under(t, t->root, hash, leaf, p, 0);
}

bool xfs_da_only_leaf(const struct xfs_da_path *p) {
    for (uint32_t d = 0; d < p->depth; d++) {
        if (p->count[d] != 1u) {
            return false;
        }
    }
    return true;
}

/* ── keys ──────────────────────────────────────────────────────────────── */

/* Sets the key of entry `index` in the node at `logical` to `hash`. */
static int set_key(struct xfs_da_tree *t, uint64_t logical,
                   uint32_t index, uint32_t hash) {
    struct xfs *fs = t->fs;
    uint64_t fsb = 0;
    uint32_t level = 0;
    int rc = load_node(t, logical, &level, &fsb);
    if (rc != KERR_OK || index >= count_of(fs)) {
        return rc != KERR_OK ? rc : KERR_IO;
    }
    xfs_put_be32(entry(fs, index), hash);
    xfs_reseal(fs, fs->dirblock, t->bsize, XFS_DA3_CRC_OFF);
    return t->ops->store(t, fsb);
}

/* The child at level `d` of the path now tops out at `hash`: its key in
 * the node above changes, and so on upward while the changed entry is the
 * last in its node — only then does the node's own highest hash move. */
static int keys_up(struct xfs_da_tree *t, const struct xfs_da_path *p,
                   uint32_t d, uint32_t hash) {
    int rc = KERR_OK;
    for (uint32_t k = d + 1u; rc == KERR_OK && k-- > 0;) {
        rc = set_key(t, p->node[k], p->index[k], hash);
        if (p->index[k] + 1u != p->count[k]) {
            break;
        }
    }
    return rc;
}

int xfs_da_set_top(struct xfs_da_tree *t, const struct xfs_da_path *p,
                   uint32_t hash) {
    return keys_up(t, p, p->depth - 1u, hash);
}

/* ── adding a child ───────────────────────────────────────────────────── */

static int add_at(struct xfs_da_tree *t, struct xfs_da_path *p, uint32_t d,
                  const struct xfs_da_split *s, uint64_t now_s);

/* A fresh block for a node, mapped into the directory past every index
 * block it has. Re-reads the inode: its fork changed. */
static int new_node_block(struct xfs_da_tree *t, uint64_t now_s, uint64_t *at,
                          uint64_t *fsb) {
    return t->ops->grow(t, at, fsb, now_s);
}

/* Writes a node at `fsb`: header, level and `n` entries copied from
 * `src` (which must not be the buffer). */
static int write_node(struct xfs_da_tree *t, uint64_t fsb,
                      const struct xfs_da_node_head *h, const uint8_t *src) {
    struct xfs *fs = t->fs;
    xfs_da_header(t->fs, t->bsize, xfs_magic_for(t->fs, XFS_BLK_DA_NODE), t->ip->ino, h->count, h->forw, h->back);
    xfs_put_be16(fs->dirblock + fs->da_level, (uint16_t)h->level);
    xfs_copy_bytes(fs->dirblock + fs->da_hdr, src, h->count * XFS_DA3_ENTRY_SIZE);
    xfs_reseal(fs, fs->dirblock, t->bsize, XFS_DA3_CRC_OFF);
    return t->ops->store(t, fsb);
}

/* The root is full: its entries move down into a new node of the root's
 * level, and the root becomes that node's parent, one level higher, with
 * the one entry naming it. The path gains a level under the root. */
static int grow_root(struct xfs_da_tree *t, struct xfs_da_path *p,
                     uint64_t now_s) {
    struct xfs *fs = t->fs;
    uint8_t *saved = fs->dirpairs;
    uint64_t at = 0, fsb = 0, root_fsb = 0;
    uint32_t level = 0, n;
    int rc = p->depth + 1u < XFS_DA3_MAX_LEVELS ? new_node_block(t, now_s, &at, &fsb)
                                                : KERR_NOSPC;
    rc = rc != KERR_OK ? rc : load_node(t, t->root, &level, &root_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    n = count_of(fs);
    xfs_copy_bytes(saved, fs->dirblock + fs->da_hdr, n * XFS_DA3_ENTRY_SIZE);
    rc = write_node(t, fsb, &(struct xfs_da_node_head){level, n, 0, 0}, saved);
    xfs_put_be32(saved, xfs_be32(saved + (n - 1u) * XFS_DA3_ENTRY_SIZE));
    xfs_put_be32(saved + 4u, (uint32_t)at);
    rc = rc != KERR_OK ? rc
                       : write_node(t, root_fsb,
                                    &(struct xfs_da_node_head){level + 1u, 1u, 0, 0}, saved);
    for (uint32_t d = p->depth; d > 0; d--) {
        put_level(p, d, p->node[d - 1u], p->index[d - 1u], p->count[d - 1u]);
    }
    p->depth++;
    p->node[1] = at;
    put_level(p, 0, t->root, 0, 1u);
    return rc;
}

/* Splits the full node at level `d` of the path: the upper half moves to a
 * new node after it on its level, the node above gains an entry for it,
 * and the path follows its own entry into whichever half now holds it.
 *
 * The path may gain a level while this runs (the parent was the full
 * root), so the node's own level is re-read from the depth afterwards.
 *
 * Runs to 30 lines: the two halves, both sibling links, the parent's two
 * keys and the path's correction are one change to three blocks that no
 * part of leaves consistent (Rule 11, Single derivation). */
static int split_node(struct xfs_da_tree *t, struct xfs_da_path *p, uint32_t d,
                      uint64_t now_s) {
    struct xfs *fs = t->fs;
    uint8_t *saved = fs->dirpairs;
    uint64_t at = 0, fsb = 0, old_fsb = 0, forw;
    uint32_t level = 0, n, half, before;
    struct xfs_da_split up;
    int rc = new_node_block(t, now_s, &at, &fsb);
    rc = rc != KERR_OK ? rc : load_node(t, p->node[d], &level, &old_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    n = count_of(fs);
    half = n / 2u;
    forw = xfs_be32(fs->dirblock + XFS_DA3_FORW_OFF);
    xfs_copy_bytes(saved, fs->dirblock + fs->da_hdr, n * XFS_DA3_ENTRY_SIZE);
    xfs_put_be16(fs->dirblock + fs->da_count, (uint16_t)half);
    xfs_put_be32(fs->dirblock + XFS_DA3_FORW_OFF, (uint32_t)at);
    xfs_reseal(fs, fs->dirblock, t->bsize, XFS_DA3_CRC_OFF);
    rc = t->ops->store(t, old_fsb);
    rc = rc != KERR_OK ? rc
                       : write_node(t, fsb,
                                    &(struct xfs_da_node_head){level, n - half, forw, p->node[d]},
                                    saved + half * XFS_DA3_ENTRY_SIZE);
    rc = rc != KERR_OK ? rc : xfs_da_set_sibling(t, forw, XFS_DA3_BACK_OFF, (uint32_t)at);
    up = (struct xfs_da_split){.lower = xfs_be32(saved + (half - 1u) * XFS_DA3_ENTRY_SIZE),
                               .child = at,
                               .upper = xfs_be32(saved + (n - 1u) * XFS_DA3_ENTRY_SIZE)};
    before = p->depth;
    rc = rc != KERR_OK ? rc : add_at(t, p, d - 1u, &up, now_s);
    d += p->depth - before; /* the root grew a level above: this node is one deeper */
    if (rc == KERR_OK && p->index[d] >= half) {
        put_level(p, d, at, p->index[d] - half, n - half);
        p->index[d - 1u]++;
    } else if (rc == KERR_OK) {
        p->count[d] = half;
    }
    return rc;
}

/* Makes room in the node at level `*d` of the path, which is full: the
 * root grows a level first, so that every full node splits the same way.
 * `*d` follows the node down when the path gains a level above it. */
static int make_room(struct xfs_da_tree *t, struct xfs_da_path *p, uint32_t *d,
                     uint64_t now_s) {
    uint32_t before;
    int rc = KERR_OK;
    if (*d == 0) {
        rc = grow_root(t, p, now_s);
        *d = 1u;
    }
    before = p->depth;
    rc = rc != KERR_OK ? rc : split_node(t, p, *d, now_s);
    *d += p->depth - before;
    return rc;
}

/* In the node at level `d` of the path: the entry the path took gets key
 * `s->lower`, and `{s->upper, s->child}` goes in after it. When that new
 * entry is the node's last, the node's highest hash rose: keys above it
 * follow. The node is split first if it is full. */
static int add_at(struct xfs_da_tree *t, struct xfs_da_path *p, uint32_t d,
                  const struct xfs_da_split *s, uint64_t now_s) {
    struct xfs *fs = t->fs;
    uint64_t fsb = 0;
    uint32_t level = 0, n, i;
    int rc = p->count[d] >= capacity(t) ? make_room(t, p, &d, now_s) : KERR_OK;
    rc = rc != KERR_OK ? rc : load_node(t, p->node[d], &level, &fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    n = count_of(fs);
    i = p->index[d];
    for (uint32_t k = n; k > i + 1u; k--) {
        xfs_copy_bytes(entry(fs, k), entry(fs, k - 1u), XFS_DA3_ENTRY_SIZE);
    }
    xfs_put_be32(entry(fs, i), s->lower);
    xfs_put_be32(entry(fs, i + 1u), s->upper);
    xfs_put_be32(entry(fs, i + 1u) + 4u, (uint32_t)s->child);
    xfs_put_be16(fs->dirblock + fs->da_count, (uint16_t)(n + 1u));
    xfs_reseal(fs, fs->dirblock, t->bsize, XFS_DA3_CRC_OFF);
    rc = t->ops->store(t, fsb);
    p->count[d] = n + 1u;
    return rc == KERR_OK && d > 0 && i + 2u == n + 1u ? keys_up(t, p, d - 1u, s->upper)
                                                      : rc;
}

int xfs_da_add_child(struct xfs_da_tree *t, struct xfs_da_path *p,
                     const struct xfs_da_split *s, uint64_t now_s) {
    return add_at(t, p, p->depth - 1u, s, now_s);
}

/* ── dropping a child ─────────────────────────────────────────────────── */

/* A node left with no entries leaves the tree: out of its level's chain,
 * out of the fork. The node is in the buffer. */
static int drop_node(struct xfs_da_tree *t, uint64_t logical, uint64_t now_s) {
    struct xfs *fs = t->fs;
    uint32_t forw = xfs_be32(fs->dirblock + XFS_DA3_FORW_OFF);
    uint32_t back = xfs_be32(fs->dirblock + XFS_DA3_BACK_OFF);
    int rc = xfs_da_set_sibling(t, back, XFS_DA3_FORW_OFF, forw);
    rc = rc != KERR_OK ? rc : xfs_da_set_sibling(t, forw, XFS_DA3_BACK_OFF, back);
    return rc != KERR_OK ? rc : t->ops->shrink(t, logical, now_s);
}

/* Takes the path's entry out of the node at level `d`. A node it empties
 * goes too, and its own entry above it; a node whose last entry went has a
 * lower highest hash, which the keys above follow. The root is never
 * emptied: the caller keeps a directory's last leaf. */
static int drop_at(struct xfs_da_tree *t, struct xfs_da_path *p, uint32_t d,
                   uint64_t now_s) {
    struct xfs *fs = t->fs;
    uint64_t fsb = 0;
    uint32_t level = 0, n, i = p->index[d];
    int rc = load_node(t, p->node[d], &level, &fsb);
    if (rc != KERR_OK || i >= (n = count_of(fs))) {
        return rc != KERR_OK ? rc : KERR_IO;
    }
    if (n == 1u && d == 0) {
        return KERR_IO; /* the directory's last leaf is kept by the caller */
    }
    if (n == 1u) {
        rc = drop_node(t, p->node[d], now_s);
        return rc != KERR_OK ? rc : drop_at(t, p, d - 1u, now_s);
    }
    for (uint32_t k = i; k + 1u < n; k++) {
        xfs_copy_bytes(entry(fs, k), entry(fs, k + 1u), XFS_DA3_ENTRY_SIZE);
    }
    xfs_put_be16(fs->dirblock + fs->da_count, (uint16_t)(n - 1u));
    xfs_reseal(fs, fs->dirblock, t->bsize, XFS_DA3_CRC_OFF);
    rc = t->ops->store(t, fsb);
    p->count[d] = n - 1u;
    if (rc != KERR_OK || i + 1u != n || d == 0) {
        return rc;
    }
    return keys_up(t, p, d - 1u, xfs_be32(entry(fs, n - 2u)));
}

int xfs_da_drop_child(struct xfs_da_tree *t, struct xfs_da_path *p,
                      uint64_t now_s) {
    return drop_at(t, p, p->depth - 1u, now_s);
}
