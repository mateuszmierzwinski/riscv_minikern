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
 * there, and as a two-level tree when it does not — a root in the inode
 * naming leaves on disk, filled evenly so every leaf is at least half full,
 * as `xfs_repair` requires of a tree's non-root blocks. The leaves a map
 * had are reused for the map it becomes, so a file that grows a block at a
 * time does not allocate and free tree blocks with every block; only a
 * change in how many leaves are needed takes or gives one back, and each
 * goes in and out of the reverse map as the file's tree block.
 *
 * A map larger than `XFS_FORK_MAP_MAX` extents, or one needing more leaves
 * than the inode's root can name, is refused with KERR_NOSPC — a
 * three-level tree is not written. Read, any depth is (xfs_bmbt.c). */
#include "core/bcache.h"
#include "core/errno.h"
#include "core/xfs/xfs_internal.h"

#define NULL_FSB 0xffffffffffffffffull

/* The map being edited, and the leaves it was loaded from. Static: one
 * core is inside the filesystem at a time, nothing here sleeps, and a map
 * of this size is far too large for a kernel stack (CLAUDE.md §10). */
static struct xfs_extent map[XFS_FORK_MAP_MAX];
static uint64_t leaves[XFS_FORK_LEAVES_MAX];
static uint32_t leaf_count;

uint32_t xfs_bmbt_hdr(const struct xfs *fs) {
    return fs->v5 ? XFS_BMBT_BLOCK_BYTES : XFS_BMBT_V4_BLOCK_BYTES;
}

static uint32_t leaf_capacity(const struct xfs *fs) {
    return (fs->blocksize - xfs_bmbt_hdr(fs)) / XFS_EXTENT_BYTES;
}

/* How many key/pointer pairs the inode's root can hold in `bytes`. */
static uint32_t root_capacity(uint32_t bytes) {
    return bytes > XFS_BMDR_HDR_BYTES
               ? (bytes - XFS_BMDR_HDR_BYTES) / (XFS_BMBT_KEY_BYTES + XFS_BMBT_PTR_BYTES)
               : 0;
}

/* ── loading ──────────────────────────────────────────────────────────── */

/* Appends one leaf's records to the map. */
static int load_leaf(struct xfs *fs, uint64_t fsb, uint32_t *count) {
    struct bbuf *b;
    uint32_t n;
    int rc = xfs_fsb_valid(fs, fsb, 1) ? bcache_get(fs->cache, xfs_fsb_to_block(fs, fsb), &b)
                                       : KERR_IO;
    if (rc != KERR_OK) {
        return rc;
    }
    n = xfs_be16(b->data + 6u);
    rc = xfs_be32(b->data) != xfs_magic_for(fs, XFS_BLK_BMBT) || xfs_be16(b->data + 4u) != 0 ||
                 n > leaf_capacity(fs) || *count + n > XFS_FORK_MAP_MAX
             ? KERR_NOSPC
             : KERR_OK;
    for (uint32_t i = 0; rc == KERR_OK && i < n; i++) {
        xfs_extent_unpack(b->data + xfs_bmbt_hdr(fs) + i * XFS_EXTENT_BYTES, &map[(*count)++]);
    }
    bcache_put(fs->cache, b);
    return rc;
}

/* A two-level tree's leaves, in order, from the root in the inode. Taller
 * trees — written by Linux for maps of thousands of extents — are refused
 * for writing; they are read as any other. */
static int load_tree(struct xfs *fs, const uint8_t *fork, uint32_t bytes, uint32_t *count) {
    uint32_t n = xfs_be16(fork + 2u), mx = root_capacity(bytes);
    int rc = xfs_be16(fork) == 1u && n > 0 && n <= mx && n <= XFS_FORK_LEAVES_MAX ? KERR_OK
                                                                                  : KERR_NOSPC;
    leaf_count = 0;
    for (uint32_t i = 0; rc == KERR_OK && i < n; i++) {
        uint64_t fsb = xfs_be64(fork + XFS_BMDR_HDR_BYTES + mx * XFS_BMBT_KEY_BYTES +
                                i * XFS_BMBT_PTR_BYTES);
        leaves[leaf_count++] = fsb;
        rc = load_leaf(fs, fsb, count);
    }
    return rc;
}

