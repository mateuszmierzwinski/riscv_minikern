#include "core/bcache.h"
#include "core/errno.h"
#include "core/xfs/xfs_internal.h"
#include "lib/crc32c.h"

#include <stddef.h>

/* ── creating a file ──────────────────────────────────────────────────────
 *
 * Four things change on disk, and all of them are refused rather than
 * guessed at when they do not look the way this understands:
 *
 *   1. an inode becomes used, in **both** inode btrees — every chunk
 *      (`IAB3`) and the chunks that still have a free inode (`FIB3`);
 *   2. the group's inode header counts one fewer free;
 *   3. the superblock's counter follows it;
 *   4. the new inode is written, and an entry for it appears in the
 *      parent's directory.
 *
 * What it will not do, each for the same reason — that the alternative is a
 * btree operation this does not implement, and a half-done btree is worse
 * than a refusal:
 *
 *   - take the **last** free inode of a chunk, which would have to delete
 *     that chunk's record from the free-inode tree;
 *   - allocate a new chunk, which needs the block allocator;
 *   - touch a tree with more than one level, or a directory that has grown
 *     past the form that lives inside its inode.
 *
 * So this creates files in a filesystem that already has room for them,
 * which is what the tests do, and says so clearly when it cannot.
 */

#define XFS_DIR_SF_COUNT_OFF   0u
#define XFS_DIR_SF_I8COUNT_OFF 1u
#define XFS_DIR_SF_PARENT_OFF  2u
#define XFS_DIR_SF_HEADER      6u /* count, i8count and a 4-byte parent */

#define XFS_DI_V3_GEN_START 1u

void xfs_put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

void xfs_put_be32(uint8_t *p, uint32_t v) {
    xfs_put_be16(p, (uint16_t)(v >> 16));
    xfs_put_be16(p + 2, (uint16_t)v);
}

void xfs_put_be64(uint8_t *p, uint64_t v) {
    xfs_put_be32(p, (uint32_t)(v >> 32));
    xfs_put_be32(p + 4, (uint32_t)v);
}

static void put_le32(uint8_t *p, uint32_t v) {
    for (uint32_t i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static void zero(uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        p[i] = 0;
    }
}

static void copy(uint8_t *dst, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

/* Recomputes a self-describing structure's checksum over len bytes. */
void xfs_reseal(uint8_t *at, uint32_t len, uint32_t crc_off) {
    put_le32(at + crc_off, 0);
    put_le32(at + crc_off, crc32c(at, len));
}

/* The group's first block, where its four header sectors live. */
uint64_t xfs_group_block(const struct xfs *fs, uint32_t agno) {
    return (uint64_t)agno * fs->agblocks;
}

/* The group's inode header, checked the way everything else here is
 * checked: its magic, its checksum and the filesystem's own UUID. */
int xfs_agi_get(struct xfs *fs, uint32_t agno, struct bbuf **out, uint8_t **agi) {
    uint8_t *at;
    int rc = bcache_get(fs->cache, xfs_group_block(fs, agno), out);
    if (rc != KERR_OK) {
        return rc;
    }
    at = (*out)->data + XFS_AGI_SECTOR * fs->sectsize;
    if (xfs_be32(at) != XFS_AGI_MAGIC || !xfs_crc_ok(at, fs->sectsize, XFS_AGI_CRC) ||
        !xfs_uuid_ok(fs, at + XFS_AGI_UUID)) {
        bcache_put(fs->cache, *out);
        return KERR_IO;
    }
    *agi = at;
    return KERR_OK;
}

/* One of the inode btrees' root blocks, which must be a lone leaf. */
int xfs_tree_get(struct xfs *fs, uint32_t agno, uint32_t block, uint32_t magic, struct bbuf **out) {
    uint64_t fsb = ((uint64_t)agno << fs->agblklog) | block;
    int rc = xfs_meta_block(fs, fsb, magic, 0, XFS_BTREE_CRC, XFS_BTREE_UUID, out);
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfs_be16((*out)->data + XFS_BTREE_LEVEL) != 0) {
        bcache_put(fs->cache, *out);
        return KERR_NOTSUP; /* a tree with branches: not ours to walk */
    }
    return KERR_OK;
}

/* The record whose chunk holds this group-relative inode, or NULL.
 *
 * By range rather than by a mask: a chunk begins wherever the allocator put
 * it — mkfs.xfs's first one starts at inode 96, which is not a multiple of
 * 64 — so rounding an inode number down to a multiple of the chunk size
 * names no chunk at all. Masking it did, and a remove failed with EIO
 * because nothing matched. */
uint8_t *xfs_tree_record_for(const struct bbuf *b, uint32_t agino) {
    uint32_t n = xfs_be16(b->data + XFS_BTREE_NUMRECS);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *rec = b->data + XFS_BTREE_RECORDS + i * XFS_INOBT_REC_SIZE;
        uint32_t start = xfs_be32(rec + XFS_INOBT_REC_STARTINO);
        if (agino >= start && agino - start < XFS_INODES_PER_CHUNK) {
            return rec;
        }
    }
    return NULL;
}

