/* The realtime bitmap: which realtime extents are free, and taking some.
 *
 * **The bitmap is a plain file on the *data* device**, not on the realtime
 * device. That is the arrangement's whole trick: the realtime subvolume
 * holds file contents and nothing else, so everything that describes it —
 * this bitmap and the summary — lives in the ordinary filesystem and is
 * read and written like any other file. Which also means allocating a
 * realtime extent needs no new write machinery.
 *
 * One bit per realtime extent, and **a set bit means free**. That is the
 * opposite of the convention most bitmaps use and the reason it is said
 * here rather than left to be inferred: a reader that assumed 1 = used
 * would see a fresh subvolume as entirely full and report `ENOSPC` with
 * everything available.
 *
 * Verified against a filesystem `mkfs.xfs -r rtdev=` actually made: 25600
 * realtime extents gave exactly 25600 set bits, in 3200 bytes of `0xff`
 * with zeroes after, and no block header of any kind.
 *
 * **The word order is a real wart in the format.** Classic XFS reads this
 * bitmap as an array of `xfs_rtword_t`, a plain `uint32` in the *host's*
 * byte order — so the on-disk bytes of a bitmap depend on the machine that
 * wrote it, which is a portability bug in XFS itself, fixed only by the
 * recent rtgroups layout that made the words big-endian. This treats the
 * bitmap as a byte array with the least significant bit of each byte first,
 * which is exactly what little-endian word access means and is therefore
 * correct here and on every little-endian writer. A big-endian image's
 * bitmap would read wrong, and nothing in the superblock says which it is;
 * that is the format's problem and not something a reader can fix.
 */
#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs_format.h"
#include "core/xfs/xfs_internal.h"

/* How many bytes of bitmap the subvolume's extents occupy. */
static uint64_t bitmap_bytes(const struct xfs *fs) {
    return (fs->rextents + 7u) / 8u;
}

/* Reads the byte holding extent `n`'s bit. */
static int bitmap_byte(struct xfs *fs, struct xfs_inode *bmp, uint64_t n, uint8_t *out) {
    uint32_t got = 0;
    int rc = xfs_file_read(fs, bmp, n / 8u, out, 1u, &got);
    return rc != KERR_OK ? rc : (got == 1u ? KERR_OK : KERR_IO);
}

/* Opens the bitmap file, which every operation here starts by doing. */
static int open_bitmap(struct xfs *fs, struct xfs_inode *bmp) {
    if (fs->rblocks == 0 || fs->rextents == 0) {
        return KERR_NOTSUP; /* no realtime subvolume */
    }
    if (fs->rbmino == 0) {
        return KERR_NODEV; /* there is one, but its bitmap was never made */
    }
    return xfs_inode_read(fs, fs->rbmino, bmp);
}

bool xfs_rt_extent_is_free(struct xfs *fs, uint64_t n) {
    struct xfs_inode bmp;
    uint8_t byte = 0;
    if (n >= fs->rextents || open_bitmap(fs, &bmp) != KERR_OK) {
        return false;
    }
    if (bitmap_byte(fs, &bmp, n, &byte) != KERR_OK) {
        return false;
    }
    return (byte & (uint8_t)(1u << (n % 8u))) != 0;
}

/* Counts the free extents, a byte at a time.
 *
 * A byte at a time rather than a word because the word order is the host's
 * (see the file comment) and a popcount does not care: counting set bits in
 * a byte array gives the same answer whatever order the words are in, which
 * makes this one operation portable even though addressing a single bit is
 * not. */
