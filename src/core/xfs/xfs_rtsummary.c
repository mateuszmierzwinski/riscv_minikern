/* The realtime summary, and the superblock's free realtime extent count,
 * rebuilt from the bitmap after every allocation and release.
 *
 * The summary is an index the allocator may use to find a run by size: an
 * array of `uint32` counters, one per (size class, bitmap block), at index
 * `log * sb_rbmblocks + bitmap block`, each the number of free runs of
 * 2^log..2^(log+1)-1 extents that START in that bitmap block. There are
 * `sb_rextslog + 1` size classes. Like the bitmap, the counters are host-
 * order words; this writes them little-endian (see xfs_rtbitmap.c).
 *
 * **Rebuilt, not adjusted.** The allocator here is first fit over the
 * bitmap and already reads it whole per allocation, so a rebuild costs the
 * same order; adjusting entries incrementally is three ways to get a merge
 * wrong. It is a stream: runs in order, the counters for one bitmap block
 * at a time, and an entry written only when its value changed. Neither the
 * summary nor `sb_frextents` was maintained before, so `xfs_repair` would
 * have flagged every realtime filesystem this writer touched.
 */
#include "core/errno.h"
#include "core/xfs/xfs_format.h"
#include "core/xfs/xfs_internal.h"

#define RSUM_LEVELS_MAX 64u

struct rsum_walk {
    struct xfs *fs;
    struct xfs_inode *sum;
    uint32_t levels;
    uint32_t rbmblocks;
    uint64_t per_block; /* realtime extents one bitmap block describes */
    uint64_t block;     /* the bitmap block whose counters are being gathered */
    uint32_t counts[RSUM_LEVELS_MAX];
    uint64_t free;
};

static uint32_t log2_floor(uint64_t v) {
    uint32_t n = 0;
    while (v > 1u) {
        v >>= 1;
        n++;
    }
    return n;
}

/* Writes one counter when it differs from what the file holds. */
static int put_entry(struct rsum_walk *w, uint32_t level, uint64_t block, uint32_t value) {
    uint64_t off = ((uint64_t)level * w->rbmblocks + block) * 4u;
    uint8_t now[4] = {0, 0, 0, 0}, want[4];
    uint32_t got = 0, put = 0;
    int rc = xfs_file_read(w->fs, w->sum, off, now, 4u, &got);
    if (rc != KERR_OK || got != 4u) {
        return rc != KERR_OK ? rc : KERR_IO;
    }
    for (uint32_t i = 0; i < 4u; i++) {
        want[i] = (uint8_t)(value >> (8u * i));
    }
    if (now[0] == want[0] && now[1] == want[1] && now[2] == want[2] && now[3] == want[3]) {
        return KERR_OK;
    }
    rc = xfs_file_write(w->fs, w->sum, off, want, 4u, &put);
    return rc != KERR_OK ? rc : (put == 4u ? KERR_OK : KERR_IO);
}

/* Writes the gathered counters of every bitmap block before `upto`. */
static int flush_until(struct rsum_walk *w, uint64_t upto) {
    int rc = KERR_OK;
    while (rc == KERR_OK && w->block < upto && w->block < w->rbmblocks) {
        for (uint32_t level = 0; rc == KERR_OK && level < w->levels; level++) {
            rc = put_entry(w, level, w->block, w->counts[level]);
            w->counts[level] = 0;
        }
        w->block++;
    }
    return rc;
}

/* One free run of `len` extents from `start`. */
static int add_run(struct rsum_walk *w, uint64_t start, uint64_t len) {
    uint32_t level = log2_floor(len);
    int rc = flush_until(w, start / w->per_block);
    if (rc == KERR_OK && level < w->levels) {
        w->counts[level]++;
    }
    w->free += len;
    return rc;
}

/* Walks the bitmap's free runs in order. */
static int walk_runs(struct rsum_walk *w, struct xfs_inode *bmp) {
    uint64_t run_start = 0, run = 0, n = 0, end = w->fs->rextents;
    int rc = KERR_OK;
    while (rc == KERR_OK && n < end) {
        uint8_t chunk[64];
        uint32_t got = 0;
        rc = xfs_file_read(w->fs, bmp, n / 8u, chunk, sizeof chunk, &got);
        for (uint32_t i = 0; rc == KERR_OK && i < got * 8u && n < end; i++, n++) {
            bool free = (chunk[i / 8u] & (uint8_t)(1u << (i % 8u))) != 0;
            if (free) {
                run_start = run == 0 ? n : run_start;
                run++;
            } else if (run != 0) {
                rc = add_run(w, run_start, run);
                run = 0;
            }
        }
        rc = rc == KERR_OK && got == 0 && n < end ? KERR_IO : rc;
    }
    return rc != KERR_OK || run == 0 ? rc : add_run(w, run_start, run);
}

/* The superblock's free realtime extent count, set to what was counted. */
static int set_frextents(struct xfs *fs, uint64_t free) {
    struct bbuf *b;
    int rc = bcache_get(fs->cache, 0, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_put_be64(b->data + XFS_SB_FREXTENTS, free);
    xfs_reseal(fs, b->data, fs->sectsize, XFS_SB_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* The summary's shape, from the superblock. */
static int summary_shape(struct xfs *fs, struct rsum_walk *w) {
    struct bbuf *b;
    int rc = bcache_get(fs->cache, 0, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    w->rbmblocks = xfs_be32(b->data + XFS_SB_RBMBLOCKS);
    w->levels = (uint32_t)b->data[XFS_SB_REXTSLOG] + 1u;
    bcache_put(fs->cache, b);
    w->per_block = (uint64_t)fs->blocksize * 8u;
    return w->levels > RSUM_LEVELS_MAX ? KERR_INVAL : KERR_OK;
}

int xfs_rt_summary_rebuild(struct xfs *fs, struct xfs_inode *bmp) {
    struct rsum_walk w = {.fs = fs, .sum = &fs->rsum_inode};
    int rc = summary_shape(fs, &w);
    bool have_summary = fs->rsumino != 0 && w.rbmblocks != 0;
    if (rc == KERR_OK && have_summary) {
        rc = xfs_inode_read(fs, fs->rsumino, &fs->rsum_inode);
    }
    if (rc != KERR_OK) {
        return rc;
    }
    if (!have_summary) {
        w.levels = 0; /* no summary file: count only */
    }
    rc = walk_runs(&w, bmp);
    rc = rc != KERR_OK ? rc : flush_until(&w, w.rbmblocks);
    return rc != KERR_OK ? rc : set_frextents(fs, w.free);
}
