/* A data fork's extent map, whatever shape the inode keeps it in.
 *
 * **Why this exists.** The writers kept the map as an array inside the
 * inode and stopped when it was full: twenty-one extents in a 512-byte
 * inode, nine in a 256-byte one. On a fragmented filesystem a file that
 * grows one block at a time can need an extent per block, and so stopped
 * growing at a few dozen kilobytes. Linux moves such a map into a b-tree —
 * the format called `BTREE`, with the root in the inode and the records in
 * leaf blocks — and this does the same.
 *
 * It does so by **loading the whole map, editing it as an array, and
 * storing it back in whichever form fits**: in the inode while it fits
 * there, and otherwise as a tree of the least height whose root fits the
 * inode — leaves of records, then as many levels of nodes as it takes —
 * each level filled evenly so every non-root block is at least half full,
 * as `xfs_repair` requires. The blocks a tree had are reused for the tree
 * it becomes, so a file that grows a block at a time does not allocate and
 * free tree blocks with every block; only a change in how many are needed
 * takes or gives one back, and each goes in and out of the reverse map as
 * the file's tree block.
 *
 * A map larger than `XFS_FORK_MAP_MAX` extents, or a tree of more than
 * `XFS_FORK_BLOCKS_MAX` blocks or `XFS_FORK_LEVELS_MAX` levels, is refused
 * with KERR_NOSPC. */
#include "core/bcache.h"
#include "core/errno.h"
#include "core/xfs/xfs_internal.h"

#define NULL_FSB 0xffffffffffffffffull

/* The map being edited, the tree blocks it was loaded from (any level, in
 * no particular order: they are a pool to reuse), and per level of the tree
 * being stored, each block's first file offset. Static: one core is inside
 * the filesystem at a time, nothing here sleeps, and a map of this size is
 * far too large for a kernel stack (CLAUDE.md §10). */
static struct xfs_extent map[XFS_FORK_MAP_MAX];
static uint64_t blocks[XFS_FORK_BLOCKS_MAX];
static uint64_t keys[XFS_FORK_BLOCKS_MAX];
static uint32_t block_count;

/* How many blocks each level of a tree has, leaves first, and how many
 * levels below the root. */
struct tree_shape {
    uint32_t width[XFS_FORK_LEVELS_MAX];
    uint32_t levels;
    uint32_t total;
};

/* Where a fork lives in the on-disk inode and how it is counted: the data
 * fork from the end of the core to `forkoff`, the attribute fork from
 * `forkoff` to the end of the inode, each with its own format byte and
 * extent count, and its tree blocks marked as which fork's in the reverse
 * map. */
struct fork_view {
    uint32_t off, bytes, format_at, cache_max;
    uint64_t tree_rmap;
    bool attr;
};

static struct fork_view view_of(const struct xfs *fs, const uint8_t *di, enum xfs_fork_which w) {
    uint32_t split = (uint32_t)di[XFS_DI_FORKOFF] * 8u;
    if (w == XFS_ATTR_FORK) {
        return (struct fork_view){fs->dinode_size + split, fs->inodesize - fs->dinode_size - split,
                                  XFS_DI_AFORMAT, XFS_AFORK_MAX,
                                  XFS_RMAP_OFF_BMBT | XFS_RMAP_OFF_ATTR, true};
    }
    return (struct fork_view){fs->dinode_size, xfs_fork_bytes(fs, di), XFS_DI_FORMAT,
                              XFS_LITERAL_MAX, XFS_RMAP_OFF_BMBT, false};
}

static uint64_t extents_of(const struct xfs *fs, const uint8_t *di, const struct fork_view *v) {
    return v->attr ? xfs_dinode_attr_extents(fs, di) : xfs_dinode_data_extents(fs, di);
}

static void set_extents(const struct xfs *fs, uint8_t *di, const struct fork_view *v, uint32_t n) {
    if (v->attr) {
        xfs_dinode_set_attr_extents(fs, di, n);
    } else {
        xfs_dinode_set_data_extents(fs, di, n);
    }
}

