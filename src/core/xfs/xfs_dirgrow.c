/* A directory outgrowing its inode: shortform becomes block format.
 *
 * **The other half of writing.** Adding an entry was implemented for a
 * *shortform* directory — one whose whole content fits in the inode's
 * literal area, which is a few hundred bytes — so a directory stopped
 * accepting names at about ten of them and answered `KERR_NOSPC` with a
 * comment saying it would have to become a block directory. This is that
 * conversion, and the add and remove that work on the result.
 *
 * A block-format directory is one directory block holding, in order: a
 * 64-byte header, the entries, the free space between them, an array of
 * `{hash, address}` pairs sorted by hash, and an eight-byte tail counting
 * them. It is the same block a reader already understands — `XDB3` — which
 * is why reading needed nothing for this.
 *
 * Two things the shortform layout does not store and this must create.
 * **`.` and `..` are not entries there**: the parent is in the header and
 * the directory's own number is the inode's, and a reader synthesizes both.
 * In a block directory they are real entries, first and second. And every
 * entry needs its **hash**, because the block carries the sorted array that
 * a lookup in a larger directory will binary-search.
 *
 * What it does not do is grow a *block* directory into leaf format. That is
 * the next step up, and it is its own entry: this one takes a directory
 * from about ten names to about a hundred and twenty, which is the step
 * that stops an ordinary directory failing.
 */
#include "core/errno.h"
#include "core/xfs/xfs_format.h"
#include "core/xfs/xfs_internal.h"

#define SF_HEADER 6u

/* An entry's bytes in a data block: inumber, name length, the name, the
 * type byte when the filesystem has them, and the two-byte tag, rounded up
 * to eight. */
uint32_t xfs_dir_entry_size(const struct xfs *fs, uint32_t namelen) {
    uint32_t n = 8u + 1u + namelen + (fs->ftype ? 1u : 0u) + 2u;
    return (n + XFS_DIR2_DATA_ALIGN - 1u) & ~(XFS_DIR2_DATA_ALIGN - 1u);
}

/* Writes one entry at `at`, and returns what it took.
 *
 * The **tag** is the entry's own offset, repeated at its end: it is how a
 * reader walking the block backwards finds entry boundaries, and it is the
 * field a writer most easily forgets, because nothing this kernel reads
 * uses it. `xfs_repair` does. */
uint32_t xfs_dir_put_entry(const struct xfs *fs, uint8_t *block, uint32_t at, uint64_t ino,
                           const char *name, uint32_t namelen, uint8_t ftype) {
    uint32_t size = xfs_dir_entry_size(fs, namelen);
    xfs_put_be64(block + at, ino);
    block[at + 8u] = (uint8_t)namelen;
    for (uint32_t i = 0; i < namelen; i++) {
        block[at + 9u + i] = (uint8_t)name[i];
    }
    if (fs->ftype) {
        block[at + 9u + namelen] = ftype;
    }
    xfs_put_be16(block + at + size - 2u, (uint16_t)at);
    return size;
}

/* Marks [at, at+len) as free, the way a reader expects to find it. */
void xfs_dir_put_free(uint8_t *block, uint32_t at, uint32_t len) {
    xfs_put_be16(block + at, XFS_DIR2_DATA_FREE_TAG);
    xfs_put_be16(block + at + 2u, (uint16_t)len);
    xfs_put_be16(block + at + len - 2u, (uint16_t)at);
}

/* One `{hash, address}` pair, kept while the block is built so that they
 * can be sorted before being written. */
struct hashent {
    uint32_t hash;
    uint32_t address;
};

/* Inserts into a hash-sorted array. An insertion sort, because the array is
 * at most a block's worth and is built once. */
static void insert_sorted(struct hashent *v, uint32_t *n, uint32_t hash, uint32_t address) {
    uint32_t i = *n;
    while (i > 0 && v[i - 1u].hash > hash) {
        v[i] = v[i - 1u];
        i--;
    }
    v[i].hash = hash;
    v[i].address = address;
    (*n)++;
}

/* Finishes a data block: the free span, the sorted hashes, the tail and the
 * header's own fields. */
