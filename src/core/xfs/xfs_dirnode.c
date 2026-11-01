/* A directory whose hashes outgrow one block: leaf format becomes node.
 *
 * **The last shape a directory has, and the one that does not run out.**
 * Leaf format keeps every hash in one block, so it ends at about five
 * hundred names however many data blocks it has. Node format puts a
 * **node** block above several `LEAFN` blocks and indexes them by hash, so
 * the ceiling becomes the node's own capacity — five hundred leaves of five
 * hundred hashes, which is past any directory a person makes.
 *
 * Three things move in the conversion, and the middle one is the surprise:
 *
 *   - the one `LEAF1` block's hashes are split across **two** `LEAFN`
 *     blocks, chained by `forw`/`back` so that a run of equal hashes can
 *     continue into the next;
 *   - the block that was the leaf becomes the **node root**, and the leaves
 *     move to new addresses. The root has to keep its block number because
 *     that is where a search starts (`fs->dir_leaf_block`) — so the
 *     conversion moves the *contents* out rather than putting the root
 *     somewhere new;
 *   - the `bests` array cannot stay, because a `LEAFN` block is all
 *     hashes. It moves to a **free block** at 64 GiB of logical offset, a
 *     third address space above the data and the leaves.
 *
 * This reader never consults `bests`; it walks the data blocks. It is
 * maintained because `xfs_repair` and Linux read it, which makes it exactly
 * the structure a writer leaves wrong.
 */
#include "core/errno.h"
#include "core/xfs/xfs_format.h"
#include "core/xfs/xfs_internal.h"

/* A da-block number, which is in **filesystem** blocks and not directory
 * blocks: a directory block spans `fs->dirblocks` of them, and the reader
 * uses what it finds here directly as a logical block to map. */
static uint64_t leaf_n(const struct xfs *fs, uint32_t which) {
    return fs->dir_leaf_block + (uint64_t)(which + 1u) * fs->dirblocks;
}

/* How many hash pairs a `LEAFN` block can hold. No `bests` and no tail —
 * that is what moving them to the free block bought. */
static uint32_t leafn_capacity(const struct xfs *fs) {
    return (fs->dirblocksize - fs->da_hdr) / XFS_DA3_ENTRY_SIZE;
}

/* The highest hash in a run of pairs, which is what a node entry carries
 * for its child: a search takes the first child whose hash is at or above
 * what it wants. */
static uint32_t top_hash(const uint8_t *pairs, uint32_t count) {
    return count == 0 ? 0 : xfs_be32(pairs + (count - 1u) * XFS_DA3_ENTRY_SIZE);
}

/* Where the free block's three counts live: after the v5 header, or
 * straight after the magic on v4. `nth` is 0, 1 or 2 — `firstdb`, `nvalid`
 * and `nused`. These were v5's offsets on both, which on v4 put the counts
 * inside the `bests` array. */
static uint32_t free_count_at(const struct xfs *fs, uint32_t nth) {
    return (fs->v5 ? XFS_DIR3_FREE_FIRSTDB : XFS_DIR2_FREE_FIRSTDB) + 4u * nth;
}

/* Recounts `nused`: the entries that name a data block at all.
 *
 * **Not the ones with room.** A best of zero is a data block that is
 * entirely full, which is a block in use; only `NULLDATAOFF` means none.
 * This counted the non-zero ones, so a full block went uncounted — and
 * once the free block existed nothing recounted it as blocks were added. */
static void free_recount(struct xfs *fs) {
    uint32_t n = xfs_be32(fs->dirblock + free_count_at(fs, 1u)), used = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t at = fs->dir_free_hdr + i * XFS_DIR2_LEAF_BEST_SIZE;
        used += xfs_be16(fs->dirblock + at) != XFS_DIR2_NULL_DATAOFF ? 1u : 0u;
    }
    xfs_put_be32(fs->dirblock + free_count_at(fs, 2u), used);
}

/* Ends `nvalid` at the last data block there is, past any trailing slots a
 * given-back block left as `NULLDATAOFF`. */
