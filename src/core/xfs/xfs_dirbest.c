/* A directory data block's own account of its free space: the free spans
 * themselves, and the header's `bestfree` table naming the three largest.
 *
 * **One owner, at the moment the block is stored.** The table used to be
 * written by whichever builder made the block — once, when the block was
 * empty — and never again, so every block that had entries added to it
 * claimed to be entirely free. Nothing here reads the table; `xfs_repair`
 * and Linux both do, and condemn the block. Computing it from the spans
 * when the block goes to disk makes it impossible for the two to disagree.
 *
 * The same pass trims a block-format directory's last free span to the
 * hash array, which grows down into it: the span keeps the length it had
 * when it was written (xfs_dirgrow.c says why the writer tolerates that),
 * and on disk a span overlapping the array is a corrupt block.
 *
 * And it **joins free spans that touch**. Removing a name frees its entry
 * where it stands, and the removers never looked at the neighbours, so a
 * block emptied one name at a time became a row of small free spans —
 * which `xfs_repair` reports block by block, and which hides a run long
 * enough for a new entry behind a row too short for one. */
#include "core/xfs/xfs_internal.h"

#define BESTFREE_SLOTS 3u

struct span {
    uint32_t at;
    uint32_t len;
};

/* Where the table sits: after the v5 header's self-describing fields, or
 * straight after the magic on v4. */
static uint32_t table_at(const struct xfs *fs) {
    return fs->v5 ? XFS_DIR3_BESTFREE_OFF : XFS_DIR2_BESTFREE_OFF;
}

/* Where the entries stop: at the hash array of a block-format directory,
 * at the end of the block otherwise. */
static uint32_t data_end(const struct xfs *fs, const uint8_t *block) {
    uint32_t count;
    if (xfs_be32(block + XFS_DIR3_HDR_MAGIC) != xfs_magic_for(fs, XFS_BLK_DIR_BLOCK)) {
        return fs->dirblocksize;
    }
    count = xfs_be32(block + fs->dirblocksize - XFS_DIR2_BLOCK_TAIL_SIZE);
    return fs->dirblocksize - XFS_DIR2_BLOCK_TAIL_SIZE - count * XFS_DIR2_LEAF_ENTRY_SIZE;
}

/* Keeps the three longest spans, longest first. A span equal to one already
 * kept goes after it, which is the order Linux's own insertion leaves —
 * and so the lower offset wins a tie, since spans are met in order. */
static void keep_if_long(struct span best[BESTFREE_SLOTS], uint32_t at, uint32_t len) {
    for (uint32_t i = 0; i < BESTFREE_SLOTS; i++) {
        if (len > best[i].len) {
            for (uint32_t j = BESTFREE_SLOTS - 1u; j > i; j--) {
                best[j] = best[j - 1u];
            }
            best[i] = (struct span){at, len};
            return;
        }
    }
}

/* The length of the free span at `at` with every free span directly after
 * it, below `end`. */
static uint32_t joined_length(const uint8_t *block, uint32_t at, uint32_t end) {
    uint32_t len = xfs_be16(block + at + 2u);
    while (at + len + XFS_DIR2_DATA_ALIGN <= end &&
           xfs_be16(block + at + len) == XFS_DIR2_DATA_FREE_TAG) {
        uint32_t more = xfs_be16(block + at + len + 2u);
        if (more < XFS_DIR2_DATA_ALIGN) {
            break;
        }
        len += more;
    }
    return len;
}

/* Walks the entries and free spans below `end`, joining spans that touch
 * and trimming one that runs past it. Stops at anything that cannot be a
 * span or an entry, leaving the rest of the block as it found it. */
static void scan_spans(const struct xfs *fs, uint8_t *block, uint32_t end,
                       struct span best[BESTFREE_SLOTS]) {
    uint32_t at = fs->dir_data_hdr;
    while (at + XFS_DIR2_DATA_ALIGN <= end) {
        uint32_t len;
        if (xfs_be16(block + at) != XFS_DIR2_DATA_FREE_TAG) {
            at += xfs_dir_entry_size(fs, block[at + 8u]);
            continue;
        }
        if (xfs_be16(block + at + 2u) < XFS_DIR2_DATA_ALIGN) {
            return;
        }
        len = joined_length(block, at, end);
        len = at + len > end ? end - at : len;
        xfs_dir_put_free(block, at, len);
        keep_if_long(best, at, len);
        at += len;
    }
}

void xfs_dir_seal_free(const struct xfs *fs, uint8_t *block) {
    uint32_t magic = xfs_be32(block + XFS_DIR3_HDR_MAGIC);
    struct span best[BESTFREE_SLOTS] = {{0, 0}, {0, 0}, {0, 0}};
    uint8_t *table = block + table_at(fs);
    if (magic != xfs_magic_for(fs, XFS_BLK_DIR_BLOCK) &&
        magic != xfs_magic_for(fs, XFS_BLK_DIR_DATA)) {
        return;
    }
    scan_spans(fs, block, data_end(fs, block), best);
    for (uint32_t i = 0; i < BESTFREE_SLOTS; i++) {
        xfs_put_be16(table + i * 4u, (uint16_t)best[i].at);
        xfs_put_be16(table + i * 4u + 2u, (uint16_t)best[i].len);
    }
}