uint32_t xfs_bmbt_hdr(const struct xfs *fs) {
    return fs->v5 ? XFS_BMBT_BLOCK_BYTES : XFS_BMBT_V4_BLOCK_BYTES;
}

static uint32_t leaf_capacity(const struct xfs *fs) {
    return (fs->blocksize - xfs_bmbt_hdr(fs)) / XFS_EXTENT_BYTES;
}

/* Key/pointer pairs one node block holds; also where its pointers start. */
static uint32_t node_capacity(const struct xfs *fs) {
    return (fs->blocksize - xfs_bmbt_hdr(fs)) / (XFS_BMBT_KEY_BYTES + XFS_BMBT_PTR_BYTES);
}

/* How many key/pointer pairs the inode's root can hold in `bytes`. */
static uint32_t root_capacity(uint32_t bytes) {
    return bytes > XFS_BMDR_HDR_BYTES
               ? (bytes - XFS_BMDR_HDR_BYTES) / (XFS_BMBT_KEY_BYTES + XFS_BMBT_PTR_BYTES)
               : 0;
}

/* ── loading ──────────────────────────────────────────────────────────── */

/* Appends one leaf's records to the map. */
static int load_records(struct xfs *fs, const uint8_t *data, uint32_t n, uint32_t *count) {
    if (n > leaf_capacity(fs) || *count + n > XFS_FORK_MAP_MAX) {
        return KERR_NOSPC;
    }
    for (uint32_t i = 0; i < n; i++) {
        xfs_extent_unpack(data + xfs_bmbt_hdr(fs) + i * XFS_EXTENT_BYTES, &map[(*count)++]);
    }
    return KERR_OK;
}

static int load_block(struct xfs *fs, uint64_t fsb, uint32_t level, uint32_t *count);

/* Descends into each child a node names, in order. */
static int load_children(struct xfs *fs, const uint8_t *data, uint32_t n, uint32_t level,
                         uint32_t *count) {
    const uint8_t *ptrs = data + xfs_bmbt_hdr(fs) + node_capacity(fs) * XFS_BMBT_KEY_BYTES;
    int rc = n > 0 && n <= node_capacity(fs) ? KERR_OK : KERR_IO;
    for (uint32_t i = 0; rc == KERR_OK && i < n; i++) {
        rc = load_block(fs, xfs_be64(ptrs + i * XFS_BMBT_PTR_BYTES), level - 1u, count);
    }
    return rc;
}

/* Loads the subtree at `fsb`, which must be a block of `level`: its records
 * when a leaf, its children's when a node. The block is held while its
 * children load, so the cache holds one block per level at most. */
static int load_block(struct xfs *fs, uint64_t fsb, uint32_t level, uint32_t *count) {
    struct bbuf *b;
    uint32_t n;
    int rc = xfs_fsb_valid(fs, fsb, 1) && block_count < XFS_FORK_BLOCKS_MAX
                 ? bcache_get(fs->cache, xfs_fsb_to_block(fs, fsb), &b)
                 : KERR_NOSPC;
    if (rc != KERR_OK) {
        return rc;
    }
    blocks[block_count++] = fsb;
    n = xfs_be16(b->data + 6u);
    if (xfs_be32(b->data) != xfs_magic_for(fs, XFS_BLK_BMBT) || xfs_be16(b->data + 4u) != level) {
        rc = KERR_IO;
    } else if (level == 0) {
        rc = load_records(fs, b->data, n, count);
    } else {
        rc = load_children(fs, b->data, n, level, count);
    }
    bcache_put(fs->cache, b);
    return rc;
}

