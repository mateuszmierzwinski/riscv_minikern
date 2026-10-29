/* The group's free list (AGFL): the blocks its own trees grow into.
 *
 * The free-space trees and the reverse-map tree cannot ask the allocator for
 * a block when they split — the allocator *is* those trees, part way through
 * a change. So each group keeps a small ring of blocks set aside, and every
 * change to them first makes sure the ring holds enough for the worst that
 * change can do: a split at every level of every tree it touches. Topping
 * it up takes blocks from free space by the one path that never asks for
 * the ring itself (`xfs_alloc_raw`), and maps them to the group (`OWN_AG`),
 * which is what a block on the ring is.
 *
 * Two counts in the AGF follow a block as it moves: `agf_btreeblks` for
 * any tree block beyond the trees' roots, and `agf_rmap_blocks` for the
 * reverse map's. The superblock's free count does not move at all — a block
 * on the ring or in a tree is counted free there, which is the identity
 * `xfs_repair` checks (`fdblocks` = free + list + tree blocks). */
#include "core/bcache.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs_internal.h"

static uint32_t agfl_size(const struct xfs *fs) {
    return (fs->sectsize - XFS_AGFL_BNO) / 4u;
}

/* The AGF and the free list, each in whichever block holds its sector. */
struct agfl_view {
    struct bbuf *fb, *lb;
    uint8_t *agf, *agfl;
};

static void agfl_release(struct xfs *fs, struct agfl_view *v) {
    bcache_put(fs->cache, v->lb);
    bcache_put(fs->cache, v->fb);
}

/* Both headers, with the AGF and the free list each checked. */
static int agfl_get(struct xfs *fs, uint32_t agno, struct agfl_view *v) {
    int rc = xfs_agf_get(fs, agno, &v->fb, &v->agf);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_group_header(fs, agno, XFS_AGFL_SECTOR, &v->lb, &v->agfl);
    if (rc != KERR_OK) {
        bcache_put(fs->cache, v->fb);
        return rc;
    }
    if (fs->v5 && (xfs_be32(v->agfl) != XFS_AGFL_MAGIC ||
                   !xfs_crc_ok(fs, v->agfl, fs->sectsize, XFS_AGFL_CRC) ||
                   !xfs_uuid_ok(fs, v->agfl + XFS_AGFL_UUID))) {
        kprintf("xfs: group %u's free list is unusable\n", agno);
        agfl_release(fs, v);
        return KERR_IO;
    }
    return KERR_OK;
}

/* Where the ring's slots start: after the v5 header, or at once on v4. */
static uint8_t *slots(const struct xfs *fs, const struct agfl_view *v) {
    return v->agfl + (fs->v5 ? XFS_AGFL_BNO : 0u);
}

static uint32_t ring_size(const struct xfs *fs) {
    return fs->v5 ? agfl_size(fs) : fs->sectsize / 4u;
}

static void agf_add(uint8_t *agf, uint32_t off, int32_t delta) {
    xfs_put_be32(agf + off, xfs_be32(agf + off) + (uint32_t)delta);
}

static void agfl_done(struct xfs *fs, struct agfl_view *v) {
    xfs_reseal(fs, v->agf, fs->sectsize, XFS_AGF_CRC);
    xfs_reseal(fs, v->agfl, fs->sectsize, XFS_AGFL_CRC);
    bcache_dirty(v->fb);
    bcache_dirty(v->lb);
    agfl_release(fs, v);
}

/* The AGF's counts of tree blocks, for a block leaving the ring for a tree
 * (+1) or coming back from one (-1). */
static void count_tree_block(uint8_t *agf, enum xfs_agfl_use use, int32_t delta) {
    if (use == XFS_AGFL_PLAIN) {
        return;
    }
    agf_add(agf, XFS_AGF_BTREEBLKS, delta);
    if (use == XFS_AGFL_RMAP) {
        agf_add(agf, XFS_AGF_RMAP_BLOCKS, delta);
    }
}