/* The first record with at least two free inodes, and the lowest free one
 * in it. Two, not one: taking the last would have to delete the record
 * from the free-inode tree, which is a btree operation this does not do. */
static uint8_t *pick_record(const struct bbuf *b, uint32_t *bit) {
    uint32_t n = xfs_be16(b->data + XFS_BTREE_NUMRECS);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *rec = b->data + XFS_BTREE_RECORDS + i * XFS_INOBT_REC_SIZE;
        uint64_t free = xfs_be64(rec + XFS_INOBT_REC_FREE);
        if (xfs_be32(rec + XFS_INOBT_REC_FREECOUNT) < 2u) {
            continue;
        }
        for (uint32_t b2 = 0; b2 < XFS_INODES_PER_CHUNK; b2++) {
            if ((free & ((uint64_t)1 << b2)) != 0) {
                *bit = b2;
                return rec;
            }
        }
    }
    return NULL;
}

/* Marks one inode of a record used: its bit cleared, its count down. */
static void take_from_record(uint8_t *rec, uint32_t bit) {
    uint64_t free = xfs_be64(rec + XFS_INOBT_REC_FREE) & ~((uint64_t)1 << bit);
    xfs_put_be32(rec + XFS_INOBT_REC_FREECOUNT, xfs_be32(rec + XFS_INOBT_REC_FREECOUNT) - 1u);
    xfs_put_be64(rec + XFS_INOBT_REC_FREE, free);
}

/* Takes a free inode from this group, marking it used in both trees and in
 * the group header. *ino is the inode number it allocated.
 *
 * The free-inode tree is updated for the same chunk, and its absence is an
 * error rather than something to work around: a filesystem that says it has
 * that tree and then does not describe a chunk in it is one this does not
 * understand, and writing to it anyway is how a reader becomes a
 * corrupter. */
static int take_inode(struct xfs *fs, uint32_t agno, uint64_t *ino) {
    struct bbuf *agib, *inob, *finob;
    uint8_t *agi, *rec, *frec;
    uint32_t bit = 0, startino;
    int rc = xfs_agi_get(fs, agno, &agib, &agi);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_tree_get(fs, agno, xfs_be32(agi + XFS_AGI_ROOT), XFS_INOBT_MAGIC, &inob);
    if (rc != KERR_OK) {
        bcache_put(fs->cache, agib);
        return rc;
    }
    rec = pick_record(inob, &bit);
    startino = rec != NULL ? xfs_be32(rec + XFS_INOBT_REC_STARTINO) : 0;
    rc = rec == NULL ? KERR_NOSPC : xfs_tree_get(fs, agno, xfs_be32(agi + XFS_AGI_FREE_ROOT), XFS_FINOBT_MAGIC,
                                             &finob);
    if (rc != KERR_OK) {
        bcache_put(fs->cache, inob);
        bcache_put(fs->cache, agib);
        return rc;
    }
    frec = xfs_tree_record_for(finob, startino + bit);
    if (frec == NULL) {
        bcache_put(fs->cache, finob);
        bcache_put(fs->cache, inob);
        bcache_put(fs->cache, agib);
        return KERR_IO;
    }
    take_from_record(rec, bit);
    take_from_record(frec, bit);
    xfs_put_be32(agi + XFS_AGI_FREECOUNT, xfs_be32(agi + XFS_AGI_FREECOUNT) - 1u);
    xfs_reseal(inob->data, fs->blocksize, XFS_BTREE_CRC);
    xfs_reseal(finob->data, fs->blocksize, XFS_BTREE_CRC);
    xfs_reseal(agi, fs->sectsize, XFS_AGI_CRC);
    bcache_dirty(inob);
    bcache_dirty(finob);
    bcache_dirty(agib);
    bcache_put(fs->cache, finob);
    bcache_put(fs->cache, inob);
    bcache_put(fs->cache, agib);
    /* **`+ bit`, not `| bit`.** The chunk's first inode number is not
     * aligned to the chunk's size — the first one in a group commonly
     * starts at 96 — so an offset shares bits with it and an `or` silently
     * returns an inode that is already in use. With a start of 96, offset
     * 32 produced 96 again: the **root directory**, whose inode was then
     * overwritten with the new file's, after which the filesystem had no
     * root. Twenty-eight creates worked first, because the offsets below 32
     * happen to have no bits in common with 96.
     *
     * It had never run: `xfs_create` had no caller until the write path was
     * wired up, so this was waiting in code that compiled. */
    *ino = ((uint64_t)agno << (fs->agblklog + fs->inopblog)) + (startino & 0xffffffffu) + bit;
    return KERR_OK;
}

