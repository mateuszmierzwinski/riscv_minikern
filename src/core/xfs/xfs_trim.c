/* Trim: telling the drive which blocks the filesystem is not using.
 *
 * Linux's `FITRIM`, which `fstrim` calls: every free extent in every group,
 * at least `minlen` blocks long and inside the range asked for, is
 * discarded through the cache (`bcache_discard`). A flash drive then knows
 * those blocks hold nothing, and stops copying them about when it erases.
 *
 * **Periodic, not per free.** A discard per freed extent (Linux's `discard`
 * mount option) costs a slow device command on every unlink, and Linux
 * leaves it off for that reason; `fstrim.timer` once a week is the default
 * it ships. This is the weekly pass, run on demand or by the kernel's timer.
 *
 * **Only what is free on the disk.** The cache is flushed first, so the
 * free-space trees on the device are the ones walked here. A block freed in
 * memory but still allocated on disk would, after a crash, be a file block
 * the drive had been told to forget. Nothing else runs while this does: the
 * one kernel lock is held by the caller. */
#include <stddef.h>

#include "core/bcache.h"
#include "core/errno.h"
#include "core/xfs/xfs_internal.h"

struct trim {
    struct xfs *fs;
    const struct xfs_trim_range *range;
    uint64_t trimmed;
};

/* One free extent, clipped to the range, discarded when long enough. */
static int trim_one(void *self, uint32_t agno, uint32_t start, uint32_t count) {
    struct trim *t = self;
    const struct xfs_trim_range *r = t->range;
    uint64_t first = (uint64_t)agno * t->fs->agblocks + start, end = first + count;
    uint64_t lo = first > r->start ? first : r->start;
    uint64_t hi = end < r->start + r->len ? end : r->start + r->len;
    int rc;
    if (hi <= lo || hi - lo < r->minlen) {
        return KERR_OK;
    }
    rc = bcache_discard(t->fs->cache, lo, (uint32_t)(hi - lo));
    t->trimmed += rc == KERR_OK ? hi - lo : 0;
    return rc;
}

/* Whether a trim can be asked of this filesystem at all. */
static int trim_refusal(const struct xfs *fs, const struct xfs_trim_range *r) {
    if (!fs->mounted || r->start >= fs->dblocks || r->len == 0) {
        return KERR_INVAL;
    }
    if (!xfs_writable(fs)) {
        return KERR_ROFS;
    }
    return fs->cache->dev->discard == NULL ? KERR_NOTSUP : KERR_OK;
}

int xfs_trim(struct xfs *fs, const struct xfs_trim_range *r, uint64_t *trimmed) {
    struct trim t = {.fs = fs, .range = r, .trimmed = 0};
    int rc = trim_refusal(fs, r);
    rc = rc != KERR_OK ? rc : xfs_flush(fs);
    for (uint32_t agno = 0; rc == KERR_OK && agno < fs->agcount; agno++) {
        rc = xfs_free_extents_each(fs, agno, trim_one, &t);
    }
    *trimmed = t.trimmed;
    return rc;
}