/* The tree's records in order, from the root in the inode down. */
static int load_tree(struct xfs *fs, const uint8_t *fork, uint32_t bytes, uint32_t *count) {
    uint32_t level = xfs_be16(fork), n = xfs_be16(fork + 2u), mx = root_capacity(bytes);
    int rc = level >= 1u && level <= XFS_FORK_LEVELS_MAX && n > 0 && n <= mx ? KERR_OK
                                                                           : KERR_NOSPC;
    for (uint32_t i = 0; rc == KERR_OK && i < n; i++) {
        uint64_t fsb = xfs_be64(fork + XFS_BMDR_HDR_BYTES + mx * XFS_BMBT_KEY_BYTES +
                                i * XFS_BMBT_PTR_BYTES);
        rc = load_block(fs, fsb, level - 1u, count);
    }
    return rc;
}

int xfs_fork_load_which(struct xfs *fs, const uint8_t *di, enum xfs_fork_which w,
                        struct xfs_fork_map *out) {
    struct fork_view v = view_of(fs, di, w);
    const uint8_t *fork = di + v.off;
    uint32_t bytes = v.bytes, n = 0;
    int rc = KERR_OK;
    block_count = 0;
    if (di[v.format_at] == XFS_DINODE_FMT_EXTENTS) {
        n = (uint32_t)extents_of(fs, di, &v);
        rc = n * XFS_EXTENT_BYTES <= bytes && n <= XFS_FORK_MAP_MAX ? KERR_OK : KERR_IO;
        for (uint32_t i = 0; rc == KERR_OK && i < n; i++) {
            xfs_extent_unpack(fork + i * XFS_EXTENT_BYTES, &map[i]);
        }
    } else if (di[v.format_at] == XFS_DINODE_FMT_BTREE) {
        rc = load_tree(fs, fork, bytes, &n);
    } else {
        rc = KERR_NOTSUP; /* a fork held in the inode itself: not an extent map */
    }
    *out = (struct xfs_fork_map){.e = map, .count = n, .cap = XFS_FORK_MAP_MAX};
    return rc;
}

int xfs_fork_load(struct xfs *fs, const uint8_t *di, struct xfs_fork_map *out) {
    return xfs_fork_load_which(fs, di, XFS_DATA_FORK, out);
}

/* ── editing ──────────────────────────────────────────────────────────── */

/* Whether `b` continues `a` on disk and in the file, and may join it. */
static bool joins(const struct xfs_extent *a, const struct xfs_extent *b) {
    return a->startoff + a->blockcount == b->startoff &&
           a->startblock + a->blockcount == b->startblock && a->unwritten == b->unwritten &&
           (uint64_t)a->blockcount + b->blockcount <= XFS_MAX_EXTLEN;
}

int xfs_fork_map_add(struct xfs_fork_map *m, const struct xfs_extent *e) {
    uint32_t at = m->count;
    for (uint32_t i = 0; i < m->count; i++) {
        if (m->e[i].startoff > e->startoff) {
            at = i;
            break;
        }
    }
    if (at > 0 && joins(&m->e[at - 1u], e)) {
        m->e[at - 1u].blockcount += e->blockcount;
        if (at < m->count && joins(&m->e[at - 1u], &m->e[at])) {
            m->e[at - 1u].blockcount += m->e[at].blockcount;
            xfs_fork_map_drop(m, at);
        }
        return KERR_OK;
    }
    if (at < m->count && joins(e, &m->e[at])) {
        m->e[at].startoff = e->startoff;
        m->e[at].startblock = e->startblock;
        m->e[at].blockcount += e->blockcount;
        return KERR_OK;
    }
    if (m->count >= m->cap) {
        return KERR_NOSPC;
    }
    for (uint32_t i = m->count; i > at; i--) {
        m->e[i] = m->e[i - 1u];
    }
    m->e[at] = *e;
    m->count++;
    return KERR_OK;
}

/* Takes [logical, logical+len) out of extent `i` — all of it, its head, its
 * tail or its middle, the last leaving an extent each side. */