/* The superblock's free-inode counter, which must follow the groups'. */
int xfs_count_inodes_free(struct xfs *fs, int32_t delta) {
    struct bbuf *b;
    int rc = bcache_get(fs->cache, 0, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_put_be64(b->data + XFS_SB_IFREE, xfs_be64(b->data + XFS_SB_IFREE) + (uint64_t)(int64_t)delta);
    xfs_reseal(b->data, fs->sectsize, XFS_SB_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* Writes a new inode over the slot the allocator just took.
 *
 * Everything a v5 inode carries is written, including the fields that only
 * exist to catch a stale one: the inode's own number, the filesystem's
 * UUID, and the checksum over the whole thing. An empty file is the
 * extents form with no extents — which is also what makes it writable
 * later by the path in xfs_file.c. */
static int write_new_inode(struct xfs *fs, uint64_t ino, uint16_t mode, uint64_t now_s) {
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    uint8_t *di;
    int rc;
    xfs_inode_place(fs, ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    zero(di, fs->inodesize);
    xfs_put_be16(di + XFS_DI_MAGIC, XFS_DINODE_MAGIC);
    xfs_put_be16(di + XFS_DI_MODE, mode);
    di[XFS_DI_VERSION] = XFS_DI_VERSION_3;
    di[XFS_DI_FORMAT] = XFS_DINODE_FMT_EXTENTS;
    di[XFS_DI_AFORMAT] = XFS_DINODE_FMT_EXTENTS;
    xfs_put_be32(di + XFS_DI_NLINK, 1);
    xfs_put_be32(di + XFS_DI_GEN, XFS_DI_V3_GEN_START);
    /* Not zero: this field is the unlinked-inode list's link, and zero
     * means "inode 0 follows" rather than "nothing does". xfs_repair said
     * so — "bad next_unlinked 0x0 on inode 100" — and would have cleared
     * the inode for it. */
    xfs_put_be32(di + XFS_DI_NEXT_UNLINKED, XFS_NULL_AGINO);
    xfs_put_be64(di + XFS_DI_INO, ino);
    copy(di + XFS_DI_UUID, fs->uuid, 16);
    for (uint32_t t = 0; t < 3; t++) {
        xfs_put_be32(di + XFS_DI_ATIME + t * 8u, (uint32_t)now_s);
    }
    xfs_put_be32(di + XFS_DI_CRTIME, (uint32_t)now_s);
    xfs_reseal(di, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* ── the parent's directory ───────────────────────────────────────────── */

/* How many bytes an entry takes inside the inode. */
static uint32_t sf_entry_bytes(uint32_t namelen) {
    return 1u + 2u + namelen + 1u + 4u; /* namelen, offset, name, type, a 4-byte inumber */
}

/* Where in a data block the next entry would sit: the first is past the
 * header and the two entries every directory has, and each one after
 * advances by its own size there, rounded up to eight (XFS.md §10). */
static uint32_t sf_next_offset(const uint8_t *sf, uint32_t count) {
    const uint8_t *e = sf + XFS_DIR_SF_HEADER;
    uint32_t off = XFS_DIR_SF_FIRST_OFFSET;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t namelen = e[0];
        off = xfs_be16(e + 1);
        off += (XFS_DIR_SF_ENTRY_FIXED + namelen + XFS_DIR2_DATA_ALIGN - 1u) & ~(XFS_DIR2_DATA_ALIGN - 1u);
        e += sf_entry_bytes(namelen);
    }
    return off;
}

/* The end of the entries, which is also the directory's size. */
static uint32_t sf_used(const uint8_t *sf, uint32_t count) {
    const uint8_t *e = sf + XFS_DIR_SF_HEADER;
    for (uint32_t i = 0; i < count; i++) {
        e += sf_entry_bytes(e[0]);
    }
    return (uint32_t)(e - sf);
}

/* True when the directory already holds this name. */
static bool sf_has_name(const uint8_t *sf, uint32_t count, const char *name, uint32_t namelen) {
    const uint8_t *e = sf + XFS_DIR_SF_HEADER;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t len = e[0];
        bool same = len == namelen;
        for (uint32_t c = 0; same && c < len; c++) {
            same = e[3 + c] == (uint8_t)name[c];
        }
        if (same) {
            return true;
        }
        e += sf_entry_bytes(len);
    }
    return false;
}

/* Adds one entry to a shortform directory, in the parent's own inode.
 *
 * Runs to 28 lines: it is one derivation — where the entry goes, what goes
 * in it, and the three fields of the inode that must then agree with it —
 * and splitting it would hand half-written bytes inside a live inode to a
 * second function (Rule 11, Single derivation). */
int xfs_dir_sf_add(struct xfs *fs, const struct xfs_inode *dir, const char *name, uint32_t namelen,
                   uint64_t ino, uint8_t ftype, uint64_t now_s) {
    uint64_t block;
    uint32_t boffset, count, used, room;
    struct bbuf *b;
    uint8_t *di, *sf, *e;
    int rc;
    xfs_inode_place(fs, dir->ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    sf = di + XFS_DI_V3_SIZE;
    count = sf[XFS_DIR_SF_COUNT_OFF];
    used = sf_used(sf, count);
    room = di[XFS_DI_FORKOFF] != 0 ? di[XFS_DI_FORKOFF] * 8u : fs->inodesize - XFS_DI_V3_SIZE;
    if (sf[XFS_DIR_SF_I8COUNT_OFF] != 0 || ino > 0xffffffffull) {
        rc = KERR_NOTSUP; /* eight-byte inumbers in a directory: a different shape */
    } else if (sf_has_name(sf, count, name, namelen)) {
        rc = KERR_EXIST;
    } else if (used + sf_entry_bytes(namelen) > room) {
        rc = KERR_NOSPC; /* it would have to become a block directory */
    }
    if (rc != KERR_OK) {
        bcache_put(fs->cache, b);
        return rc;
    }
    e = sf + used;
    e[0] = (uint8_t)namelen;
    xfs_put_be16(e + 1, (uint16_t)sf_next_offset(sf, count));
    copy(e + 3, (const uint8_t *)name, namelen);
    e[3 + namelen] = ftype;
    xfs_put_be32(e + 4 + namelen, (uint32_t)ino);
    sf[XFS_DIR_SF_COUNT_OFF] = (uint8_t)(count + 1u);
    xfs_put_be64(di + XFS_DI_SIZE, used + sf_entry_bytes(namelen));
    xfs_put_be32(di + XFS_DI_MTIME, (uint32_t)now_s);
    xfs_put_be32(di + XFS_DI_CTIME, (uint32_t)now_s);
    xfs_reseal(di, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* Names that may not be created, whatever the filesystem would make of
 * them: a traversal, an empty name, or one too long for the form. */
static bool name_is_allowed(const char *name, uint32_t namelen) {
    if (namelen == 0 || namelen > 255u) {
        return false;
    }
    for (uint32_t i = 0; i < namelen; i++) {
        if (name[i] == '/' || name[i] == '\0') {
            return false;
        }
    }
    return !(namelen <= 2u && name[0] == '.' && (namelen == 1u || name[1] == '.'));
}

/* Writes the new inode and its parent to the log as one transaction.
 *
 * It is one transaction because it is one change: a file that existed in
 * its parent's directory but had no inode, or the other way round, is not
 * half a create — it is a corrupt filesystem. Both cores are read back from
 * the cache rather than kept from earlier, so what is logged is what was
 * actually written. */
static int log_create(struct xfs *fs, uint64_t ino, uint64_t dirino) {
    struct xfs_log_inode_item items[2];
    struct bbuf *b[2];
    uint64_t block[2];
    uint32_t boffset[2];
    uint64_t inos[2] = {ino, dirino};
    int rc = KERR_OK;
    for (uint32_t i = 0; i < 2u && rc == KERR_OK; i++) {
        xfs_inode_place(fs, inos[i], &block[i], &boffset[i]);
        rc = bcache_get(fs->cache, block[i], &b[i]);
        if (rc != KERR_OK) {
            while (i-- > 0) {
                bcache_put(fs->cache, b[i]);
            }
            return rc;
        }
        items[i].ino = inos[i];
        items[i].chunk_block = block[i];
        items[i].boffset = boffset[i];
        items[i].core = b[i]->data + boffset[i];
    }
    rc = xfs_log_inodes(fs, items, 2);
    bcache_put(fs->cache, b[1]);
    bcache_put(fs->cache, b[0]);
    return rc;
}

int xfs_create(struct xfs *fs, const struct xfs_inode *dir, const char *name, uint32_t namelen, uint16_t mode,
               uint64_t now_s, uint64_t *ino_out) {
    uint64_t ino = 0;
    int rc;
    if (!xfs_writable(fs)) {
        return KERR_ROFS;
    }
    if (!xfs_is_dir(dir)) {
        return KERR_NOTDIR;
    }
    if (dir->format != XFS_DINODE_FMT_LOCAL && dir->format != XFS_DINODE_FMT_EXTENTS) {
        return KERR_NOTSUP; /* a fork shape the write path cannot place into */
    }
    if (!name_is_allowed(name, namelen)) {
        return KERR_INVAL;
    }
    rc = take_inode(fs, xfs_fsb_ag(fs, dir->ino >> fs->inopblog), &ino);
    if (rc == KERR_OK) {
        rc = xfs_count_inodes_free(fs, -1);
    }
    if (rc == KERR_OK) {
        rc = write_new_inode(fs, ino, mode, now_s);
    }
    if (rc == KERR_OK) {
        rc = xfs_dir_add(fs, dir, name, namelen, ino, XFS_DIR3_FT_REG, now_s);
    }
    if (rc == KERR_OK) {
        rc = log_create(fs, ino, dir->ino);
    }
    *ino_out = ino;
    return rc;
}

/* Adds an entry, growing the directory if it has to.
 *
 * **The one place that knows a directory can change shape.** Every caller —
 * create, link, rename — wants the same thing, "this name now points at
 * this inode", and none of them should have to know whether the directory
 * still fits in its inode. `KERR_NOSPC` from the shortform path is not a
 * failure but the signal that it is time to grow, which is why that path
 * returns it rather than attempting the conversion itself: the conversion
 * needs the new entry, and building the block from the old entries *and*
 * the new one together is what keeps the directory from briefly holding
 * neither answer. */
int xfs_dir_add(struct xfs *fs, const struct xfs_inode *dir, const char *name, uint32_t namelen,
                uint64_t ino, uint8_t ftype, uint64_t now_s) {
    int rc;
    if (dir->format == XFS_DINODE_FMT_LOCAL) {
        rc = xfs_dir_sf_add(fs, dir, name, namelen, ino, ftype, now_s);
        if (rc != KERR_NOSPC) {
            return rc;
        }
        return xfs_dir_to_block(fs, dir, name, namelen, ino, ftype, now_s);
    }
    return xfs_dir_block_add(fs, dir, name, namelen, ino, ftype, now_s);
}