static void seal_block(struct xfs *fs, uint8_t *block, uint64_t ino, uint32_t used,
                       const struct hashent *ents, uint32_t count) {
    uint32_t leaf_at = fs->dirblocksize - XFS_DIR2_BLOCK_TAIL_SIZE -
                       count * XFS_DIR2_LEAF_ENTRY_SIZE;
    if (leaf_at > used) {
        xfs_dir_put_free(block, used, leaf_at - used);
        xfs_put_be16(block + 48u, (uint16_t)used);            /* best free: offset */
        xfs_put_be16(block + 50u, (uint16_t)(leaf_at - used)); /* and its length */
    }
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *e = block + leaf_at + i * XFS_DIR2_LEAF_ENTRY_SIZE;
        xfs_put_be32(e, ents[i].hash);
        xfs_put_be32(e + 4u, ents[i].address);
    }
    xfs_put_be32(block + fs->dirblocksize - XFS_DIR2_BLOCK_TAIL_SIZE, count);
    xfs_put_be32(block + fs->dirblocksize - 4u, 0); /* no stale entries */
    xfs_put_be32(block + XFS_DIR3_HDR_MAGIC, xfs_magic_for(fs, XFS_BLK_DIR_BLOCK));
    xfs_stamp_owner(fs, block, 0, XFS_DIR3_HDR_UUID, XFS_DIR3_HDR_OWNER, 0, ino);
    xfs_reseal(fs, block, fs->dirblocksize, XFS_DIR3_HDR_CRC);
}

/* Copies the shortform entries into the block being built, and records each
 * one's hash.
 *
 * Runs to 28 lines: it walks two layouts at once — the inode's entries and
 * the block's — and the offset in each depends on the last entry written,
 * so splitting it would need both cursors and both arrays passed across the
 * boundary (Rule 11, Parameters). */
static uint32_t copy_shortform(struct xfs *fs, const uint8_t *sf, uint32_t count, uint32_t at,
                               struct hashent *ents, uint32_t *nents) {
    const uint8_t *e = sf + SF_HEADER;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t namelen = e[0];
        const char *name = (const char *)e + 3u;
        /* The inumber sits after the name and the type byte: four bytes
         * while `i8count` is zero, which is every directory small enough to
         * have been shortform. */
        uint64_t ino = xfs_be32(e + 3u + namelen + 1u);
        uint8_t ftype = e[3u + namelen];
        insert_sorted(ents, nents, xfs_dir_hashname(name, namelen), at / XFS_DIR2_DATA_ALIGN);
        at += xfs_dir_put_entry(fs, fs->dirblock, at, ino, name, namelen, ftype);
        e += 1u + 2u + namelen + 1u + 4u;
    }
    return at;
}

/* The block that will hold the directory, allocated near its inode.
 *
 * Near, because a directory and its one block being in the same allocation
 * group is what keeps a lookup from crossing the disk — the allocator takes
 * the preference as a hint and falls back to the largest free extent. */
static int take_a_block(struct xfs *fs, const struct xfs_inode *dir, uint64_t *fsb) {
    uint32_t agno = xfs_fsb_ag(fs, dir->ino >> fs->inopblog);
    uint32_t got = 0;
    int rc = xfs_alloc_somewhere(fs, &agno, 1u, 0, &got);
    if (rc != KERR_OK) {
        return rc;
    }
    *fsb = ((uint64_t)agno << fs->agblklog) | got;
    return KERR_OK;
}

/* Marks the directory's own inode as changed.
 *
 * Its size and block count do not move when an entry is added to a block
 * directory — the block was already there — so this is only the times, and
 * it is separate because every edit to the block needs it. */
