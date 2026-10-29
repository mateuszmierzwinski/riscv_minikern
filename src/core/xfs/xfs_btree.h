/* xfs_btree.h — a group's short-pointer B+tree, written: lookup, insert with
 * splits, delete with borrowing and merging, and keys kept true up the path.
 *
 * The free-space and inode trees here are written as single leaves and
 * refuse to grow (xfs_alloc.c says why that has been enough for them). The
 * reverse-map tree cannot be treated that way: it gains a record for nearly
 * every file, and one 4 KiB leaf holds 168 — a root filesystem passes that
 * in its first directory. So this is the whole structure, kept generic over
 * the record format so the reference-count tree can be the next user.
 *
 * What it maintains, because `xfs_repair` checks every one of them:
 *   - records sorted by their low key, and every non-root block at least
 *     half full — a delete that leaves less borrows or merges;
 *   - each node key the low key of its child, and, in an **overlapping**
 *     tree, the highest high key beneath it as well;
 *   - sibling links at every level, and the root and height in the AGF.
 *
 * Blocks come from and go back to the tree's own supply (`alloc_block`,
 * `free_block`): the free list for the reverse-map tree. A caller makes sure
 * the supply is deep enough *before* an operation (`xfs_btree_worst_case`),
 * because running dry half way through a split leaves a tree no one can
 * finish. */
#ifndef MINIKERN_CORE_XFS_XFS_BTREE_H
#define MINIKERN_CORE_XFS_XFS_BTREE_H

#include "core/xfs/xfs_internal.h"

#define XFS_BTREE_MAXLEVELS 9u
#define XFS_BTREE_KEY_MAX   20u /* the largest key of any tree that uses this */
#define XFS_BTREE_REC_MAX   24u /* and the largest record */

struct xfs_btree_cur;

/* What differs between trees. Keys and records are the on-disk bytes. */
struct xfs_btree_ops {
    uint32_t magic;
    uint32_t rec_size;
    uint32_t key_size;
    bool overlapping; /* nodes carry a high key beside each low key */
    bool in_agi;        /* the root and height are in the AGI, not the AGF */
    uint32_t root_off;  /* where the header keeps the root block */
    uint32_t level_off; /* and the height */
    void (*low_key)(const uint8_t *rec, uint8_t *key);
    void (*high_key)(const uint8_t *rec, uint8_t *key);
    int (*cmp)(const uint8_t *a, const uint8_t *b);
    int (*alloc_block)(struct xfs_btree_cur *cur, uint32_t *agbno);
    int (*free_block)(struct xfs_btree_cur *cur, uint32_t agbno);
};

/* A position in one group's tree: the block and index at every level, leaf
 * first. The leaf index is -1 when the position is before every record. */
struct xfs_btree_cur {
    struct xfs *fs;
    const struct xfs_btree_ops *ops;
    uint32_t magic; /* the ops' magic, or the one this filesystem's version uses */
    uint32_t agno;
    uint32_t levels;
    uint32_t root;
    uint32_t bno[XFS_BTREE_MAXLEVELS];
    int32_t idx[XFS_BTREE_MAXLEVELS];
};

/* Reads the root and height from the group's AGF. */
int xfs_btree_open(struct xfs_btree_cur *cur, struct xfs *fs, const struct xfs_btree_ops *ops,
                   uint32_t agno);

/* Moves to the last record whose low key is at or below `key`. */
int xfs_btree_lookup_le(struct xfs_btree_cur *cur, const uint8_t *key);

/* The record at the position (KERR_NOENT before the first), and the one
 * after it, across a leaf boundary if need be (KERR_NOENT at the end). */
int xfs_btree_get(struct xfs_btree_cur *cur, uint8_t *rec);
int xfs_btree_peek_next(struct xfs_btree_cur *cur, uint8_t *rec);

/* Walks backwards from the position without moving it: `*leaf` and `*idx`
 * start as the cursor's leaf and index and step to the record before,
 * across a leaf boundary if need be; KERR_NOENT before the first. For a
 * tree whose records overlap, where the record wanted can sit behind
 * others that share its blocks. */
int xfs_btree_get_prev(struct xfs_btree_cur *cur, uint32_t *leaf, int32_t *idx, uint8_t *rec);

/* Rewrites the record at the position. Its low key must not move it out of
 * order; a change that would is a delete and an insert. */
int xfs_btree_update(struct xfs_btree_cur *cur, const uint8_t *rec);

/* Inserts after the position, which a lookup of the record's own low key
 * leaves exactly where it belongs; and deletes the record at it. Both leave
 * the cursor spent: look up again before the next operation. */
int xfs_btree_insert(struct xfs_btree_cur *cur, const uint8_t *rec);
int xfs_btree_delete(struct xfs_btree_cur *cur);

/* The blocks one insert can take from the supply — a split at every level
 * and a new root — and so what the supply must hold before one. */
uint32_t xfs_btree_worst_case(const struct xfs_btree_cur *cur);

#endif
