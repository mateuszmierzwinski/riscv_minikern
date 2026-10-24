/* Finding a name without reading every entry: the directory's hash index.
 *
 * **What leaf and node format are for.** Listing a directory walks its data
 * blocks, and that is how this reader found names too — a linear scan, so
 * one lookup in a directory of three thousand entries read all three
 * thousand. Correct, and O(n) where the format offers O(log n); in a
 * directory of forty thousand it is the difference between a path walk and a
 * pause.
 *
 * The index is above 32 GiB of logical offset, which is how it stays out of
 * the data. A **leaf** block holds `{hash, address}` pairs sorted by hash. A
 * directory with more than one leaf gains **node** blocks above them,
 * holding `{hash, block}` pairs, and the descent picks the first child whose
 * hash is not below the one wanted.
 *
 * Two details decide whether this is correct rather than nearly correct.
 * **Hashes collide**, so finding one is the start of a scan and not the
 * answer: every entry with the same hash must be resolved and its name
 * compared, and a run of equal hashes can continue into the next leaf, which
 * is why `forw` is followed. And an **address** is a byte offset divided by
 * eight, so it resolves to a block and an offset within it, not to a block.
 *
 * The hash itself is an on-disk interface fact. A reader that computes it
 * differently from the writer finds nothing at all — which is a failure
 * mode worth naming, because it looks exactly like an empty directory.
 */
#include "core/errno.h"
#include "core/xfs/xfs_format.h"
#include "core/xfs/xfs_internal.h"

static uint32_t rol32(uint32_t v, uint32_t n) {
    return (v << n) | (v >> (32u - n));
}

uint32_t xfs_dir_hashname(const char *name, uint32_t len) {
    const uint8_t *p = (const uint8_t *)name;
    uint32_t hash = 0;
    while (len >= 4u) {
        hash = ((uint32_t)p[0] << 21) ^ ((uint32_t)p[1] << 14) ^ ((uint32_t)p[2] << 7) ^
               ((uint32_t)p[3]) ^ rol32(hash, 28u);
        len -= 4u;
        p += 4u;
    }
    switch (len) {
    case 3:
        return ((uint32_t)p[0] << 14) ^ ((uint32_t)p[1] << 7) ^ ((uint32_t)p[2]) ^ rol32(hash, 21u);
    case 2:
        return ((uint32_t)p[0] << 7) ^ ((uint32_t)p[1]) ^ rol32(hash, 14u);
    case 1:
        return ((uint32_t)p[0]) ^ rol32(hash, 7u);
    default:
        return hash;
    }
}

/* The entry at a directory address, compared against the name wanted.
 *
 * An address is a byte offset into the data space divided by eight, so this
 * is where the index meets the data: the offset names a block and a place
 * inside it, and what is there must be an entry whose name fits within the
 * block it started in. */