static int cut_extent(struct xfs_fork_map *m, uint32_t i, uint64_t logical, uint32_t len) {
    struct xfs_extent *e = &m->e[i], tail = *e;
    uint64_t end = e->startoff + e->blockcount;
    if (logical == e->startoff && len == e->blockcount) {
        xfs_fork_map_drop(m, i);
        return KERR_OK;
    }
    tail.startoff = logical + len;
    tail.startblock = e->startblock + (logical + len - e->startoff);
    tail.blockcount = (uint32_t)(end - (logical + len));
    if (logical == e->startoff) {
        *e = tail;
        return KERR_OK;
    }
    if (tail.blockcount > 0 && m->count >= m->cap) {
        return KERR_NOSPC;
    }
    e->blockcount = (uint32_t)(logical - e->startoff);
    if (tail.blockcount > 0) {
        for (uint32_t j = m->count; j > i + 1u; j--) {
            m->e[j] = m->e[j - 1u];
        }
        m->e[i + 1u] = tail;
        m->count++;
    }
    return KERR_OK;
}

int xfs_fork_map_cut(struct xfs_fork_map *m, uint64_t logical, uint32_t len, uint64_t *fsb) {
    for (uint32_t i = 0; i < m->count; i++) {
        const struct xfs_extent *e = &m->e[i];
        if (logical >= e->startoff && logical + len <= e->startoff + e->blockcount) {
            *fsb = e->startblock + (logical - e->startoff);
            return cut_extent(m, i, logical, len);
        }
    }
    return KERR_IO; /* nothing maps it: not the block the caller thinks it is */
}

void xfs_fork_map_drop(struct xfs_fork_map *m, uint32_t i) {
    for (uint32_t j = i; j + 1u < m->count; j++) {
        m->e[j] = m->e[j + 1u];
    }
    m->count--;
}

/* ── storing ──────────────────────────────────────────────────────────── */

/* Writes the map into the inode's literal area as a plain extent list. */
static void store_extents(uint8_t *di, const struct fork_view *v, const struct xfs_fork_map *m) {
    uint8_t *fork = di + v->off;
    for (uint32_t i = 0; i < v->bytes; i++) {
        fork[i] = 0;
    }
    for (uint32_t i = 0; i < m->count; i++) {
        xfs_extent_pack(fork + i * XFS_EXTENT_BYTES, &m->e[i]);
    }
    di[v->format_at] = XFS_DINODE_FMT_EXTENTS;
}

/* The tree of least height for `count` records under a root of `root_cap`
 * pairs: leaves, then levels of nodes until one level fits the root. */
static int plan_tree(const struct xfs *fs, uint32_t count, uint32_t root_cap,
                     struct tree_shape *t) {
    uint32_t w = (count + leaf_capacity(fs) - 1u) / leaf_capacity(fs);
    *t = (struct tree_shape){.levels = 0};
    while (t->levels < XFS_FORK_LEVELS_MAX) {
        t->width[t->levels++] = w;
        t->total += w;
        if (w <= root_cap) {
            return t->total <= XFS_FORK_BLOCKS_MAX ? KERR_OK : KERR_NOSPC;
        }
        w = (w + node_capacity(fs) - 1u) / node_capacity(fs);
    }
    return KERR_NOSPC;
}

/* Makes the tree's blocks number `want`: takes new ones near the inode and
 * gives back ones no longer needed, each through the reverse map as this
 * inode's tree block. *delta is how many the inode gained (or lost). */
static int fit_blocks(struct xfs *fs, uint64_t ino, uint64_t tree_rmap, uint32_t want,
                      int64_t *delta) {
    int rc = KERR_OK;
    *delta = 0;
    while (rc == KERR_OK && block_count < want) {
        uint32_t agno = xfs_fsb_ag(fs, ino >> fs->inopblog), got = 0;
        rc = xfs_alloc_somewhere(fs, &agno, 1u, 0, &got);
        if (rc == KERR_OK) {
            blocks[block_count] = ((uint64_t)agno << fs->agblklog) | got;
            rc = xfs_rmap_map(fs, blocks[block_count], 1u, ino, tree_rmap);
            block_count++;
            (*delta)++;
        }
    }
    while (rc == KERR_OK && block_count > want) {
        block_count--;
        rc = xfs_free_owned(fs, blocks[block_count], 1u, ino, tree_rmap);
        (*delta)--;
    }
    return rc;
}

