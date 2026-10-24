/* xfs_internal.h — what the parts of the reader share: the big-endian
 * accessors, the block-number arithmetic, and the one function that gets a
 * metadata block and refuses to hand it over unverified. */
#ifndef MINIKERN_CORE_XFS_XFS_INTERNAL_H
#define MINIKERN_CORE_XFS_XFS_INTERNAL_H

#include "core/xfs/xfs.h"
#include "core/xfs/xfs_format.h"

static inline uint16_t xfs_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t xfs_be32(const uint8_t *p) {
    return ((uint32_t)xfs_be16(p) << 16) | xfs_be16(p + 2);
}

static inline uint64_t xfs_be64(const uint8_t *p) {
    return ((uint64_t)xfs_be32(p) << 32) | xfs_be32(p + 4);
}

/* The checksums are the one little-endian field in the format. */
static inline uint32_t xfs_le32(const uint8_t *p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

/* A filesystem block number is (ag << agblklog) | agbno, and the group's
 * blocks are agblocks apart on the device — which is not the same thing,
 * because agblocks need not be a power of two. */
static inline uint64_t xfs_fsb_to_block(const struct xfs *fs, uint64_t fsb) {
    uint64_t mask = ((uint64_t)1 << fs->agblklog) - 1u;
    return (fsb >> fs->agblklog) * fs->agblocks + (fsb & mask);
}

static inline uint32_t xfs_fsb_ag(const struct xfs *fs, uint64_t fsb) {
    return (uint32_t)(fsb >> fs->agblklog);
}

/* Copies `n` bytes. Shared rather than written twice: reading a file and
 * gathering a directory block both do it, and `memcpy` is not available to
 * a freestanding core that deliberately links no libc. */
static inline void xfs_copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

/* True when fsb names a block inside the filesystem. */
bool xfs_fsb_valid(const struct xfs *fs, uint64_t fsb, uint64_t count);

/* Verifies the CRC32C stored little-endian at crc_off within len bytes,
 * with the field itself counted as zero (which is how it was computed). */
bool xfs_crc_ok(const uint8_t *buf, uint32_t len, uint32_t crc_off);

/* The filesystem's UUID at uuid, or not. */
bool xfs_uuid_ok(const struct xfs *fs, const uint8_t *uuid);

/* One verified metadata block, pinned: its magic, its CRC and its UUID all
 * checked. KERR_IO when any of them is wrong, KERR_INVAL for a block
 * number outside the filesystem. A second magic may be given (directories
 * have two), or 0 for none. */
int xfs_meta_block(struct xfs *fs, uint64_t fsb, uint32_t magic, uint32_t magic2, uint32_t crc_off,
                   uint32_t uuid_off, struct bbuf **out);

/* The leaf-format attribute reader, reached from `xfs_attr_get`/`_list`
 * when the fork is not shortform (xfs_attr.c). */
int xfs_attr_leaf_get(struct xfs *fs, const struct xfs_inode *ip, const char *name,
                      uint32_t namelen, uint8_t flags, uint8_t *out, uint32_t room,
                      uint32_t *valuelen);
int xfs_attr_leaf_list(struct xfs *fs, const struct xfs_inode *ip, xfs_attr_fn fn,
                       void *self);

/* Checks the structures this reader walks, without writing anything
 * (xfs_check.c): both headers of every group, their four b-tree roots, the
 * counts each group states about itself, and the root inode.
 *
 * `KERR_OK` when the filesystem is sound enough to read, `KERR_DIRTY` with
 * a count and the first fault when it is not. Bounded by the group count,
 * not the filesystem's size — walking every inode is what `xfs_repair`
 * does and would make boot time a function of how full the disk is. */
int xfs_check(struct xfs *fs);

/* The clean-log check (xfs_log.c): KERR_OK when the log's newest record is
 * an unmount record, KERR_NOTSUP when it is anything else. */
int xfs_log_check(struct xfs *fs);

/* Replays the log into the metadata (xfs_logreplay.c), so that a filesystem
 * a crash left behind comes back **clean and writable** rather than
 * read-only.
 *
 * All or nothing: a transaction without its commit is dropped, an item type
 * it does not understand abandons the whole replay, and nothing reaches the
 * disk until the entire log has been read without complaint. An abort is
 * not a failure — the caller falls back to the read-only mount. */
int xfs_log_replay(struct xfs *fs);

/* Converts a logged inode core into an on-disk one (xfs_logwrite.c).
 *
 * A logged inode keeps each field's offset but writes it in the host's byte
 * order, so replay cannot simply copy the bytes in. Only the fields the log
 * carries are written; the caller reseals. */
void xfs_log_dinode_to_disk(uint8_t *di, const uint8_t *logged);

/* Writes an unmount record, which is what makes a log clean. */
int xfs_log_mark_clean(struct xfs *fs);

/* Where an inode lives: the device block holding its chunk, and its byte
 * offset within that block. The same arithmetic the reader uses, exposed
 * because a write has to put the inode back where it came from. */
void xfs_inode_place(const struct xfs *fs, uint64_t ino, uint64_t *device_block, uint32_t *boffset);

/* One inode in a transaction: which inode, where on the device its chunk
 * is and where in that chunk it sits — what recovery would need to find it
 * — and its on-disk core. */
struct xfs_log_inode_item {
    uint64_t ino;
    uint64_t chunk_block;
    uint32_t boffset;
    const uint8_t *core; /* the on-disk core, big-endian; converted on the way in */
};

#define XFS_LOG_MAX_ITEMS 2u

/* ── the group headers and the btrees (xfs_create.c) ───────────────────── */

/* The group's first block, where its four header sectors live. */
uint64_t xfs_group_block(const struct xfs *fs, uint32_t agno);

/* Recomputes a self-describing structure's checksum over len bytes. */
void xfs_reseal(uint8_t *at, uint32_t len, uint32_t crc_off);

/* The group's inode header, with its magic, checksum and UUID checked. */
int xfs_agi_get(struct xfs *fs, uint32_t agno, struct bbuf **out, uint8_t **agi);

/* One of a group's btree roots, which must be a lone leaf: KERR_NOTSUP for
 * a tree with branches, because walking one is not implemented. */
int xfs_tree_get(struct xfs *fs, uint32_t agno, uint32_t block, uint32_t magic, struct bbuf **out);

/* The inode-btree record whose chunk holds this group-relative inode, or
 * NULL. Found by range: a chunk does not begin at a multiple of its size. */
uint8_t *xfs_tree_record_for(const struct bbuf *b, uint32_t agino);

/* The superblock's free-inode counter, moved by delta. */
int xfs_count_inodes_free(struct xfs *fs, int32_t delta);

/* Big-endian writers, shared by everything that changes metadata. */
void xfs_put_be16(uint8_t *p, uint16_t v);
void xfs_put_be32(uint8_t *p, uint32_t v);
void xfs_put_be64(uint8_t *p, uint64_t v);

/* ── the free-space btrees (xfs_alloc.c) ───────────────────────────────── */

/* Takes one block from a group: `want`, if that block is free, and
 * otherwise the first block of the largest free extent. *got is the block
 * it took. KERR_NOSPC when the group has none, KERR_NOTSUP for a tree with
 * branches. */
int xfs_alloc_block(struct xfs *fs, uint32_t agno, uint32_t want, uint32_t *got);

/* Gives an extent back, joined to whatever it abuts. */
int xfs_free_extent(struct xfs *fs, uint32_t agno, uint32_t start, uint32_t count);

/* Writes these inodes to the log as one committed transaction, then an
 * unmount record after it so the log is clean again (xfs_logwrite.c).
 *
 * Several items rather than one because a change usually is several: a
 * created file is a new inode *and* its parent's directory, and the point
 * of a transaction is that they arrive together or not at all. */
int xfs_log_inodes(struct xfs *fs, const struct xfs_log_inode_item *items, uint32_t n);

/* One extent record, unpacked. */
struct xfs_extent {
    uint64_t startoff;   /* logical block within the file */
    uint64_t startblock; /* filesystem block */
    uint32_t blockcount;
    bool unwritten; /* allocated but never written: reads as zeroes */
};

/* Unpacks the sixteen-byte record at p. */
void xfs_extent_unpack(const uint8_t *p, struct xfs_extent *out);

/* Packs one back, which is the exact inverse (there is a round-trip test). */
void xfs_extent_pack(uint8_t *p, const struct xfs_extent *e);

/* Maps a logical block of the **attribute** fork, which is a separate
 * extent list in the inode's literal area above `di_forkoff`.
 *
 * The data fork's mapper cannot answer for it: reading an attribute block
 * through `xfs_bmap` would return a block of the file's own contents. */
int xfs_attr_bmap(struct xfs *fs, const struct xfs_inode *ip, uint64_t logical,
                  struct xfs_extent *out);

/* Takes `count` contiguous free blocks from a group, preferring to start at
 * `want` and otherwise taking the **smallest** free extent that fits.
 *
 * Best fit rather than largest: taking the largest run for every request
 * breaks up the one a later caller may need whole, which did not matter
 * while every allocation was a single block. */
int xfs_alloc_extent(struct xfs *fs, uint32_t agno, uint32_t count, uint32_t want, uint32_t *got);

/* Adds a chunk of sixty-four inodes to a group, so that a filesystem whose
 * existing chunks are full can still make a file. Reports the first inode
 * number of the new chunk. */
int xfs_inode_chunk_add(struct xfs *fs, uint32_t *agno, uint32_t *startino);

/* ── the pieces a directory data block is made of ──────────────────────── */

/* Allocates `count` contiguous blocks, preferring the group in `*agno` and
 * trying **every other group** before giving up; `*agno` comes back as the
 * group that answered.
 *
 * This is what every caller that does not need a particular group should
 * use. `xfs_alloc_extent` asks one group and answers `KERR_NOSPC` for the
 * whole filesystem if that one is full, which is a refusal with the disk
 * mostly empty. `want` is a locality hint within the preferred group and is
 * dropped once the search leaves it, because an address inside one group
 * means nothing in another. */
int xfs_alloc_somewhere(struct xfs *fs, uint32_t *agno, uint32_t count, uint32_t want,
                        uint32_t *got);

/* Marks a directory's own inode as changed: its times, and nothing else,
 * because an edit inside an existing block moves neither its size nor its
 * block count. */
int xfs_dir_touch(struct xfs *fs, const struct xfs_inode *dir, uint64_t now_s);

/* An entry's bytes in a data block: inumber, name length, name, the type
 * byte when the filesystem has them, and the two-byte tag, rounded to
 * eight. */
uint32_t xfs_dir_entry_size(const struct xfs *fs, uint32_t namelen);

/* Writes one entry at `at` and returns what it took. The **tag** at its end
 * is the field a writer forgets, because nothing this kernel reads uses it
 * and `xfs_repair` does. */
uint32_t xfs_dir_put_entry(const struct xfs *fs, uint8_t *block, uint32_t at, uint64_t ino,
                           const char *name, uint32_t namelen, uint8_t ftype);

/* Whether the name at `here` is `name`. Shared because three formats'
 * removals all compare one. */
static inline bool xfs_dir_name_here(const uint8_t *here, const char *name, uint32_t namelen) {
    for (uint32_t i = 0; i < namelen; i++) {
        if (here[i] != (uint8_t)name[i]) {
            return false;
        }
    }
    return true;
}

/* Marks [at, at+len) free, the way a reader expects to find it. */
void xfs_dir_put_free(uint8_t *block, uint32_t at, uint32_t len);

/* A free span of at least `need` bytes below `end`, or 0.
 *
 * `end` is where the data area stops, which differs by format: a block
 * directory's data stops where its hash array begins and moves as that
 * array grows, while a data block of a leaf directory runs to the end of
 * the block. It uses the part of a span that is still free rather than
 * requiring all of it to lie below `end`. */
uint32_t xfs_dir_find_room(const struct xfs *fs, const uint8_t *block, uint32_t end,
                           uint32_t need);

/* ── pieces shared by the leaf and node shapes ────────────────────────── */

/* Reads one of the directory's blocks into `fs->dirblock` and reports the
 * filesystem block it came from; writes it back.
 *
 * There is **one** buffer, so a step that needs two blocks at once has to
 * write one before reading the other — which is why the conversions read
 * the way they do rather than the way they would be described. */
int xfs_dir_raw_load(struct xfs *fs, const struct xfs_inode *dir, uint64_t logical,
                     uint64_t *fsb);
int xfs_dir_raw_store(struct xfs *fs, uint64_t fsb);

/* A fresh block for the directory, from any group that has one. */
int xfs_dir_take_block(struct xfs *fs, uint64_t near, uint64_t *fsb);

/* Adds an extent to the directory's fork in sorted order, merging where it
 * can; `grows_data` also moves the directory's size, which only a data
 * block does. */
int xfs_dir_fork_insert(struct xfs *fs, const struct xfs_inode *dir, uint64_t logical,
                        uint64_t fsb, bool grows_data, uint64_t now_s);

/* The largest free span in the data block held in the buffer: what the
 * `bests` array records. */
uint32_t xfs_dir_largest_free(const struct xfs *fs, const uint8_t *block);

/* Inserts into a sorted array of `{hash, address}` pairs held as bytes. */
void xfs_dir_pairs_insert(uint8_t *pairs, uint32_t *count, uint32_t hash, uint32_t address);

/* How many data blocks the directory has, and the address a leaf entry
 * carries for an entry at `at` in data block `which` — a byte offset within
 * the directory's data, divided by eight. */
uint32_t xfs_dir_data_count(const struct xfs *fs, const struct xfs_inode *dir);
uint32_t xfs_dir_address_of(const struct xfs *fs, uint32_t which, uint32_t at);

/* Puts an entry in a data block that has room, growing the directory by a
 * block when none has; reports which block took it, where, that block's
 * largest remaining free span, and how many data blocks there now are. */
int xfs_dir_place_entry(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                        uint32_t namelen, uint64_t ino, uint8_t ftype, uint64_t now_s,
                        uint32_t *which, uint32_t *at, uint32_t *best, uint32_t *blocks);

/* Finds a name in the data block held in the buffer and frees its entry. */
int xfs_dir_take_out(struct xfs *fs, const char *name, uint32_t namelen, uint64_t *ino,
                     uint32_t *at);

/* The magic of a directory's index block: `XFS_DIR3_LEAF1_MAGIC`,
 * `XFS_DA3_NODE_MAGIC`, or 0 when there is no index block.
 *
 * **Asked before dispatching, never discovered by trying.** The add paths
 * write the entry into a data block before they touch the index, so calling
 * the wrong one and falling back on its error leaves an entry on disk that
 * the other path then writes again — a duplicate the first version of this
 * produced on its first board run. A probe that only reads is the way to
 * choose between operations that write. */
uint16_t xfs_dir_index_shape(struct xfs *fs, const struct xfs_inode *dir);

/* Turns a leaf-format directory into a node-format one, adding `name` as
 * part of the same step, and adds to and removes from one afterwards. */
int xfs_dir_to_node(struct xfs *fs, const struct xfs_inode *dir, uint64_t now_s);
int xfs_dir_node_add(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                     uint32_t namelen, uint64_t ino, uint8_t ftype, uint64_t now_s);
int xfs_dir_node_remove(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                        uint32_t namelen, uint64_t now_s, uint64_t *ino);

/* Turns a block-format directory into a leaf-format one, adding `name` as
 * part of the same step — for the same reason the shortform conversion
 * does. KERR_NOSPC when even leaf format would not hold it, which is where
 * node format begins. */
int xfs_dir_to_leaf(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                    uint32_t namelen, uint64_t ino, uint8_t ftype, uint64_t now_s);

/* Adds to and removes from a leaf-format directory: the entry goes in a
 * data block, its hash in the leaf block. */
int xfs_dir_leaf_add(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                     uint32_t namelen, uint64_t ino, uint8_t ftype, uint64_t now_s);
int xfs_dir_leaf_remove(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                        uint32_t namelen, uint64_t now_s, uint64_t *ino);

/* Adds an entry to a directory of whatever shape, growing it when the
 * shape it has cannot hold one more. What `create`, `link` and `rename` all
 * want, so that none of them has to know about formats. */
int xfs_dir_add(struct xfs *fs, const struct xfs_inode *dir, const char *name, uint32_t namelen,
                uint64_t ino, uint8_t ftype, uint64_t now_s);

/* Removes an entry from a directory of whatever shape. */
int xfs_dir_remove(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                   uint32_t namelen, uint64_t now_s, uint64_t *ino);

/* Adds an entry to a block-format directory: into the free space, with its
 * hash inserted into the sorted array and the tail's count raised.
 * KERR_NOSPC when the block is full, which is where leaf format begins. */
int xfs_dir_block_add(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                      uint32_t namelen, uint64_t ino, uint8_t ftype, uint64_t now_s);

/* Removes one from a block-format directory, leaving the space free and the
 * hash array one shorter. */
int xfs_dir_block_remove(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                         uint32_t namelen, uint64_t now_s, uint64_t *ino);

/* Turns a shortform directory into a block-format one, adding `name` as
 * part of the same step.
 *
 * It takes the new entry because the conversion exists *because* of it: the
 * block is built once, from the old entries and the new one together, and
 * converting first and adding afterwards would write the block twice and
 * leave a directory briefly holding neither answer. KERR_NOSPC when even
 * one block would not hold them all, which is where leaf format begins. */
int xfs_dir_to_block(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                     uint32_t namelen, uint64_t ino, uint8_t ftype, uint64_t now_s);

/* Adds or removes one entry of a shortform directory, and touches its
 * times. Shared because `create`, `unlink`, `link` and `rename` are four
 * arrangements of the same two operations. */
int xfs_dir_sf_add(struct xfs *fs, const struct xfs_inode *dir, const char *name, uint32_t namelen,
                   uint64_t ino, uint8_t ftype, uint64_t now_s);
int xfs_dir_sf_remove(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                      uint32_t namelen, uint64_t now_s, uint64_t *ino);

/* Moves an inode's link count by `delta`, resealing it. The count is what
 * says whether a file still exists when a name is removed. */
int xfs_inode_adjust_nlink(struct xfs *fs, uint64_t ino, int32_t delta);

/* One directory block's bytes, however many filesystem blocks it took.
 *
 * `held` is the cache buffer to give back, and `NULL` when the block was
 * gathered into the filesystem's buffer instead. Shared because the hash
 * index's leaf and node blocks are directory blocks too: they are the same
 * size, gathered the same way and verified the same way, and the only thing
 * that differs is what the bytes mean. */
struct xfs_dirblock {
    const uint8_t *at;
    struct bbuf *held;
};

/* The directory block at logical filesystem block `first`, verified.
 *
 * **Two functions because the two kinds of block disagree about where their
 * header is.** A *data* block carries a 32-bit magic at offset 0 with its
 * checksum at 4; an *index* block — leaf or node — is an `xfs_da3_blkinfo`
 * whose magic is 16 bits at offset 8 with its checksum at 12. One function
 * taking a magic cannot express that, and taking the data block's offsets
 * for an index block rejects every one of them. */
int xfs_dir_data_get(struct xfs *fs, const struct xfs_inode *dir, uint64_t first,
                     struct xfs_dirblock *out);
int xfs_dir_index_get(struct xfs *fs, const struct xfs_inode *dir, uint64_t first,
                      struct xfs_dirblock *out);
void xfs_dir_block_put(struct xfs *fs, struct xfs_dirblock *b);

/* The name hash the directory's index is sorted by.
 *
 * An on-disk interface fact: the index is a sorted array of these, so a
 * reader that computes them differently finds nothing. Four bytes at a time
 * with a seven-bit rotation, which is the format's definition. */
uint32_t xfs_dir_hashname(const char *name, uint32_t len);

/* The inode a name maps to, through the hash index. KERR_NOENT when the
 * name is not there, KERR_NOTSUP when this directory has no index to use. */
int xfs_dir_hash_lookup(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                        uint32_t len, uint64_t *ino);

/* The extent covering logical block off, or KERR_NOENT when off is in a
 * hole. Walks the inode's inline extent list. */
int xfs_bmap(const struct xfs *fs, const struct xfs_inode *ip, uint64_t off, struct xfs_extent *out);

/* The same question for an inode whose fork is a b-tree: the extent
 * covering logical block `off`, by descending from the root in the fork.
 * KERR_NOENT for a hole, KERR_IO for a tree that does not hold together. */
int xfs_bmbt_lookup(const struct xfs *fs, const struct xfs_inode *ip, uint64_t off,
                    struct xfs_extent *out);

/* The same descent over a fork given directly, which is how the
 * **attribute** fork's b-tree is read: an inode has two forks and either
 * can be a b-tree, and the walk does not care which it is handed. */
int xfs_bmbt_lookup_in(const struct xfs *fs, const uint8_t *fork, uint32_t fork_len,
                       uint64_t off, struct xfs_extent *out);

#endif