int xfs_rt_free_extents(struct xfs *fs, uint64_t *out) {
    static const uint8_t NIBBLE[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
    struct xfs_inode bmp;
    uint64_t total = 0, at = 0, end = bitmap_bytes(fs);
    int rc = open_bitmap(fs, &bmp);
    *out = 0;
    if (rc != KERR_OK) {
        return rc;
    }
    while (at < end) {
        uint8_t chunk[64];
        uint32_t want = (uint32_t)(end - at < sizeof chunk ? end - at : sizeof chunk);
        uint32_t got = 0;
        rc = xfs_file_read(fs, &bmp, at, chunk, want, &got);
        if (rc != KERR_OK || got == 0) {
            return rc != KERR_OK ? rc : KERR_IO;
        }
        for (uint32_t i = 0; i < got; i++) {
            total += NIBBLE[chunk[i] & 0xfu] + NIBBLE[chunk[i] >> 4];
        }
        at += got;
    }
    /* The last byte may hold bits past the end of the subvolume. They are
     * zero on a filesystem `mkfs` made, but a count that trusted that
     * would be wrong on one that did not, and too *high* — which reads as
     * free space that does not exist. */
    *out = total > fs->rextents ? fs->rextents : total;
    return KERR_OK;
}

/* Sets or clears one extent's bit. Free is 1, so taking an extent clears. */
static int set_bit(struct xfs *fs, struct xfs_inode *bmp, uint64_t n, bool free) {
    uint8_t byte = 0;
    uint32_t put = 0;
    int rc = bitmap_byte(fs, bmp, n, &byte);
    uint8_t bit = (uint8_t)(1u << (n % 8u));
    if (rc != KERR_OK) {
        return rc;
    }
    byte = free ? (uint8_t)(byte | bit) : (uint8_t)(byte & ~bit);
    rc = xfs_file_write(fs, bmp, n / 8u, &byte, 1u, &put);
    return rc != KERR_OK ? rc : (put == 1u ? KERR_OK : KERR_IO);
}

/* Marks a run, all of it or none: a half-marked run is a bitmap that
 * disagrees with the inodes, which is worse than a failed allocation. */
static int mark_run(struct xfs *fs, struct xfs_inode *bmp, uint64_t start, uint64_t count,
                    bool free) {
    for (uint64_t i = 0; i < count; i++) {
        int rc = set_bit(fs, bmp, start + i, free);
        if (rc != KERR_OK) {
            /* Put back what was already changed, so the bitmap is left as
             * it was found rather than half way. */
            for (uint64_t j = 0; j < i; j++) {
                (void)set_bit(fs, bmp, start + j, !free);
            }
            return rc;
        }
    }
    return KERR_OK;
}

/* The first run of `count` free extents, or `KERR_NOSPC`.
 *
 * First fit, scanning bit by bit. The *summary* file exists precisely so a
 * real allocator does not have to do this — it indexes free runs by size
 * per bitmap block — but first fit over the bitmap gives the same answer,
 * and an allocator that is correct and slow is a better starting point than
 * one that is fast and consults an index this does not yet maintain.
 * Maintaining the summary without using it would be the worst of the three:
 * work whose correctness nothing checks. */
static int find_run(struct xfs *fs, struct xfs_inode *bmp, uint64_t count, uint64_t *start) {
    uint64_t run = 0;
    for (uint64_t n = 0; n < fs->rextents; n++) {
        uint8_t byte = 0;
        int rc = bitmap_byte(fs, bmp, n, &byte);
        if (rc != KERR_OK) {
            return rc;
        }
        run = (byte & (uint8_t)(1u << (n % 8u))) != 0 ? run + 1u : 0;
        if (run == count) {
            *start = n + 1u - count;
            return KERR_OK;
        }
    }
    return KERR_NOSPC;
}

int xfs_rt_alloc(struct xfs *fs, uint64_t count, uint64_t *start) {
    struct xfs_inode bmp;
    int rc;
    if (count == 0) {
        return KERR_INVAL;
    }
    if (!xfs_writable(fs)) {
        return KERR_ROFS;
    }
    rc = open_bitmap(fs, &bmp);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = find_run(fs, &bmp, count, start);
    return rc != KERR_OK ? rc : mark_run(fs, &bmp, *start, count, false);
}

int xfs_rt_release(struct xfs *fs, uint64_t start, uint64_t count) {
    struct xfs_inode bmp;
    int rc;
    if (count == 0 || start + count > fs->rextents) {
        return KERR_INVAL;
    }
    if (!xfs_writable(fs)) {
        return KERR_ROFS;
    }
    rc = open_bitmap(fs, &bmp);
    return rc != KERR_OK ? rc : mark_run(fs, &bmp, start, count, true);
}