int xfs_dir_touch(struct xfs *fs, const struct xfs_inode *dir, uint64_t now_s) {
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    uint8_t *di;
    int rc;
    xfs_inode_place(fs, dir->ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    xfs_put_be32(di + XFS_DI_MTIME, (uint32_t)now_s);
    xfs_put_be32(di + XFS_DI_CTIME, (uint32_t)now_s);
    xfs_reseal(fs, di, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* Points the inode at its new block: one extent, one block, and the size a
 * block directory has. */
static int repoint_inode(struct xfs *fs, const struct xfs_inode *dir, uint64_t fsb,
                         uint64_t now_s) {
    struct xfs_extent e = {.startoff = 0, .startblock = fsb, .blockcount = 1};
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    uint8_t *di;
    int rc;
    xfs_inode_place(fs, dir->ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    di[XFS_DI_FORMAT] = XFS_DINODE_FMT_EXTENTS;
    xfs_extent_pack(di + fs->dinode_size, &e);
    xfs_put_be32(di + XFS_DI_NEXTENTS, 1u);
    xfs_put_be64(di + XFS_DI_NBLOCKS, 1u);
    xfs_put_be64(di + XFS_DI_SIZE, fs->dirblocksize);
    xfs_put_be32(di + XFS_DI_MTIME, (uint32_t)now_s);
    xfs_put_be32(di + XFS_DI_CTIME, (uint32_t)now_s);
    xfs_reseal(fs, di, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* Writes the built block out to the device. */
static int store_block(struct xfs *fs, uint64_t fsb) {
    struct bbuf *b;
    int rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, fsb), &b);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_copy_bytes(b->data, fs->dirblock, fs->blocksize);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

int xfs_dir_to_block(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                     uint32_t namelen, uint64_t ino, uint8_t ftype, uint64_t now_s) {
    /* **Bounded by what shortform can hold, not by what a block can.** The
     * first version sized this for a whole directory block — eight thousand
     * pairs, 64 KiB — and the frame guard refused it, which is the rule
     * about kernel stacks earning its keep. A shortform directory lives in
     * a few hundred bytes of inode and an entry costs at least nine of
     * them, so forty is already generous for "every entry it had, plus the
     * one being added". */
    struct hashent ents[48];
    uint32_t nents = 0;
    uint32_t at = fs->dir_data_hdr;
    uint32_t count = dir->fork[XFS_DIR_SF_COUNT];
    uint64_t parent = xfs_be32(dir->fork + XFS_DIR_SF_PARENT);
    uint64_t fsb = 0;
    int rc;
    /* **One filesystem block to a directory block.** A larger directory
     * block would need as many blocks allocated and as many extents
     * recorded, and `-n size=` is read but not written: refusing is the
     * honest half of a half-implemented geometry. */
    if (fs->dirblocks != 1u || dir->fork[XFS_DIR_SF_I8COUNT] != 0) {
        return KERR_NOTSUP;
    }
    if (count + 3u > sizeof ents / sizeof ents[0]) {
        return KERR_NOSPC;
    }
    for (uint32_t i = 0; i < fs->dirblocksize; i++) {
        fs->dirblock[i] = 0;
    }
    insert_sorted(ents, &nents, xfs_dir_hashname(".", 1u), at / XFS_DIR2_DATA_ALIGN);
    at += xfs_dir_put_entry(fs, fs->dirblock, at, dir->ino, ".", 1u, XFS_DIR3_FT_DIR);
    insert_sorted(ents, &nents, xfs_dir_hashname("..", 2u), at / XFS_DIR2_DATA_ALIGN);
    at += xfs_dir_put_entry(fs, fs->dirblock, at, parent, "..", 2u, XFS_DIR3_FT_DIR);
    at = copy_shortform(fs, dir->fork, count, at, ents, &nents);
    insert_sorted(ents, &nents, xfs_dir_hashname(name, namelen), at / XFS_DIR2_DATA_ALIGN);
    at += xfs_dir_put_entry(fs, fs->dirblock, at, ino, name, namelen, ftype);
    if (at + nents * XFS_DIR2_LEAF_ENTRY_SIZE + XFS_DIR2_BLOCK_TAIL_SIZE > fs->dirblocksize) {
        return KERR_NOSPC; /* it would have to become a leaf directory */
    }
    rc = take_a_block(fs, dir, &fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    seal_block(fs, fs->dirblock, dir->ino, at, ents, nents);
    rc = store_block(fs, fsb);
    return rc != KERR_OK ? rc : repoint_inode(fs, dir, fsb, now_s);
}

/* ── adding to and removing from a block-format directory ──────────────── */

/* Where the directory's single block lives, and its bytes, gathered.
 *
 * A block-format directory is one directory block at logical offset zero,
 * so this is the whole of its content — which is what makes add and remove
 * here a matter of rewriting one block rather than of allocation. */
static int the_block(struct xfs *fs, const struct xfs_inode *dir, uint64_t *fsb) {
    struct xfs_extent e;
    int rc = xfs_bmap(fs, dir, 0, &e);
    if (rc != KERR_OK) {
        return rc;
    }
    *fsb = e.startblock;
    return KERR_OK;
}

/* Reads the block into the filesystem's buffer, so it can be rewritten. */
static int load_block(struct xfs *fs, uint64_t fsb) {
    struct bbuf *b;
    int rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, fsb), &b);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_copy_bytes(fs->dirblock, b->data, fs->blocksize);
    bcache_put(fs->cache, b);
    return xfs_be32(fs->dirblock + XFS_DIR3_HDR_MAGIC) == xfs_magic_for(fs, XFS_BLK_DIR_BLOCK) ? KERR_OK
                                                                               : KERR_NOTSUP;
}

/* The name at `at`, against the one wanted. */
static bool same_name(const uint8_t *block, uint32_t at, const char *name, uint32_t namelen) {
    if (block[at + 8u] != namelen) {
        return false;
    }
    for (uint32_t i = 0; i < namelen; i++) {
        if (block[at + 9u + i] != (uint8_t)name[i]) {
            return false;
        }
    }
    return true;
}

/* Where the sorted hash array begins, and how many pairs are in it. */
static uint32_t leaf_start(const struct xfs *fs, uint32_t *count) {
    *count = xfs_be32(fs->dirblock + fs->dirblocksize - XFS_DIR2_BLOCK_TAIL_SIZE);
    return fs->dirblocksize - XFS_DIR2_BLOCK_TAIL_SIZE - *count * XFS_DIR2_LEAF_ENTRY_SIZE;
}

/* A free span of at least `need` bytes, or `0`.
 *
 * The data area ends where the hash array begins, and **the array grows
 * downwards as entries are added** — so a span is only usable if it still
 * fits once the array has taken its eight more bytes. Forgetting that is
 * how an entry gets written over the first hash pair. */
uint32_t xfs_dir_find_room(const struct xfs *fs, const uint8_t *block, uint32_t end,
                           uint32_t need) {
    uint32_t at = fs->dir_data_hdr;
    while (at + 8u <= end) {
        uint32_t len;
        if (xfs_be16(block + at) != XFS_DIR2_DATA_FREE_TAG) {
            at += xfs_dir_entry_size(fs, block[at + 8u]);
            continue;
        }
        len = xfs_be16(block + at + 2u);
        if (len < XFS_DIR2_DATA_ALIGN) {
            return 0; /* a free span that cannot be true */
        }
        /* **The part of the span that is still free, not the part it
         * claims.** The trailing span records the length it had when it was
         * written, and the hash array has been growing down into it ever
         * since — so requiring the *whole* span to lie below the array
         * rejects the only span there is. The first version did exactly
         * that: after one addition the directory reported itself full with
         * three and a half kilobytes free. */
        if (at < end && (len <= end - at ? len : end - at) >= need) {
            return at;
        }
        at += len;
    }
    return 0;
}

/* Slides the hash array down by one pair and inserts into it, which is what
 * adding an entry does to the index. */
static void index_insert(struct xfs *fs, uint32_t hash, uint32_t address) {
    uint32_t count, from = leaf_start(fs, &count);
    uint32_t to = from - XFS_DIR2_LEAF_ENTRY_SIZE;
    uint32_t at = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t h = xfs_be32(fs->dirblock + from + i * XFS_DIR2_LEAF_ENTRY_SIZE);
        xfs_copy_bytes(fs->dirblock + to + i * XFS_DIR2_LEAF_ENTRY_SIZE,
                       fs->dirblock + from + i * XFS_DIR2_LEAF_ENTRY_SIZE,
                       XFS_DIR2_LEAF_ENTRY_SIZE);
        if (h <= hash) {
            at = i + 1u;
        }
    }
    for (uint32_t i = count; i > at; i--) {
        xfs_copy_bytes(fs->dirblock + to + i * XFS_DIR2_LEAF_ENTRY_SIZE,
                       fs->dirblock + to + (i - 1u) * XFS_DIR2_LEAF_ENTRY_SIZE,
                       XFS_DIR2_LEAF_ENTRY_SIZE);
    }
    xfs_put_be32(fs->dirblock + to + at * XFS_DIR2_LEAF_ENTRY_SIZE, hash);
    xfs_put_be32(fs->dirblock + to + at * XFS_DIR2_LEAF_ENTRY_SIZE + 4u, address);
    xfs_put_be32(fs->dirblock + fs->dirblocksize - XFS_DIR2_BLOCK_TAIL_SIZE, count + 1u);
}

/* Takes one pair out of the hash array and slides the rest up. */
static void index_remove(struct xfs *fs, uint32_t address) {
    uint32_t count, from = leaf_start(fs, &count);
    uint32_t to = from + XFS_DIR2_LEAF_ENTRY_SIZE;
    uint32_t drop = count;
    for (uint32_t i = 0; i < count; i++) {
        if (xfs_be32(fs->dirblock + from + i * XFS_DIR2_LEAF_ENTRY_SIZE + 4u) == address) {
            drop = i;
            break;
        }
    }
    if (drop == count) {
        return; /* nothing claimed that address: leave the array alone */
    }
    for (uint32_t i = drop; i > 0; i--) {
        xfs_copy_bytes(fs->dirblock + to + (i - 1u) * XFS_DIR2_LEAF_ENTRY_SIZE,
                       fs->dirblock + from + (i - 1u) * XFS_DIR2_LEAF_ENTRY_SIZE,
                       XFS_DIR2_LEAF_ENTRY_SIZE);
    }
    xfs_put_be32(fs->dirblock + fs->dirblocksize - XFS_DIR2_BLOCK_TAIL_SIZE, count - 1u);
}

/* Writes the buffer back and reseals it. */
static int flush_block(struct xfs *fs, const struct xfs_inode *dir, uint64_t fsb, uint64_t now_s) {
    struct bbuf *b;
    int rc;
    xfs_stamp_owner(fs, fs->dirblock, 0, 0, XFS_DIR3_HDR_OWNER, 0, dir->ino);
    xfs_reseal(fs, fs->dirblock, fs->dirblocksize, XFS_DIR3_HDR_CRC);
    rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, fsb), &b);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_copy_bytes(b->data, fs->dirblock, fs->blocksize);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return xfs_dir_touch(fs, dir, now_s);
}

int xfs_dir_block_add(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                      uint32_t namelen, uint64_t ino, uint8_t ftype, uint64_t now_s) {
    uint32_t need = xfs_dir_entry_size(fs, namelen);
    uint64_t fsb = 0;
    uint32_t at, len;
    int rc = the_block(fs, dir, &fsb);
    rc = rc != KERR_OK ? rc : load_block(fs, fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    {
        uint32_t count;
        at = xfs_dir_find_room(fs, fs->dirblock,
                               leaf_start(fs, &count) - XFS_DIR2_LEAF_ENTRY_SIZE, need);
    }
    if (at == 0) {
        return KERR_NOSPC; /* it would have to become a leaf directory */
    }
    /* **The span is clamped to where the index will be, not to where it
     * is.** Adding an entry adds a pair to the hash array, and the array
     * grows *downwards* — so the last eight bytes of a free span that
     * reaches it are about to stop being free. The first version left the
     * span's recorded length alone, so after about twenty additions the
     * span claimed bytes the array had taken and an entry was written over
     * the hashes: the directory kept working until a name landed there and
     * then stopped being a directory at all. */
    {
        uint32_t count, usable = leaf_start(fs, &count) - XFS_DIR2_LEAF_ENTRY_SIZE;
        len = xfs_be16(fs->dirblock + at + 2u);
        if (at + len > usable) {
            len = usable - at;
        }
    }
    (void)xfs_dir_put_entry(fs, fs->dirblock, at, ino, name, namelen, ftype);
    /* What is left of the span stays free; a remainder too small to hold a
     * free record is left inside the entry's padding instead. */
    if (len - need >= XFS_DIR2_DATA_ALIGN) {
        xfs_dir_put_free(fs->dirblock, at + need, len - need);
    }
    index_insert(fs, xfs_dir_hashname(name, namelen), at / XFS_DIR2_DATA_ALIGN);
    return flush_block(fs, dir, fsb, now_s);
}

int xfs_dir_block_remove(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                         uint32_t namelen, uint64_t now_s, uint64_t *ino) {
    uint64_t fsb = 0;
    uint32_t count, end, at;
    int rc = the_block(fs, dir, &fsb);
    rc = rc != KERR_OK ? rc : load_block(fs, fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    end = leaf_start(fs, &count);
    at = fs->dir_data_hdr;
    while (at + 8u <= end) {
        uint32_t size;
        if (xfs_be16(fs->dirblock + at) == XFS_DIR2_DATA_FREE_TAG) {
            at += xfs_be16(fs->dirblock + at + 2u);
            continue;
        }
        size = xfs_dir_entry_size(fs, fs->dirblock[at + 8u]);
        if (same_name(fs->dirblock, at, name, namelen)) {
            *ino = xfs_be64(fs->dirblock + at);
            index_remove(fs, at / XFS_DIR2_DATA_ALIGN);
            xfs_dir_put_free(fs->dirblock, at, size);
            return flush_block(fs, dir, fsb, now_s);
        }
        at += size;
    }
    return KERR_NOENT;
}