static void free_trim(struct xfs *fs) {
    uint32_t n = xfs_be32(fs->dirblock + free_count_at(fs, 1u));
    while (n > 0 && xfs_be16(fs->dirblock + fs->dir_free_hdr +
                             (n - 1u) * XFS_DIR2_LEAF_BEST_SIZE) == XFS_DIR2_NULL_DATAOFF) {
        xfs_put_be16(fs->dirblock + fs->dir_free_hdr + (n - 1u) * XFS_DIR2_LEAF_BEST_SIZE, 0);
        n--;
    }
    xfs_put_be32(fs->dirblock + free_count_at(fs, 1u), n);
}

/* Writes the free block: the `bests` array, evicted from the leaf.
 * `nvalid` is how many data blocks it describes and `nused` how many of
 * those exist. */
static void build_free(struct xfs *fs, uint64_t ino, const uint8_t *bests, uint32_t n) {
    for (uint32_t i = 0; i < fs->dirblocksize; i++) {
        fs->dirblock[i] = 0;
    }
    xfs_put_be32(fs->dirblock, xfs_magic_for(fs, XFS_BLK_DIR_FREE));
    /* The UUID at 24 and the owner at 40, as every v5 directory header has
     * them. The owner went in at 32, over the UUID's second half, and the
     * field `xfs_repair` reads stayed zero. */
    xfs_stamp_owner(fs, fs->dirblock, 0, XFS_DIR3_HDR_UUID, XFS_DIR3_HDR_OWNER, 0, ino);
    xfs_put_be32(fs->dirblock + free_count_at(fs, 0), 0);
    xfs_put_be32(fs->dirblock + free_count_at(fs, 1u), n);
    for (uint32_t i = 0; i < n; i++) {
        uint16_t best = (uint16_t)xfs_be16(bests + i * XFS_DIR2_LEAF_BEST_SIZE);
        xfs_put_be16(fs->dirblock + fs->dir_free_hdr + i * XFS_DIR2_LEAF_BEST_SIZE,
                     best);
    }
    free_recount(fs);
}

/* How many data blocks one free block describes, and the logical block
 * of free block `k`: the free index is a run of them from 64 GiB up, the
 * k-th describing data blocks from `k * per` on (its `firstdb`). One was
 * all there was, and past its ~2,000 slots a create answered EIO. */
static uint32_t free_per_block(const struct xfs *fs) {
    return (fs->dirblocksize - fs->dir_free_hdr) / XFS_DIR2_LEAF_BEST_SIZE;
}

static uint64_t free_block_at(const struct xfs *fs, uint32_t k) {
    return fs->dir_free_block + (uint64_t)k * fs->dirblocks;
}

/* Free block `k`, new, in the buffer: its `firstdb`, no slots in use. The
 * block is taken and mapped; the caller fills and stores it. */
static int free_block_new(struct xfs *fs, struct xfs_inode *dir, uint32_t k, uint64_t now_s,
                          uint64_t *fsb) {
    int rc = xfs_dir_take_block(fs, dir->ino >> fs->inopblog, fsb);
    rc = rc != KERR_OK ? rc
                       : xfs_dir_fork_insert(fs, dir, free_block_at(fs, k), *fsb, false, now_s);
    rc = rc != KERR_OK ? rc : xfs_inode_read(fs, dir->ino, dir);
    if (rc == KERR_OK) {
        build_free(fs, dir->ino, fs->dirpairs, 0); /* no slots: nothing is read */
        xfs_put_be32(fs->dirblock + free_count_at(fs, 0), k * free_per_block(fs));
    }
    return rc;
}

/* Sets slot `slot` of the free block in the buffer to `best`, the slots it
 * skips over to "no data block", and the counts. */