int xfs_agfl_pop(struct xfs *fs, uint32_t agno, enum xfs_agfl_use use, uint32_t *agbno) {
    struct agfl_view v;
    uint32_t first;
    int rc = agfl_get(fs, agno, &v);
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfs_be32(v.agf + XFS_AGF_FLCOUNT) == 0) {
        agfl_release(fs, &v);
        return KERR_NOSPC;
    }
    first = xfs_be32(v.agf + XFS_AGF_FLFIRST);
    *agbno = xfs_be32(slots(fs, &v) + first * 4u);
    xfs_put_be32(v.agf + XFS_AGF_FLFIRST, (first + 1u) % ring_size(fs));
    agf_add(v.agf, XFS_AGF_FLCOUNT, -1);
    count_tree_block(v.agf, use, 1);
    agfl_done(fs, &v);
    return KERR_OK;
}

int xfs_agfl_push(struct xfs *fs, uint32_t agno, enum xfs_agfl_use use, uint32_t agbno) {
    struct agfl_view v;
    uint32_t last;
    int rc = agfl_get(fs, agno, &v);
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfs_be32(v.agf + XFS_AGF_FLCOUNT) >= ring_size(fs)) {
        agfl_release(fs, &v);
        return KERR_NOSPC;
    }
    last = (xfs_be32(v.agf + XFS_AGF_FLLAST) + 1u) % ring_size(fs);
    xfs_put_be32(slots(fs, &v) + last * 4u, agbno);
    xfs_put_be32(v.agf + XFS_AGF_FLLAST, last);
    agf_add(v.agf, XFS_AGF_FLCOUNT, 1);
    count_tree_block(v.agf, use, -1);
    agfl_done(fs, &v);
    return KERR_OK;
}

/* The blocks one change can take from the ring: two inserts — a cut from
 * the middle of a free extent leaves a record either side — splitting every
 * level of both free-space trees, and the same of the reverse map. */
static uint32_t worst_case(const struct xfs *fs, const uint8_t *agf) {
    uint32_t bno = xfs_be32(agf + XFS_AGF_LEVELS), cnt = xfs_be32(agf + XFS_AGF_LEVELS + 4u);
    uint32_t rmap = fs->has_rmapbt ? xfs_be32(agf + XFS_AGF_LEVELS + 8u) : 0u;
    return 2u * (bno + 1u) + 2u * (cnt + 1u) + (fs->has_rmapbt ? 2u * (rmap + 1u) : 0u);
}

/* Moves `count` blocks from free space onto the ring and maps them to the
 * group. One run when there is one, which is also one reverse-map record. */
static int refill(struct xfs *fs, uint32_t agno, uint32_t count) {
    uint32_t start = 0;
    int rc = xfs_alloc_raw(fs, agno, count, &start);
    for (uint32_t i = 0; rc == KERR_OK && i < count; i++) {
        rc = xfs_agfl_push(fs, agno, XFS_AGFL_PLAIN, start + i);
    }
    return rc != KERR_OK ? rc
                         : xfs_rmap_map(fs, ((uint64_t)agno << fs->agblklog) | start, count,
                                        XFS_RMAP_OWN_AG, 0);
}

int xfs_agfl_ensure(struct xfs *fs, uint32_t agno) {
    struct agfl_view v;
    uint32_t need, have, size = ring_size(fs);
    int rc = agfl_get(fs, agno, &v);
    if (rc != KERR_OK) {
        return rc;
    }
    need = worst_case(fs, v.agf);
    have = xfs_be32(v.agf + XFS_AGF_FLCOUNT);
    agfl_release(fs, &v);
    if (need > size / 2u) {
        return KERR_NOSPC; /* trees this tall would not fit the ring's guarantee */
    }
    if (have >= need) {
        return size - have >= need ? KERR_OK : KERR_NOSPC;
    }
    /* Topped up to twice the need, so the refill's own changes — it takes
     * from the free-space trees and maps what it took — are covered too. */
    return refill(fs, agno, 2u * need - have);
}