/* One block of the tree being written: where it is, its level, how many
 * entries it holds, and its siblings on that level. */
struct tree_block {
    uint64_t fsb, left, right;
    uint32_t level, n;
};

/* Takes the block, zeroed, with its header written; the caller fills the
 * entries and hands it to finish_block. */
static int start_block(struct xfs *fs, uint64_t ino, const struct tree_block *tb,
                       struct bbuf **out) {
    int rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, tb->fsb), out);
    uint8_t *d;
    if (rc != KERR_OK) {
        return rc;
    }
    d = (*out)->data;
    for (uint32_t i = 0; i < fs->blocksize; i++) {
        d[i] = 0;
    }
    xfs_put_be32(d, xfs_magic_for(fs, XFS_BLK_BMBT));
    xfs_put_be16(d + 4u, (uint16_t)tb->level);
    xfs_put_be16(d + 6u, (uint16_t)tb->n);
    xfs_put_be64(d + 8u, tb->left);
    xfs_put_be64(d + 16u, tb->right);
    if (fs->v5) {
        xfs_put_be64(d + 24u, xfs_fsb_to_daddr(fs, tb->fsb));
        xfs_copy_bytes(d + XFS_BMBT_UUID_OFF, fs->uuid, 16u);
        xfs_put_be64(d + 56u, ino);
    }
    return KERR_OK;
}

static void finish_block(struct xfs *fs, struct bbuf *b) {
    xfs_reseal(fs, b->data, fs->blocksize, XFS_BMBT_CRC_OFF);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
}

/* A leaf holding records [first, first+n). */
static int write_leaf(struct xfs *fs, uint64_t ino, const struct tree_block *tb,
                      const struct xfs_fork_map *m, uint32_t first) {
    struct bbuf *b;
    int rc = start_block(fs, ino, tb, &b);
    for (uint32_t i = 0; rc == KERR_OK && i < tb->n; i++) {
        xfs_extent_pack(b->data + xfs_bmbt_hdr(fs) + i * XFS_EXTENT_BYTES, &m->e[first + i]);
    }
    if (rc == KERR_OK) {
        finish_block(fs, b);
    }
    return rc;
}

/* A node naming children [first, first+n) of the level below, whose blocks
 * start at blocks[child_base] and whose first offsets are keys[]. */
static int write_node(struct xfs *fs, uint64_t ino, const struct tree_block *tb,
                      uint32_t child_base, uint32_t first) {
    uint32_t hdr = xfs_bmbt_hdr(fs), ptrs = hdr + node_capacity(fs) * XFS_BMBT_KEY_BYTES;
    struct bbuf *b;
    int rc = start_block(fs, ino, tb, &b);
    for (uint32_t i = 0; rc == KERR_OK && i < tb->n; i++) {
        xfs_put_be64(b->data + hdr + i * XFS_BMBT_KEY_BYTES, keys[first + i]);
        xfs_put_be64(b->data + ptrs + i * XFS_BMBT_PTR_BYTES, blocks[child_base + first + i]);
    }
    if (rc == KERR_OK) {
        finish_block(fs, b);
    }
    return rc;
}

/* Writes level `l` of the tree, whose blocks are blocks[base...]: the
 * entries below spread evenly over them, and keys[j] left as block j's
 * first file offset for the level above. keys[] is rewritten in place,
 * which is safe because block j's first child is never before child j. */