static void free_put(struct xfs *fs, uint32_t slot, uint32_t best) {
    uint32_t n = xfs_be32(fs->dirblock + free_count_at(fs, 1u));
    for (; n < slot; n++) {
        xfs_put_be16(fs->dirblock + fs->dir_free_hdr + n * XFS_DIR2_LEAF_BEST_SIZE,
                     XFS_DIR2_NULL_DATAOFF);
    }
    xfs_put_be16(fs->dirblock + fs->dir_free_hdr + slot * XFS_DIR2_LEAF_BEST_SIZE, (uint16_t)best);
    if (n < slot + 1u) {
        xfs_put_be32(fs->dirblock + free_count_at(fs, 1u), slot + 1u);
    }
    free_trim(fs);
    free_recount(fs);
}

/* Records data block `which`'s largest free span in the free block that
 * describes it, making that block when it is the first of its range, and
 * giving back one past the first that no longer names any data block — as
 * Linux does; `xfs_repair` reads an empty one as damage.
 *
 * Loaded, edited and stored in one function because the directory buffer is
 * shared: anything that touched a data block in between would overwrite it.
 * The same reason `edit_leaf` is one function in the leaf format. */
static int free_set(struct xfs *fs, struct xfs_inode *dir, uint32_t which, uint32_t best,
                    uint64_t now_s) {
    uint32_t k = which / free_per_block(fs), slot = which % free_per_block(fs);
    uint64_t fsb = 0;
    int rc = xfs_dir_raw_load(fs, dir, free_block_at(fs, k), &fsb);
    if (rc == KERR_NOENT && k > 0 && best == XFS_DIR2_NULL_DATAOFF) {
        return KERR_OK; /* the block describing it was given back: nothing named */
    }
    if (rc == KERR_NOENT && k > 0) {
        rc = free_block_new(fs, dir, k, now_s, &fsb);
    }
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfs_be32(fs->dirblock) != xfs_magic_for(fs, XFS_BLK_DIR_FREE)) {
        return KERR_IO;
    }
    free_put(fs, slot, best);
    if (k > 0 && xfs_be32(fs->dirblock + free_count_at(fs, 2u)) == 0) {
        rc = xfs_dir_fork_remove(fs, dir, free_block_at(fs, k), now_s);
        return rc != KERR_OK ? rc : xfs_inode_read(fs, dir->ino, dir);
    }
    return xfs_dir_raw_store(fs, fsb);
}

/* ── the conversion ───────────────────────────────────────────────────── */

/* Moves the leaf's `bests` array into a free block of its own.
 *
 * A pass of its own, re-reading the leaf, rather than holding both halves
 * at once: the array can be eight kilobytes and the frame guard refused it
 * on a stack — the third time that rule has caught this work and the third
 * time it was right. The buffer is shared, so the cost is one cached read
 * and the gain is that no copy of the directory lives on a stack. */