static int name_at(struct xfs *fs, const struct xfs_inode *dir, uint32_t address,
                   const char *name, uint32_t len, uint64_t *ino) {
    uint64_t byte = (uint64_t)address * XFS_DIR2_DATA_ALIGN;
    uint64_t first = (byte / fs->dirblocksize) * fs->dirblocks;
    uint32_t within = (uint32_t)(byte % fs->dirblocksize);
    struct xfs_dirblock b;
    int rc;
    if (within + 9u + len > fs->dirblocksize || within < fs->dir_data_hdr) {
        return KERR_IO;
    }
    rc = xfs_dir_data_get(fs, dir, first, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = KERR_NOENT;
    if (b.at[within + 8u] == len) {
        uint32_t i = 0;
        while (i < len && b.at[within + 9u + i] == (uint8_t)name[i]) {
            i++;
        }
        if (i == len) {
            *ino = xfs_be64(b.at + within);
            rc = KERR_OK;
        }
    }
    xfs_dir_block_put(fs, &b);
    return rc;
}

/* The first index in a leaf whose hash is not below `hash`.
 *
 * A binary search, which is the point of the whole structure — and it
 * returns the *first* of a run of equal hashes rather than any of them,
 * because a colliding name may be at either end of that run. */
static uint32_t first_at_or_above(const uint8_t *entries, uint32_t count, uint32_t hash) {
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (xfs_be32(entries + mid * XFS_DA3_ENTRY_SIZE) < hash) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* Scans one leaf's run of matching hashes, and says where to continue.
 *
 * `next` is the logical block of the following leaf when the run reached the
 * end of this one — a run of equal hashes may span leaves, and stopping at
 * the boundary would miss a name whose hash happens to sit there. */
static int search_leaf(struct xfs *fs, const struct xfs_inode *dir, const uint8_t *leaf,
                       uint32_t hash, const char *name, uint32_t len, uint64_t *ino,
                       uint64_t *next) {
    uint32_t count = xfs_be16(leaf + fs->da_count);
    const uint8_t *entries = leaf + fs->da_hdr;
    uint32_t at;
    *next = 0;
    if (fs->da_hdr + (uint64_t)count * XFS_DA3_ENTRY_SIZE > fs->dirblocksize) {
        return KERR_IO;
    }
    at = first_at_or_above(entries, count, hash);
    for (; at < count; at++) {
        const uint8_t *e = entries + at * XFS_DA3_ENTRY_SIZE;
        uint32_t address = xfs_be32(e + 4u);
        int rc;
        if (xfs_be32(e) != hash) {
            return KERR_NOENT; /* past the run: the name is not here */
        }
        if (address == 0) {
            continue; /* a stale entry: the name it had was removed */
        }
        rc = name_at(fs, dir, address, name, len, ino);
        if (rc != KERR_NOENT) {
            return rc;
        }
    }
    /* The run reached the end of the leaf, so it may continue in the next. */
    *next = xfs_be32(leaf + XFS_DA3_FORW_OFF);
    return KERR_NOENT;
}

/* Descends the node blocks to the leaf that would hold `hash`.
 *
 * Node entries are `{hash, block}` sorted by hash, where the hash is the
 * *largest* in the child — so the child wanted is the first whose hash is
 * not below the one being looked for. */
static int find_leaf(struct xfs *fs, const struct xfs_inode *dir, uint32_t hash, uint64_t *block) {
    uint64_t at = fs->dir_leaf_block;
    for (uint32_t depth = 0; depth <= XFS_DA3_MAX_LEVELS; depth++) {
        struct xfs_dirblock b;
        uint32_t magic, count, pick;
        int rc = xfs_dir_index_get(fs, dir, at, &b);
        if (rc != KERR_OK) {
            return rc;
        }
        magic = xfs_be16(b.at + XFS_DA3_MAGIC_OFF);
        count = xfs_be16(b.at + fs->da_count);
        if (magic != xfs_magic_for(fs, XFS_BLK_DA_NODE)) {
            xfs_dir_block_put(fs, &b);
            *block = at;
            return KERR_OK; /* a leaf: this is the one */
        }
        pick = first_at_or_above(b.at + fs->da_hdr, count, hash);
        if (count == 0 || fs->da_hdr + (uint64_t)count * XFS_DA3_ENTRY_SIZE >
                              fs->dirblocksize) {
            xfs_dir_block_put(fs, &b);
            return KERR_IO;
        }
        pick = pick < count ? pick : count - 1u;
        at = xfs_be32(b.at + fs->da_hdr + pick * XFS_DA3_ENTRY_SIZE + 4u);
        xfs_dir_block_put(fs, &b);
    }
    return KERR_IO; /* deeper than any real tree */
}

int xfs_dir_hash_lookup(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                        uint32_t len, uint64_t *ino) {
    uint32_t hash = xfs_dir_hashname(name, len);
    struct xfs_extent e;
    uint64_t block;
    int rc;
    /* No index without a block above the data: a shortform directory keeps
     * its entries in the inode, and a block-format one keeps its hashes in
     * its single block's tail — both are a handful of entries, and a linear
     * walk over them beats reading a structure that is not there. */
    if (xfs_bmap(fs, dir, fs->dir_leaf_block, &e) != KERR_OK) {
        return KERR_NOTSUP;
    }
    rc = find_leaf(fs, dir, hash, &block);
    for (uint32_t hops = 0; rc == KERR_OK && hops <= XFS_DA3_MAX_LEVELS; hops++) {
        struct xfs_dirblock b;
        uint64_t next = 0;
        rc = xfs_dir_index_get(fs, dir, block, &b);
        if (rc != KERR_OK) {
            return rc;
        }
        rc = search_leaf(fs, dir, b.at, hash, name, len, ino, &next);
        xfs_dir_block_put(fs, &b);
        if (rc != KERR_NOENT || next == 0) {
            return rc;
        }
        block = next;
        rc = KERR_OK;
    }
    return rc == KERR_OK ? KERR_NOENT : rc;
}