static int store_level(struct xfs *fs, uint64_t ino, const struct xfs_fork_map *m,
                       const struct tree_shape *t, uint32_t l, uint32_t base) {
    uint32_t width = t->width[l], below = l == 0 ? m->count : t->width[l - 1u], at = 0;
    int rc = KERR_OK;
    for (uint32_t j = 0; rc == KERR_OK && j < width; j++) {
        struct tree_block tb = {.fsb = blocks[base + j], .level = l,
                                .n = (below - at) / (width - j),
                                .left = j > 0 ? blocks[base + j - 1u] : NULL_FSB,
                                .right = j + 1u < width ? blocks[base + j + 1u] : NULL_FSB};
        uint64_t first = l == 0 ? m->e[at].startoff : keys[at];
        rc = l == 0 ? write_leaf(fs, ino, &tb, m, at)
                    : write_node(fs, ino, &tb, base - below, at);
        keys[j] = first;
        at += tb.n;
    }
    return rc;
}

/* The root in the inode, naming the top level's blocks (blocks[top...]). */
static void store_root(uint8_t *di, const struct fork_view *v, const struct tree_shape *t,
                       uint32_t top) {
    uint8_t *fork = di + v->off;
    uint32_t mx = root_capacity(v->bytes), n = t->width[t->levels - 1u];
    for (uint32_t i = 0; i < v->bytes; i++) {
        fork[i] = 0;
    }
    xfs_put_be16(fork, (uint16_t)t->levels);
    xfs_put_be16(fork + 2u, (uint16_t)n);
    for (uint32_t i = 0; i < n; i++) {
        xfs_put_be64(fork + XFS_BMDR_HDR_BYTES + i * XFS_BMBT_KEY_BYTES, keys[i]);
        xfs_put_be64(fork + XFS_BMDR_HDR_BYTES + mx * XFS_BMBT_KEY_BYTES + i * XFS_BMBT_PTR_BYTES,
                     blocks[top + i]);
    }
    di[v->format_at] = XFS_DINODE_FMT_BTREE;
}

/* Writes the map as the tree `t`, leaves first, from the pool's blocks in
 * order: the leaves take the first ones, each node level the next. */
static int store_tree(struct xfs *fs, uint64_t ino, uint8_t *di, const struct fork_view *v,
                      const struct xfs_fork_map *m, const struct tree_shape *t) {
    uint32_t base = 0;
    int rc = KERR_OK;
    for (uint32_t l = 0; rc == KERR_OK && l < t->levels; l++) {
        rc = store_level(fs, ino, m, t, l, base);
        base += t->width[l];
    }
    if (rc == KERR_OK) {
        store_root(di, v, t, base - t->width[t->levels - 1u]);
    }
    return rc;
}

int xfs_fork_store_which(struct xfs *fs, uint64_t ino, uint8_t *di, enum xfs_fork_which w,
                         const struct xfs_fork_map *m) {
    struct fork_view v = view_of(fs, di, w);
    bool fits = m->count * XFS_EXTENT_BYTES <= v.bytes;
    struct tree_shape t = {.levels = 0};
    int64_t delta = 0;
    int rc = fits ? KERR_OK : plan_tree(fs, m->count, root_capacity(v.bytes), &t);
    if (rc != KERR_OK) {
        return rc;
    }
    if (v.bytes > v.cache_max && (!fits || m->count * XFS_EXTENT_BYTES > v.cache_max)) {
        return KERR_NOTSUP; /* the last guard: this reader would refuse the result */
    }
    rc = fit_blocks(fs, ino, v.tree_rmap, t.total, &delta);
    if (rc != KERR_OK) {
        return rc;
    }
    if (fits) {
        store_extents(di, &v, m);
    } else {
        rc = store_tree(fs, ino, di, &v, m, &t);
    }
    set_extents(fs, di, &v, m->count);
    xfs_put_be64(di + XFS_DI_NBLOCKS, xfs_be64(di + XFS_DI_NBLOCKS) + (uint64_t)delta);
    return rc;
}

int xfs_fork_store(struct xfs *fs, uint64_t ino, uint8_t *di, const struct xfs_fork_map *m) {
    return xfs_fork_store_which(fs, ino, di, XFS_DATA_FORK, m);
}