int xfs_fork_load(struct xfs *fs, const uint8_t *di, struct xfs_fork_map *out) {
    const uint8_t *fork = di + fs->dinode_size;
    uint32_t bytes = xfs_fork_bytes(fs, di), n = 0;
    int rc = KERR_OK;
    leaf_count = 0;
    if (di[XFS_DI_FORMAT] == XFS_DINODE_FMT_EXTENTS) {
        n = (uint32_t)xfs_dinode_data_extents(fs, di);
        rc = n * XFS_EXTENT_BYTES <= bytes && n <= XFS_FORK_MAP_MAX ? KERR_OK : KERR_IO;
        for (uint32_t i = 0; rc == KERR_OK && i < n; i++) {
            xfs_extent_unpack(fork + i * XFS_EXTENT_BYTES, &map[i]);
        }
    } else if (di[XFS_DI_FORMAT] == XFS_DINODE_FMT_BTREE) {
        rc = load_tree(fs, fork, bytes, &n);
    } else {
        rc = KERR_NOTSUP; /* a fork held in the inode itself: not an extent map */
    }
    *out = (struct xfs_fork_map){.e = map, .count = n, .cap = XFS_FORK_MAP_MAX};
    return rc;
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
static void store_extents(struct xfs *fs, uint8_t *di, const struct xfs_fork_map *m) {
    uint8_t *fork = di + fs->dinode_size;
    for (uint32_t i = 0; i < xfs_fork_bytes(fs, di); i++) {
        fork[i] = 0;
    }
    for (uint32_t i = 0; i < m->count; i++) {
        xfs_extent_pack(fork + i * XFS_EXTENT_BYTES, &m->e[i]);
    }
    di[XFS_DI_FORMAT] = XFS_DINODE_FMT_EXTENTS;
}

/* Writes records [first, first+n) into the leaf at `fsb`, between its
 * siblings. */
static int write_leaf(struct xfs *fs, uint64_t ino, uint64_t fsb, const struct xfs_fork_map *m,
                      uint32_t first, uint32_t n, uint64_t left, uint64_t right) {
    struct bbuf *b;
    int rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, fsb), &b);
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint32_t i = 0; i < fs->blocksize; i++) {
        b->data[i] = 0;
    }
    xfs_put_be32(b->data, xfs_magic_for(fs, XFS_BLK_BMBT));
    xfs_put_be16(b->data + 6u, (uint16_t)n);
    xfs_put_be64(b->data + 8u, left);
    xfs_put_be64(b->data + 16u, right);
    if (fs->v5) {
        xfs_put_be64(b->data + 24u, xfs_fsb_to_daddr(fs, fsb));
        xfs_copy_bytes(b->data + XFS_BMBT_UUID_OFF, fs->uuid, 16u);
        xfs_put_be64(b->data + 56u, ino);
    }
    for (uint32_t i = 0; i < n; i++) {
        xfs_extent_pack(b->data + xfs_bmbt_hdr(fs) + i * XFS_EXTENT_BYTES, &m->e[first + i]);
    }
    xfs_reseal(fs, b->data, fs->blocksize, XFS_BMBT_CRC_OFF);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* Makes the tree's leaves number `want`: takes new ones near the inode and
 * gives back ones no longer needed, each through the reverse map as this
 * inode's tree block. *delta is how many the inode gained (or lost). */
static int fit_leaves(struct xfs *fs, uint64_t ino, uint32_t want, int64_t *delta) {
    int rc = KERR_OK;
    *delta = 0;
    while (rc == KERR_OK && leaf_count < want) {
        uint32_t agno = xfs_fsb_ag(fs, ino >> fs->inopblog), got = 0;
        rc = xfs_alloc_somewhere(fs, &agno, 1u, 0, &got);
        if (rc == KERR_OK) {
            leaves[leaf_count] = ((uint64_t)agno << fs->agblklog) | got;
            rc = xfs_rmap_map(fs, leaves[leaf_count], 1u, ino, XFS_RMAP_OFF_BMBT);
            leaf_count++;
            (*delta)++;
        }
    }
    while (rc == KERR_OK && leaf_count > want) {
        leaf_count--;
        rc = xfs_free_owned(fs, leaves[leaf_count], 1u, ino, XFS_RMAP_OFF_BMBT);
        (*delta)--;
    }
    return rc;
}

/* Writes the map as a two-level tree: leaves filled evenly, and the root
 * in the inode naming them by their first record's file offset. */
static int store_tree(struct xfs *fs, uint64_t ino, uint8_t *di, const struct xfs_fork_map *m,
                      uint32_t want) {
    uint8_t *fork = di + fs->dinode_size;
    uint32_t mx = root_capacity(xfs_fork_bytes(fs, di)), at = 0;
    int rc = KERR_OK;
    for (uint32_t i = 0; i < xfs_fork_bytes(fs, di); i++) {
        fork[i] = 0;
    }
    xfs_put_be16(fork, 1u);
    xfs_put_be16(fork + 2u, (uint16_t)want);
    for (uint32_t l = 0; rc == KERR_OK && l < want; l++) {
        uint32_t n = (m->count - at) / (want - l);
        uint64_t left = l > 0 ? leaves[l - 1u] : NULL_FSB;
        uint64_t right = l + 1u < want ? leaves[l + 1u] : NULL_FSB;
        xfs_put_be64(fork + XFS_BMDR_HDR_BYTES + l * XFS_BMBT_KEY_BYTES, m->e[at].startoff);
        xfs_put_be64(fork + XFS_BMDR_HDR_BYTES + mx * XFS_BMBT_KEY_BYTES +
                         l * XFS_BMBT_PTR_BYTES, leaves[l]);
        rc = write_leaf(fs, ino, leaves[l], m, at, n, left, right);
        at += n;
    }
    di[XFS_DI_FORMAT] = XFS_DINODE_FMT_BTREE;
    return rc;
}

int xfs_fork_store(struct xfs *fs, uint64_t ino, uint8_t *di, const struct xfs_fork_map *m) {
    uint32_t bytes = xfs_fork_bytes(fs, di);
    bool fits = m->count * XFS_EXTENT_BYTES <= bytes;
    uint32_t want = fits ? 0 : (m->count + leaf_capacity(fs) - 1u) / leaf_capacity(fs);
    int64_t delta = 0;
    int rc;
    if (want > root_capacity(bytes) || want > XFS_FORK_LEAVES_MAX) {
        return KERR_NOSPC; /* a third level: not written */
    }
    if (bytes > XFS_FORK_MAX && (!fits || m->count * XFS_EXTENT_BYTES > XFS_FORK_MAX)) {
        return KERR_NOTSUP; /* the last guard: this reader would refuse the result */
    }
    rc = fit_leaves(fs, ino, want, &delta);
    if (rc != KERR_OK) {
        return rc;
    }
    if (fits) {
        store_extents(fs, di, m);
    } else {
        rc = store_tree(fs, ino, di, m, want);
    }
    xfs_dinode_set_data_extents(fs, di, m->count);
    xfs_put_be64(di + XFS_DI_NBLOCKS, xfs_be64(di + XFS_DI_NBLOCKS) + (uint64_t)delta);
    return rc;
}