static int free_block_from_leaf(struct xfs *fs, const struct xfs_inode *dir, uint64_t f_fsb) {
    uint8_t *bests = fs->dirpairs;
    uint64_t leaf_fsb = 0;
    uint32_t bestcount;
    int rc = xfs_dir_raw_load(fs, dir, fs->dir_leaf_block, &leaf_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    bestcount = xfs_be32(fs->dirblock + fs->dirblocksize - XFS_DIR2_LEAF_TAIL_SIZE);
    if (bestcount * XFS_DIR2_LEAF_BEST_SIZE > XFS_DIRPAIRS_BYTES) {
        return KERR_NOSPC;
    }
    xfs_copy_bytes(bests,
                   fs->dirblock + fs->dirblocksize - XFS_DIR2_LEAF_TAIL_SIZE -
                       bestcount * XFS_DIR2_LEAF_BEST_SIZE,
                   bestcount * XFS_DIR2_LEAF_BEST_SIZE);
    build_free(fs, dir->ino, bests, bestcount);
    return xfs_dir_raw_store(fs, f_fsb);
}

/* Writes one of the two leaves the conversion splits into. */
static int write_leafn(struct xfs *fs, const struct xfs_inode *dir, const uint8_t *pairs,
                       uint32_t count, uint64_t forw, uint64_t back, uint64_t fsb) {
    xfs_da_header(fs, xfs_magic_for(fs, XFS_BLK_DIR_LEAFN), dir->ino, count, forw, back);
    xfs_copy_bytes(fs->dirblock + fs->da_hdr, pairs, count * XFS_DA3_ENTRY_SIZE);
    xfs_reseal(fs, fs->dirblock, fs->dirblocksize, XFS_DA3_CRC_OFF);
    return xfs_dir_raw_store(fs, fsb);
}

/* Turns the block that was the leaf into the node root naming both leaves.
 *
 * **The root keeps its block number**, which is why the conversion moves
 * the leaf's contents out rather than putting the root somewhere new: a
 * search starts at `dir_leaf_block` and would have no way to find a root
 * that had moved. */
static int write_root(struct xfs *fs, const struct xfs_inode *dir, const uint8_t *pairs,
                      uint32_t half, uint32_t count, uint64_t root_fsb) {
    uint8_t *e = fs->dirblock + fs->da_hdr;
    xfs_da_header(fs, xfs_magic_for(fs, XFS_BLK_DA_NODE), dir->ino, 2u, 0, 0);
    xfs_put_be16(fs->dirblock + fs->da_level, 1u);
    xfs_put_be32(e, top_hash(pairs, half));
    xfs_put_be32(e + 4u, (uint32_t)leaf_n(fs, 0));
    xfs_put_be32(e + XFS_DA3_ENTRY_SIZE, top_hash(pairs, count));
    xfs_put_be32(e + XFS_DA3_ENTRY_SIZE + 4u, (uint32_t)leaf_n(fs, 1));
    xfs_reseal(fs, fs->dirblock, fs->dirblocksize, XFS_DA3_CRC_OFF);
    return xfs_dir_raw_store(fs, root_fsb);
}

int xfs_dir_to_node(struct xfs *fs, const struct xfs_inode *dir, uint64_t now_s) {
    uint8_t *pairs = fs->dirpairs;
    uint32_t count, half;
    uint64_t root_fsb = 0, a_fsb = 0, b_fsb = 0, f_fsb = 0;
    int rc = xfs_dir_raw_load(fs, dir, fs->dir_leaf_block, &root_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfs_be16(fs->dirblock + XFS_DA3_MAGIC_OFF) != xfs_magic_for(fs, XFS_BLK_DIR_LEAF1)) {
        return KERR_NOTSUP; /* already a node, or not an index at all */
    }
    count = xfs_be16(fs->dirblock + fs->da_count);
    if (count * XFS_DA3_ENTRY_SIZE > XFS_DIRPAIRS_BYTES) {
        return KERR_NOSPC;
    }
    rc = xfs_dir_take_block(fs, dir->ino >> fs->inopblog, &a_fsb);
    rc = rc != KERR_OK ? rc : xfs_dir_take_block(fs, a_fsb, &b_fsb);
    rc = rc != KERR_OK ? rc : xfs_dir_take_block(fs, a_fsb, &f_fsb);
    /* The free block first, while the leaf still has the `bests` array in
     * it: the next step overwrites the leaf and the buffer is shared. */
    rc = rc != KERR_OK ? rc : free_block_from_leaf(fs, dir, f_fsb);
    rc = rc != KERR_OK ? rc : xfs_dir_raw_load(fs, dir, fs->dir_leaf_block, &root_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_copy_bytes(pairs, fs->dirblock + fs->da_hdr, count * XFS_DA3_ENTRY_SIZE);
    half = count / 2u;
    /* Chained forward, so that a run of equal hashes split across the
     * boundary is followed through `forw` — which the reader already does. */
    rc = write_leafn(fs, dir, pairs, half, leaf_n(fs, 1), 0, a_fsb);
    rc = rc != KERR_OK ? rc
                       : write_leafn(fs, dir, pairs + half * XFS_DA3_ENTRY_SIZE,
                                     count - half, 0, leaf_n(fs, 0), b_fsb);
    rc = rc != KERR_OK ? rc : write_root(fs, dir, pairs, half, count, root_fsb);
    rc = rc != KERR_OK ? rc : xfs_dir_fork_insert(fs, dir, leaf_n(fs, 0), a_fsb, false, now_s);
    rc = rc != KERR_OK ? rc : xfs_dir_fork_insert(fs, dir, leaf_n(fs, 1), b_fsb, false, now_s);
    return rc != KERR_OK ? rc
                         : xfs_dir_fork_insert(fs, dir, fs->dir_free_block, f_fsb, false,
                                               now_s);
}

uint16_t xfs_dir_index_shape(struct xfs *fs, const struct xfs_inode *dir) {
    uint64_t fsb = 0;
    if (xfs_dir_raw_load(fs, dir, fs->dir_leaf_block, &fsb) != KERR_OK) {
        return 0;
    }
    return (uint16_t)xfs_be16(fs->dirblock + XFS_DA3_MAGIC_OFF);
}

/* ── adding and removing once it is a node ───────────────────────────── */

/* Splits a full leaf — the path's — in two: the new leaf takes the
 * **upper** hashes and goes after the old one in the chain, so `forw`/`back`
 * stay in hash order and a run of equal hashes crossing the boundary is
 * still followed. The index above gains an entry for it (`xfs_da_add_child`,
 * which splits nodes up to the root as they fill).
 *
 * The fork gains the new leaf before the index is touched, and the inode
 * is re-read then: the index's blocks are found through the fork, and a
 * copy of the inode from before the change found the wrong one once the
 * fork was a b-tree whose leaves a store rebalances — a create at 108,698
 * names answered "no such file". */
static int split_leaf(struct xfs *fs, struct xfs_inode *dir, struct xfs_da_path *p,
                      uint64_t now_s) {
    uint8_t *pairs = fs->dirpairs;
    uint32_t count, half;
    uint64_t old_fsb = 0, new_fsb = 0, forw, at = 0;
    int rc = xfs_da_next_block(fs, dir, &at);
    rc = rc != KERR_OK ? rc : xfs_dir_raw_load(fs, dir, p->leaf, &old_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    count = xfs_be16(fs->dirblock + fs->da_count);
    forw = xfs_be32(fs->dirblock + XFS_DA3_FORW_OFF);
    if (count * XFS_DA3_ENTRY_SIZE > XFS_DIRPAIRS_BYTES) {
        return KERR_NOSPC;
    }
    xfs_copy_bytes(pairs, fs->dirblock + fs->da_hdr, count * XFS_DA3_ENTRY_SIZE);
    half = count / 2u;
    rc = xfs_dir_take_block(fs, dir->ino >> fs->inopblog, &new_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_put_be16(fs->dirblock + fs->da_count, (uint16_t)half);
    xfs_put_be32(fs->dirblock + XFS_DA3_FORW_OFF, (uint32_t)at);
    xfs_reseal(fs, fs->dirblock, fs->dirblocksize, XFS_DA3_CRC_OFF);
    rc = xfs_dir_raw_store(fs, old_fsb);
    rc = rc != KERR_OK ? rc
                       : write_leafn(fs, dir, pairs + half * XFS_DA3_ENTRY_SIZE, count - half,
                                     forw, p->leaf, new_fsb);
    rc = rc != KERR_OK ? rc : xfs_da_set_sibling(fs, dir, forw, XFS_DA3_BACK_OFF, (uint32_t)at);
    rc = rc != KERR_OK ? rc : xfs_dir_fork_insert(fs, dir, at, new_fsb, false, now_s);
    rc = rc != KERR_OK ? rc : xfs_inode_read(fs, dir->ino, dir);
    return rc != KERR_OK
               ? rc
               : xfs_da_add_child(fs, dir, p,
                                  &(struct xfs_da_split){top_hash(pairs, half), at,
                                                         top_hash(pairs, count)},
                                  now_s);
}

/* Re-reads the inode in place.
 *
 * **Called after anything that changes the fork**, because a caller's copy
 * of an inode is only valid until the inode changes — and growing a
 * directory changes it repeatedly: a data block here, a leaf there, three
 * extents during a conversion. Mapping a block that was just added through
 * a fork that predates it answers `KERR_NOENT` for a block that certainly
 * exists, which on the board read as a *create* reporting "no such file". */
static int refresh(struct xfs *fs, struct xfs_inode *ip) {
    return xfs_inode_read(fs, ip->ino, ip);
}

/* The path to the leaf `hash` goes in, with that leaf loaded — split first
 * when it is full, after which the hash may belong in the new half. */
static int leaf_with_room(struct xfs *fs, struct xfs_inode *dir, uint32_t hash,
                          struct xfs_da_path *p, uint64_t *leaf_fsb, uint64_t now_s) {
    int rc = xfs_da_descend(fs, dir, hash, p);
    rc = rc != KERR_OK ? rc : xfs_dir_raw_load(fs, dir, p->leaf, leaf_fsb);
    if (rc != KERR_OK || xfs_be16(fs->dirblock + fs->da_count) < leafn_capacity(fs)) {
        return rc;
    }
    rc = split_leaf(fs, dir, p, now_s);
    fs->leaf_splits += rc == KERR_OK ? 1u : 0u;
    rc = rc != KERR_OK ? rc : refresh(fs, dir);
    rc = rc != KERR_OK ? rc : xfs_da_descend(fs, dir, hash, p);
    return rc != KERR_OK ? rc : xfs_dir_raw_load(fs, dir, p->leaf, leaf_fsb);
}

/* Puts a hash into the leaf that owns it. If it went in at the end, the
 * leaf's top hash has moved and every key above it that names it must say
 * so — otherwise a later search for that name stops one child early. */
static int leaf_insert(struct xfs *fs, struct xfs_inode *dir, uint32_t hash,
                       uint32_t address, uint64_t now_s) {
    struct xfs_da_path p;
    uint64_t leaf_fsb = 0;
    uint32_t count;
    int rc = leaf_with_room(fs, dir, hash, &p, &leaf_fsb, now_s);
    if (rc != KERR_OK) {
        return rc;
    }
    count = xfs_be16(fs->dirblock + fs->da_count);
    xfs_dir_pairs_insert(fs->dirblock + fs->da_hdr, &count, hash, address);
    xfs_put_be16(fs->dirblock + fs->da_count, (uint16_t)count);
    xfs_reseal(fs, fs->dirblock, fs->dirblocksize, XFS_DA3_CRC_OFF);
    rc = xfs_dir_raw_store(fs, leaf_fsb);
    if (rc == KERR_OK && hash >= top_hash(fs->dirblock + fs->da_hdr, count)) {
        rc = xfs_da_set_top(fs, dir, &p, hash);
    }
    return rc;
}

int xfs_dir_node_add(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                     uint32_t namelen, uint64_t ino, uint8_t ftype, uint64_t now_s) {
    struct xfs_inode here = *dir;
    uint32_t which = 0, at = 0, best = 0, blocks = 0;
    int rc = xfs_dir_place_entry(fs, &here, name, namelen, ino, ftype, now_s, &which, &at,
                                 &best, &blocks);
    /* Placing the entry may have given the directory a data block. */
    rc = rc != KERR_OK ? rc : refresh(fs, &here);
    rc = rc != KERR_OK ? rc
                       : leaf_insert(fs, &here, xfs_dir_hashname(name, namelen),
                                     xfs_dir_address_of(fs, which, at), now_s);
    rc = rc != KERR_OK ? rc : refresh(fs, &here);
    rc = rc != KERR_OK ? rc : free_set(fs, &here, which, best, now_s);
    return rc != KERR_OK ? rc : xfs_dir_touch(fs, &here, now_s);
}

/* A hash leaf left with no hashes leaves the directory, as `xfs_da3_join`
 * takes one out: its entry in the index (and any node that empties), its
 * place in the sibling chain, and its block. Kept, it carried a key for
 * hashes it no longer had, which the checker reports for every such leaf.
 * The leaf is in the buffer. */
static int drop_empty_leaf(struct xfs *fs, struct xfs_inode *dir, struct xfs_da_path *p,
                           uint64_t now_s) {
    uint32_t forw = xfs_be32(fs->dirblock + XFS_DA3_FORW_OFF);
    uint32_t back = xfs_be32(fs->dirblock + XFS_DA3_BACK_OFF);
    int rc = xfs_da_drop_child(fs, dir, p, now_s);
    rc = rc != KERR_OK ? rc : xfs_da_set_sibling(fs, dir, back, XFS_DA3_FORW_OFF, forw);
    rc = rc != KERR_OK ? rc : xfs_da_set_sibling(fs, dir, forw, XFS_DA3_BACK_OFF, back);
    rc = rc != KERR_OK ? rc : xfs_dir_fork_remove(fs, dir, p->leaf, now_s);
    return rc != KERR_OK ? rc : refresh(fs, dir);
}

/* Where in the leaf in the buffer the pair carrying `address` is; `count`
 * when it is not there. The address identifies it, not the hash: two names
 * can hash alike and only one of them is being removed. */
static uint32_t pair_with(struct xfs *fs, uint32_t count, uint32_t address) {
    for (uint32_t i = 0; i < count; i++) {
        if (xfs_be32(fs->dirblock + fs->da_hdr + i * XFS_DA3_ENTRY_SIZE + 4u) == address) {
            return i;
        }
    }
    return count;
}

/* The leaf holding `address`: the one `hash` descends to, or one after it
 * along `forw`, since a run of equal hashes can continue into the next. */
static int leaf_holding(struct xfs *fs, const struct xfs_inode *dir, uint32_t hash,
                        uint32_t address, uint64_t *leaf) {
    struct xfs_da_path p;
    uint64_t fsb = 0;
    int rc = xfs_da_descend(fs, dir, hash, &p);
    *leaf = p.leaf;
    for (uint32_t hops = 0; rc == KERR_OK && hops < 1024u; hops++) {
        uint32_t count;
        rc = xfs_dir_raw_load(fs, dir, *leaf, &fsb);
        count = rc == KERR_OK ? xfs_be16(fs->dirblock + fs->da_count) : 0;
        if (rc != KERR_OK || pair_with(fs, count, address) < count) {
            return rc;
        }
        *leaf = xfs_be32(fs->dirblock + XFS_DA3_FORW_OFF);
        rc = *leaf == 0 ? KERR_NOENT : KERR_OK;
    }
    return rc == KERR_OK ? KERR_IO : rc;
}

/* Takes a hash out of whichever leaf holds it. The path is found first,
 * because finding it uses the buffer; the leaf is then loaded and edited.
 * When the highest hash in the leaf goes, the keys above it come down to
 * the new highest: the checker requires them equal. */
static int leaf_remove(struct xfs *fs, struct xfs_inode *dir, uint32_t hash, uint32_t address,
                       uint64_t now_s) {
    struct xfs_da_path p;
    uint64_t leaf = 0, fsb = 0;
    uint32_t count, found;
    int rc = leaf_holding(fs, dir, hash, address, &leaf);
    rc = rc != KERR_OK ? rc : xfs_da_find(fs, dir, hash, leaf, &p);
    rc = rc != KERR_OK ? rc : xfs_dir_raw_load(fs, dir, leaf, &fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    count = xfs_be16(fs->dirblock + fs->da_count);
    found = pair_with(fs, count, address);
    if (found == count) {
        return KERR_IO;
    }
    for (uint32_t i = found; i + 1u < count; i++) {
        xfs_copy_bytes(fs->dirblock + fs->da_hdr + i * XFS_DA3_ENTRY_SIZE,
                       fs->dirblock + fs->da_hdr + (i + 1u) * XFS_DA3_ENTRY_SIZE,
                       XFS_DA3_ENTRY_SIZE);
    }
    xfs_put_be16(fs->dirblock + fs->da_count, (uint16_t)(count - 1u));
    if (count == 1u && !xfs_da_only_leaf(&p)) {
        return drop_empty_leaf(fs, dir, &p, now_s);
    }
    xfs_reseal(fs, fs->dirblock, fs->dirblocksize, XFS_DA3_CRC_OFF);
    rc = xfs_dir_raw_store(fs, fsb);
    return rc == KERR_OK && found + 1u == count && count > 1u
               ? xfs_da_set_top(fs, dir, &p, top_hash(fs->dirblock + fs->da_hdr, count - 1u))
               : rc;
}

/* Finishes a removal from data block `which`, as the leaf format's does:
 * the hash out of its leaf, and the block either stored or — empty now —
 * given back, its slot in the free block emptied. */
static int node_after_removal(struct xfs *fs, struct xfs_inode *dir, const char *name,
                              uint32_t namelen, uint64_t fsb, uint32_t which, uint32_t at,
                              uint64_t now_s) {
    bool empty = which != 0 && xfs_dir_block_is_empty(fs, fs->dirblock);
    uint32_t best = empty ? XFS_DIR2_NULL_DATAOFF : xfs_dir_largest_free(fs, fs->dirblock);
    int rc = empty ? KERR_OK : xfs_dir_raw_store(fs, fsb);
    rc = rc != KERR_OK ? rc
                       : leaf_remove(fs, dir, xfs_dir_hashname(name, namelen),
                                     xfs_dir_address_of(fs, which, at), now_s);
    rc = rc != KERR_OK ? rc : free_set(fs, dir, which, best, now_s);
    rc = rc != KERR_OK || !empty ? rc
                                 : xfs_dir_fork_remove(fs, dir, (uint64_t)which * fs->dirblocks,
                                                       now_s);
    return rc != KERR_OK ? rc : xfs_dir_touch(fs, dir, now_s);
}

/* The data block holding `name`, loaded, with the name taken out of it:
 * the one the hash index's address names, or — if the index cannot say —
 * whichever one a scan of them all finds it in. The scan read every data
 * block per removal: 2.7 ms a name at 300,000 names. */
static int take_name_out(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                         uint32_t namelen, uint64_t *ino, uint64_t *fsb, uint32_t *which,
                         uint32_t *at) {
    uint32_t blocks = xfs_dir_data_count(fs, dir), address = 0;
    uint64_t found = 0;
    int rc = xfs_dir_hash_find(fs, dir, name, namelen, &found, &address);
    if (rc == KERR_OK) {
        *which = (uint32_t)((uint64_t)address * XFS_DIR2_DATA_ALIGN / fs->dirblocksize);
        rc = xfs_dir_raw_load(fs, dir, (uint64_t)*which * fs->dirblocks, fsb);
        rc = rc != KERR_OK ? rc : xfs_dir_take_out(fs, name, namelen, ino, at);
        if (rc == KERR_OK) {
            return KERR_OK;
        }
    }
    for (*which = 0, rc = KERR_NOENT; *which < blocks; (*which)++) {
        rc = xfs_dir_raw_load(fs, dir, (uint64_t)*which * fs->dirblocks, fsb);
        if (rc == KERR_NOENT) {
            continue; /* a block given back earlier: a hole */
        }
        rc = rc != KERR_OK ? rc : xfs_dir_take_out(fs, name, namelen, ino, at);
        if (rc != KERR_NOENT) {
            return rc;
        }
    }
    return rc;
}

int xfs_dir_node_remove(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                        uint32_t namelen, uint64_t now_s, uint64_t *ino) {
    struct xfs_inode here = *dir; /* re-read as the index changes the fork */
    uint64_t fsb = 0;
    uint32_t at = 0, which = 0;
    int rc = take_name_out(fs, dir, name, namelen, ino, &fsb, &which, &at);
    if (rc != KERR_OK) {
        return rc;
    }
    return node_after_removal(fs, &here, name, namelen, fsb, which, at, now_s);
}
