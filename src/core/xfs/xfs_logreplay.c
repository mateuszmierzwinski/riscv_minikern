/* Replaying the log: putting a filesystem back the way its log says it is.
 *
 * **What this is for.** A filesystem whose log holds uncommitted work has
 * metadata that is *behind* the log. Checking it and mounting it read-only
 * (`xfs_check`) makes it readable, which is most of what matters after a
 * power cut — but it stays read-only for ever, because writing over a log
 * that says something newer happened would build on a superseded state.
 * Replay is what resolves that: apply what the log says, write an unmount
 * record, and the filesystem is clean and writable again.
 *
 * **Why it is the most dangerous code here, and what that buys.** Every
 * other operation in this filesystem acts on what it is told; this one acts
 * on what it *infers* from a structure a crash was in the middle of
 * writing. Getting it wrong does not fail to mount — it writes plausible
 * rubbish into metadata and the damage is found later, somewhere else. So
 * the rule throughout is **all or nothing**:
 *
 *   - a transaction is applied only when its `COMMIT_TRANS` has been seen.
 *     A crash leaves a partial transaction at the end of the log, and that
 *     one is exactly the one that must not be applied;
 *   - an item type this does not understand **aborts the whole replay**
 *     rather than being skipped, and so does an intent other than an
 *     extent free that no done item answers;
 *   - every change goes to the block cache, and the cache is written only
 *     when the whole log has been applied; an abort drops it.
 *
 * **How a log is read.** An item is several *regions*, each the payload of
 * one log operation — or of several, when an operation was split across
 * records (`CONTINUE`/`WAS_CONT`) — and its first region, the format,
 * says how many there are. Two passes: the first learns which transactions
 * committed, which buffers were later freed (`CANCEL`), and which intents
 * were finished; the second applies the committed items in log order. Then
 * the extents of an extent-free intent that was never finished are freed.
 *
 * **What it applies**, judged against a log Linux wrote
 * (test/xfs_linuxlog_test.sh): inode items with their forks (inline data,
 * extent lists, b-tree roots, a device number); buffer items, an inode
 * buffer's unlinked pointers only; inode-create items; dquot items; and
 * extent-free intents.
 */
#include "core/bcache.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs_format.h"
#include "core/xfs/xfs_internal.h"

/* One record's worth of the log, which is what an operation may span.
 *
 * Static for the reason `xfs_log.c` gives about its own buffer: a kernel
 * stack is 1 KiB and this is 32 KiB. Mounting is not reentrant and the
 * kernel is not preemptible, so one buffer is enough. */
#define REPLAY_MAX_BYTES (64u * XLOG_SECTOR)
static uint8_t record[REPLAY_MAX_BYTES];

/* The item being assembled from its regions: a buffer item can carry a
 * whole 64 KiB directory block, and up to one region per run of logged
 * chunks. */
#define ITEM_MAX_BYTES (XFS_DIRBLK_MAX + 8192u)
#define ITEM_REGIONS_MAX 520u
static uint8_t itembuf[ITEM_MAX_BYTES];

struct item {
    uint32_t tid;
    uint32_t nregions;
    uint32_t want; /* regions the format promises; 0 until it is read */
    uint32_t used;
    bool open_region; /* the last region continues in the next operation */
    uint32_t off[ITEM_REGIONS_MAX];
    uint32_t len[ITEM_REGIONS_MAX];
};

/* What pass 1 records, by transaction, for pass 2 to consult once it knows
 * which transactions committed. */
#define COMMITTED_MAX 512u
#define CANCELS_MAX   1024u
#define INTENTS_MAX   256u
#define EFI_EXTENTS_MAX 16u

struct cancel {
    uint32_t tid, seq;
    uint64_t blkno;
};

struct intent {
    uint32_t tid, seq;
    uint16_t type;
    uint64_t id;
    uint32_t nextents;
    uint64_t start[EFI_EXTENTS_MAX];
    uint32_t len[EFI_EXTENTS_MAX];
};

struct done {
    uint32_t tid;
    uint64_t id;
};

struct pass {
    bool applying;
    bool in_txn;
    uint32_t txn_tid;
    uint32_t seq; /* items seen, in log order: the same count in both passes */
    uint32_t applied;
    uint32_t items_applied;
    struct item it;
    uint32_t committed[COMMITTED_MAX];
    uint32_t ncommitted;
    struct cancel cancels[CANCELS_MAX];
    uint32_t ncancels;
    struct intent intents[INTENTS_MAX];
    uint32_t nintents;
    struct done dones[INTENTS_MAX];
    uint32_t ndones;
};

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4u) << 32);
}

/* ── reading records ─────────────────────────────────────────────────── */

static uint64_t log_blocks(const struct xfs *fs) {
    return (uint64_t)fs->logblocks * (fs->blocksize / XLOG_SECTOR);
}

/* One basic block of the log into the record buffer at `at`, wherever the
 * ring puts it: a record's data may continue at the log's start. */
static int read_ring(struct xfs *fs, uint64_t bb, uint8_t *at) {
    return bcache_pread(fs->log_cache, fs->log_base + (bb % log_blocks(fs)) * XLOG_SECTOR, at,
                        XLOG_SECTOR);
}

/* Reads a whole record — header and data — and puts the stamped-over bytes
 * back.
 *
 * The writer replaces the first four bytes of every data sector with the
 * record's cycle so that a torn write is detectable, keeping the originals
 * in `h_cycle_data`. Undoing that is not optional: without it, the first
 * operation header in the second sector has a length and a client id taken
 * from the middle of a cycle number. */
static int read_record(struct xfs *fs, uint64_t bb, uint32_t *bytes, uint32_t *nops) {
    uint32_t len, sectors;
    int rc = fs->log_cache == NULL ? KERR_NODEV : read_ring(fs, bb, record);
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfs_be32(record + XLOG_REC_MAGIC) != XLOG_MAGIC) {
        return KERR_NOENT; /* not a record where one has to be */
    }
    len = xfs_be32(record + XLOG_REC_LEN);
    sectors = (len + XLOG_SECTOR - 1u) / XLOG_SECTOR;
    if (len == 0 || sectors * XLOG_SECTOR > REPLAY_MAX_BYTES - XLOG_REC_HDR_SIZE ||
        sectors > XLOG_DATA_MAX_BB || xfs_log_record_blocks(record) != 1u + sectors) {
        return KERR_NOTSUP; /* larger than this can hold, or more than one header block */
    }
    for (uint32_t i = 0; rc == KERR_OK && i < sectors; i++) {
        rc = read_ring(fs, bb + 1u + i, record + XLOG_REC_HDR_SIZE + i * XLOG_SECTOR);
    }
    for (uint32_t i = 0; rc == KERR_OK && i < sectors; i++) {
        uint8_t *sector = record + XLOG_REC_HDR_SIZE + i * XLOG_SECTOR;
        xfs_copy_bytes(sector, record + XLOG_REC_CYCLE_DATA + i * 4u, 4u);
    }
    *bytes = len;
    *nops = xfs_be32(record + XLOG_REC_NUM_OPS);
    return rc;
}

/* ── what pass 1 learns ──────────────────────────────────────────────── */

static bool committed(const struct pass *p, uint32_t tid) {
    for (uint32_t i = 0; i < p->ncommitted; i++) {
        if (p->committed[i] == tid) {
            return true;
        }
    }
    return false;
}

/* Whether a committed `CANCEL` for the buffer at `blkno` comes after the
 * item at `seq`: the block was freed later, so what this item says of it
 * is void — and replaying it could overwrite the block's next owner. */
static bool cancelled_later(const struct pass *p, uint64_t blkno, uint32_t seq) {
    for (uint32_t i = 0; i < p->ncancels; i++) {
        const struct cancel *c = &p->cancels[i];
        if (c->blkno == blkno && c->seq > seq && committed(p, c->tid)) {
            return true;
        }
    }
    return false;
}

static bool is_intent(uint16_t type) {
    return type == XFS_LI_EFI || type == XFS_LI_RUI || type == XFS_LI_CUI ||
           type == XFS_LI_BUI || type == XFS_LI_ATTRI || type == XFS_LI_XMI;
}

static bool is_done(uint16_t type) {
    return type == XFS_LI_EFD || type == XFS_LI_RUD || type == XFS_LI_CUD ||
           type == XFS_LI_BUD || type == XFS_LI_ATTRD || type == XFS_LI_XMD;
}

/* An extent-free intent's extents, as many as the item holds. Linux logs
 * either 16-byte records or packed 12-byte ones; the item's length says
 * which. */
static int note_efi(struct intent *in, const uint8_t *fmt, uint32_t len) {
    uint32_t n = le32(fmt + EFI_NEXTENTS);
    uint32_t each = n == 0 ? 0 : (len - EFI_EXTENTS) / n;
    if (n == 0 || n > EFI_EXTENTS_MAX || (each != 16u && each != 12u) ||
        EFI_EXTENTS + n * each > len) {
        return KERR_NOTSUP;
    }
    in->nextents = n;
    for (uint32_t i = 0; i < n; i++) {
        in->start[i] = le64(fmt + EFI_EXTENTS + i * each);
        in->len[i] = le32(fmt + EFI_EXTENTS + i * each + 8u);
    }
    return KERR_OK;
}

/* Pass 1's record of one item: a cancelled buffer, an intent, a done. */
static int note_item(struct pass *p, uint32_t tid, uint32_t seq, uint16_t type) {
    const uint8_t *fmt = itembuf + p->it.off[0];
    if (type == XFS_LI_BUF && (le16(fmt + BLF_FLAGS) & XFS_BLF_CANCEL) != 0) {
        if (p->ncancels == CANCELS_MAX) {
            return KERR_NOTSUP;
        }
        p->cancels[p->ncancels++] = (struct cancel){tid, seq, le64(fmt + BLF_BLKNO)};
    } else if (is_intent(type) && p->it.len[0] >= INTENT_ID + 8u) {
        struct intent *in = p->nintents < INTENTS_MAX ? &p->intents[p->nintents] : NULL;
        if (in == NULL) {
            return KERR_NOTSUP;
        }
        *in = (struct intent){.tid = tid, .seq = seq, .type = type, .id = le64(fmt + INTENT_ID)};
        p->nintents++;
        return type == XFS_LI_EFI ? note_efi(in, fmt, p->it.len[0]) : KERR_OK;
    } else if (is_done(type) && p->it.len[0] >= INTENT_ID + 8u) {
        if (p->ndones == INTENTS_MAX) {
            return KERR_NOTSUP;
        }
        p->dones[p->ndones++] = (struct done){tid, le64(fmt + INTENT_ID)};
    }
    return KERR_OK;
}

/* Whether a committed done item answers the intent `id`. */
static bool finished(const struct pass *p, uint64_t id) {
    for (uint32_t i = 0; i < p->ndones; i++) {
        if (p->dones[i].id == id && committed(p, p->dones[i].tid)) {
            return true;
        }
    }
    return false;
}

/* Every committed intent that no done item answers must be one this can
 * finish — an extent free. Anything else is refused before a byte is
 * applied. */
static int intents_finishable(const struct pass *p) {
    for (uint32_t i = 0; i < p->nintents; i++) {
        const struct intent *in = &p->intents[i];
        if (committed(p, in->tid) && !finished(p, in->id) && in->type != XFS_LI_EFI) {
            kprintf("xfs: replay: an unfinished intent of type 0x%x this cannot finish\n",
                    in->type);
            return KERR_NOTSUP;
        }
    }
    return KERR_OK;
}

/* ── applying items ──────────────────────────────────────────────────── */

/* The block that holds device block `blkno` plus `byte` bytes. */
static int block_at(struct xfs *fs, uint64_t blkno, uint32_t byte, struct bbuf **b,
                    uint32_t *within) {
    uint64_t at = blkno * XLOG_SECTOR + byte;
    *within = (uint32_t)(at % fs->blocksize);
    return bcache_get(fs->cache, at / fs->blocksize, b);
}

/* A b-tree fork root as it is logged — the in-core root, a long-form block
 * header then keys and pointers laid out for as many records as the logged
 * region holds — written as the on-disk root of a `bytes`-long fork: level,
 * count, then keys and pointers for as many as *that* holds. */
static int put_broot(const struct xfs *fs, const uint8_t *src, uint32_t srclen, uint8_t *dst,
                     uint32_t bytes) {
    uint32_t hdr = xfs_bmbt_hdr(fs);
    uint32_t smax = srclen > hdr ? (srclen - hdr) / (XFS_BMBT_KEY_BYTES + XFS_BMBT_PTR_BYTES) : 0;
    uint32_t dmax = bytes > XFS_BMDR_HDR_BYTES
                        ? (bytes - XFS_BMDR_HDR_BYTES) / (XFS_BMBT_KEY_BYTES + XFS_BMBT_PTR_BYTES)
                        : 0;
    uint32_t n = srclen > hdr ? xfs_be16(src + 6u) : UINT32_MAX;
    if (n > smax || n > dmax) {
        return KERR_IO;
    }
    for (uint32_t i = 0; i < bytes; i++) {
        dst[i] = 0;
    }
    xfs_copy_bytes(dst, src + 4u, 4u); /* level and count */
    xfs_copy_bytes(dst + XFS_BMDR_HDR_BYTES, src + hdr, n * XFS_BMBT_KEY_BYTES);
    xfs_copy_bytes(dst + XFS_BMDR_HDR_BYTES + dmax * XFS_BMBT_KEY_BYTES,
                   src + hdr + smax * XFS_BMBT_KEY_BYTES, n * XFS_BMBT_PTR_BYTES);
    return KERR_OK;
}

/* One fork of a logged inode, from region `r`: inline bytes or an extent
 * list copied, a b-tree root converted. `kind` is the fork's three flags'
 * meaning in order: data, extents, root. */
static int put_fork(struct xfs *fs, uint8_t *di, uint32_t at, uint32_t bytes, uint32_t fields,
                    const uint32_t kind[3], const struct item *it, uint32_t r) {
    const uint8_t *src = itembuf + it->off[r];
    uint32_t len = it->len[r];
    if (at + bytes > fs->inodesize) {
        return KERR_IO;
    }
    if ((fields & kind[2]) != 0) {
        return put_broot(fs, src, len, di + at, bytes);
    }
    if (len > bytes) {
        return KERR_IO;
    }
    xfs_copy_bytes(di + at, src, len);
    return KERR_OK;
}

/* The forks a logged inode carries after its core, in the order the log
 * puts them: data (or a device number), then attributes. The core is
 * already in place, so `di_forkoff` says where the attribute fork starts. */
static int put_forks(struct xfs *fs, uint8_t *di, const uint8_t *ilf, const struct item *it) {
    static const uint32_t data[3] = {XFS_ILOG_DDATA, XFS_ILOG_DEXT, XFS_ILOG_DBROOT};
    static const uint32_t attr[3] = {XFS_ILOG_ADATA, XFS_ILOG_AEXT, XFS_ILOG_ABROOT};
    uint32_t fields = le32(ilf + ILF_FIELDS), r = 2;
    uint32_t split = (uint32_t)di[XFS_DI_FORKOFF] * 8u;
    uint32_t area = fs->inodesize - fs->dinode_size;
    int rc = KERR_OK;
    if ((fields & XFS_ILOG_DEV) != 0) {
        xfs_put_be32(di + fs->dinode_size, le32(ilf + ILF_RDEV));
    }
    if ((fields & (XFS_ILOG_DDATA | XFS_ILOG_DEXT | XFS_ILOG_DBROOT)) != 0) {
        rc = r < it->nregions ? put_fork(fs, di, fs->dinode_size, split == 0 ? area : split,
                                         fields, data, it, r++)
                              : KERR_IO;
    }
    if (rc == KERR_OK && (fields & (XFS_ILOG_ADATA | XFS_ILOG_AEXT | XFS_ILOG_ABROOT)) != 0) {
        rc = r < it->nregions && split != 0
                 ? put_fork(fs, di, fs->dinode_size + split, area - split, fields, attr, it, r)
                 : KERR_IO;
    }
    return rc;
}

/* Writes a logged inode back: its core, converted from the log's byte
 * order field by field (`xfs_log_dinode_to_disk`, which leaves the
 * unlinked pointer the log does not carry), then its forks.
 *
 * The item's own `blkno`/`boffset` locate the inode, rather than computing
 * them from the inode number: the log says where it was. */
static int apply_inode(struct xfs *fs, const struct pass *p, uint32_t seq) {
    const struct item *it = &p->it;
    const uint8_t *ilf = itembuf + it->off[0];
    uint32_t fields = le32(ilf + ILF_FIELDS), within = 0;
    uint64_t blkno = le64(ilf + ILF_BLKNO);
    struct bbuf *b;
    int rc;
    if (it->len[0] < ILF_STRUCT_SIZE || it->nregions < 2u || it->len[1] > fs->inodesize ||
        (fields & (XFS_ILOG_DOWNER | XFS_ILOG_AOWNER)) != 0) {
        return KERR_NOTSUP;
    }
    if (cancelled_later(p, blkno, seq)) {
        return KERR_OK; /* the chunk was freed after this */
    }
    rc = block_at(fs, blkno, le32(ilf + ILF_BOFFSET), &b, &within);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_log_dinode_to_disk(b->data + within, itembuf + it->off[1], fs->dinode_size);
    rc = put_forks(fs, b->data + within, ilf, it);
    xfs_reseal(fs, b->data + within, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return rc;
}

/* Copies the logged 128-byte chunk `i` of a buffer, from `data`. */
static int put_chunk(struct xfs *fs, uint64_t blkno, uint32_t i, const uint8_t *data) {
    struct bbuf *b;
    uint32_t within = 0;
    int rc = block_at(fs, blkno, i * BLF_CHUNK, &b, &within);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_copy_bytes(b->data + within, data, BLF_CHUNK);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

static bool chunk_logged(const uint8_t *blf, uint32_t i) {
    return (le32(blf + BLF_DATA_MAP + (i / 32u) * 4u) & ((uint32_t)1 << (i % 32u))) != 0;
}

/* A logged buffer's dirty chunks, written back. A buffer item names a
 * block and a bitmap of its 128-byte chunks; the regions after the format
 * hold those chunks back to back, so the count of set bits has to match
 * the data — a mismatch means the item is not what it says. */
static int apply_buffer(struct xfs *fs, const struct item *it) {
    const uint8_t *blf = itembuf + it->off[0];
    uint32_t maps = le32(blf + BLF_MAP_SIZE), at = 0, total = 0;
    const uint8_t *data = it->nregions > 1u ? itembuf + it->off[1] : itembuf;
    int rc = KERR_OK;
    for (uint32_t r = 1; r < it->nregions; r++) {
        total += it->len[r];
    }
    if (maps == 0 || maps > 16u || it->len[0] < BLF_DATA_MAP + maps * 4u) {
        return KERR_NOTSUP;
    }
    for (uint32_t i = 0; rc == KERR_OK && i < maps * 32u; i++) {
        if (!chunk_logged(blf, i)) {
            continue;
        }
        rc = at + BLF_CHUNK <= total ? put_chunk(fs, le64(blf + BLF_BLKNO), i, data + at)
                                     : KERR_IO;
        at += BLF_CHUNK;
    }
    return rc != KERR_OK ? rc : (at == total ? KERR_OK : KERR_IO);
}

/* Where a logged chunk's bytes are in the item's data: the set bits before
 * it say how many chunks precede it. */
static uint32_t chunk_data_at(const uint8_t *blf, uint32_t chunk) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < chunk; i++) {
        n += chunk_logged(blf, i) ? 1u : 0u;
    }
    return n * BLF_CHUNK;
}

/* An inode buffer: Linux logs only each inode's unlinked pointer through it
 * (the cores are inode items of their own), so only those four bytes are
 * taken — copying the whole chunk would put an older core over a newer
 * one. Each inode touched is resealed. */
static int apply_inode_buffer(struct xfs *fs, const struct item *it) {
    const uint8_t *blf = itembuf + it->off[0];
    uint32_t inodes = (uint32_t)le16(blf + BLF_LEN) * XLOG_SECTOR / fs->inodesize;
    uint64_t blkno = le64(blf + BLF_BLKNO);
    int rc = KERR_OK;
    for (uint32_t i = 0; rc == KERR_OK && i < inodes; i++) {
        uint32_t field = i * fs->inodesize + XFS_DI_NEXT_UNLINKED, within = 0;
        struct bbuf *b;
        if (it->nregions < 2u || !chunk_logged(blf, field / BLF_CHUNK)) {
            continue;
        }
        rc = block_at(fs, blkno, i * fs->inodesize, &b, &within);
        if (rc != KERR_OK) {
            return rc;
        }
        xfs_copy_bytes(b->data + within + XFS_DI_NEXT_UNLINKED,
                       itembuf + it->off[1] + chunk_data_at(blf, field / BLF_CHUNK) +
                           field % BLF_CHUNK,
                       4u);
        xfs_reseal(fs, b->data + within, fs->inodesize, XFS_DI_CRC);
        bcache_dirty(b);
        bcache_put(fs->cache, b);
    }
    return rc;
}

/* Where a sector-sized header's checksum is, by its magic; 0 for none. */
static uint32_t header_crc(uint32_t magic) {
    switch (magic) {
    case XFS_SB_MAGIC:
        return XFS_SB_CRC;
    case XFS_AGF_MAGIC:
        return XFS_AGF_CRC;
    case XFS_AGI_MAGIC:
        return XFS_AGI_CRC;
    case XFS_AGFL_MAGIC:
        return XFS_AGFL_CRC;
    default:
        return 0;
    }
}

/* Where a whole block's checksum is, by its magic; 0 for none. */
static uint32_t block_crc(uint32_t magic) {
    switch (magic) {
    case XFS_BNOBT_MAGIC:
    case XFS_CNTBT_MAGIC:
    case XFS_INOBT_MAGIC:
    case XFS_FINOBT_MAGIC:
    case XFS_RMAP_MAGIC:
    case XFS_REFC_MAGIC:
        return XFS_BTREE_CRC;
    case XFS_BMBT_CRC_MAGIC:
        return XFS_BMBT_CRC_OFF;
    case XFS_ATTR3_RMT_MAGIC:
        return ATTR_RMT_CRC;
    case XFS_SYMLINK_MAGIC:
        return XFS_SYMLINK_CRC;
    default:
        return 0;
    }
}

/* Reseals every record of a block that holds many: dquots, or inodes. */
static void reseal_records(struct xfs *fs, uint8_t *at, uint32_t bytes, uint32_t size,
                           uint32_t crc_off) {
    for (uint32_t i = 0; i + size <= bytes; i += size) {
        xfs_reseal(fs, at + i, size, crc_off);
    }
}

/* A replayed buffer's checksum, recomputed. **Linux logs a buffer as it
 * stands in memory, whose checksum is only computed when it is written
 * back** — so replayed blocks carry stale ones, and the first was the
 * superblock's. The block's own magic says which structure it is and so
 * where its checksum lives. A directory or attribute block larger than a
 * filesystem block is refused rather than resealed in part. */
static int reseal_buffer(struct xfs *fs, uint64_t blkno, uint32_t bytes) {
    struct bbuf *b;
    uint32_t within = 0, magic;
    uint16_t magic16;
    int rc = block_at(fs, blkno, 0, &b, &within);
    if (rc != KERR_OK) {
        return rc;
    }
    magic = xfs_be32(b->data + within);
    magic16 = xfs_be16(b->data + within + XFS_DA3_MAGIC_OFF);
    if (header_crc(magic) != 0) {
        xfs_reseal(fs, b->data + within, fs->sectsize, header_crc(magic));
    } else if (block_crc(magic) != 0) {
        xfs_reseal(fs, b->data + within, fs->blocksize, block_crc(magic));
    } else if (xfs_be16(b->data + within) == XFS_DQUOT_MAGIC) {
        reseal_records(fs, b->data + within, bytes, XFS_DQBLK_SIZE, XFS_DQ_CRC_OFF);
    } else if (xfs_be16(b->data + within) == XFS_DINODE_MAGIC) {
        reseal_records(fs, b->data + within, bytes, fs->inodesize, XFS_DI_CRC);
    } else if (bytes > fs->blocksize && (magic16 != 0 || (magic >> 8) == 0x584444u)) {
        rc = KERR_NOTSUP;
    } else {
        xfs_stamp_daddr(fs, b->data + within, blkno * XLOG_SECTOR / fs->blocksize);
    }
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return rc;
}

static int apply_buf_item(struct xfs *fs, const struct pass *p, uint32_t seq) {
    const uint8_t *blf = itembuf + p->it.off[0];
    uint16_t flags = p->it.len[0] >= BLF_DATA_MAP ? le16(blf + BLF_FLAGS) : 0;
    int rc;
    if (p->it.len[0] < BLF_DATA_MAP) {
        return KERR_NOTSUP;
    }
    if ((flags & XFS_BLF_CANCEL) != 0 || cancelled_later(p, le64(blf + BLF_BLKNO), seq)) {
        return KERR_OK;
    }
    if ((flags & XFS_BLF_INODE_BUF) != 0) {
        return apply_inode_buffer(fs, &p->it);
    }
    rc = apply_buffer(fs, &p->it);
    return rc != KERR_OK || !fs->v5
               ? rc
               : reseal_buffer(fs, le64(blf + BLF_BLKNO),
                               (uint32_t)le16(blf + BLF_LEN) * XLOG_SECTOR);
}

/* Fresh inodes for one block of a created chunk, as `mkfs` and Linux make
 * them: valid empty inodes with the chunk's generation number and a null
 * unlinked pointer — and on v5 their own number, the filesystem's UUID and
 * a checksum. */
static int fresh_inodes(struct xfs *fs, uint32_t agno, uint32_t agbno, uint32_t gen) {
    uint64_t fsb = ((uint64_t)agno << fs->agblklog) | agbno;
    uint64_t first = fsb << fs->inopblog;
    struct bbuf *b;
    int rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, fsb), &b);
    for (uint32_t i = 0; rc == KERR_OK && i < fs->inopblock; i++) {
        uint8_t *di = b->data + i * fs->inodesize;
        for (uint32_t j = 0; j < fs->inodesize; j++) {
            di[j] = 0;
        }
        xfs_put_be16(di + XFS_DI_MAGIC, XFS_DINODE_MAGIC);
        di[XFS_DI_VERSION] = fs->v5 ? XFS_DI_VERSION_3 : XFS_DI_VERSION_2;
        xfs_put_be32(di + XFS_DI_GEN, gen);
        xfs_put_be32(di + XFS_DI_NEXT_UNLINKED, XFS_NULL_AGINO);
        if (fs->v5) {
            xfs_put_be64(di + XFS_DI_INO, first + i);
            xfs_copy_bytes(di + XFS_DI_UUID, fs->uuid, 16u);
        }
        xfs_reseal(fs, di, fs->inodesize, XFS_DI_CRC);
    }
    if (rc == KERR_OK) {
        bcache_dirty(b);
        bcache_put(fs->cache, b);
    }
    return rc;
}

/* An inode-create item: the chunk's blocks filled with fresh inodes. Unlike
 * the other formats, its fields after the type and size are **big-endian**
 * (Linux declares them `__be32`); read in the log's host order they named
 * an allocation group and a size nothing has. */
static int apply_icreate(struct xfs *fs, const struct item *it) {
    const uint8_t *icl = itembuf + it->off[0];
    uint32_t agno, agbno, count, length;
    int rc = KERR_OK;
    if (it->len[0] < ICR_SIZE) {
        return KERR_NOTSUP;
    }
    agno = xfs_be32(icl + ICR_AG);
    agbno = xfs_be32(icl + ICR_AGBNO);
    count = xfs_be32(icl + ICR_COUNT);
    length = xfs_be32(icl + ICR_LENGTH);
    if (agno >= fs->agcount || xfs_be32(icl + ICR_ISIZE) != fs->inodesize ||
        (uint64_t)count * fs->inodesize != (uint64_t)length * fs->blocksize) {
        return KERR_IO;
    }
    for (uint32_t k = 0; rc == KERR_OK && k < length; k++) {
        rc = fresh_inodes(fs, agno, agbno + k, xfs_be32(icl + ICR_GEN));
    }
    return rc;
}

/* A dquot item: the logged dquot copied into its block, resealed on v5. */
static int apply_dquot(struct xfs *fs, const struct pass *p, uint32_t seq) {
    const struct item *it = &p->it;
    const uint8_t *qlf = itembuf + it->off[0];
    uint32_t within = 0;
    struct bbuf *b;
    int rc;
    if (it->len[0] < QLF_SIZE || it->nregions < 2u || it->len[1] > XFS_DQBLK_SIZE) {
        return KERR_NOTSUP;
    }
    if (cancelled_later(p, le64(qlf + QLF_BLKNO), seq)) {
        return KERR_OK;
    }
    rc = block_at(fs, le64(qlf + QLF_BLKNO), le32(qlf + QLF_BOFFSET), &b, &within);
    if (rc != KERR_OK) {
        return rc;
    }
    if (within + XFS_DQBLK_SIZE <= fs->blocksize) {
        xfs_copy_bytes(b->data + within, itembuf + it->off[1], it->len[1]);
        xfs_reseal(fs, b->data + within, XFS_DQBLK_SIZE, XFS_DQ_CRC_OFF);
        bcache_dirty(b);
    }
    bcache_put(fs->cache, b);
    return within + XFS_DQBLK_SIZE <= fs->blocksize ? KERR_OK : KERR_IO;
}

/* Pass 2's handling of one committed item. Intents are finished after the
 * walk; done items and quota-off records change nothing by themselves.
 * Anything else this does not know aborts the replay. */
static int apply_item(struct xfs *fs, struct pass *p, uint32_t seq, uint16_t type) {
    switch (type) {
    case XFS_LI_INODE:
        return apply_inode(fs, p, seq);
    case XFS_LI_BUF:
        return apply_buf_item(fs, p, seq);
    case XFS_LI_ICREATE:
        return apply_icreate(fs, &p->it);
    case XFS_LI_DQUOT:
        return apply_dquot(fs, p, seq);
    case XFS_LI_QUOTAOFF:
        return KERR_OK;
    default:
        if (is_intent(type) || is_done(type)) {
            return KERR_OK;
        }
        kprintf("xfs: replay: item type 0x%x is not one this reader applies\n", type);
        return KERR_NOTSUP;
    }
}

/* ── assembling items from operations ───────────────────────────────── */

/* A whole item has been assembled: noted (pass 1) or, when its transaction
 * committed, applied (pass 2). */
static int item_done(struct xfs *fs, struct pass *p, uint32_t tid) {
    uint16_t type = le16(itembuf + p->it.off[0]);
    uint32_t seq = p->seq++;
    int rc = KERR_OK;
    if (!p->applying) {
        rc = note_item(p, tid, seq, type);
    } else if (committed(p, tid)) {
        rc = apply_item(fs, p, seq, type);
        p->items_applied += rc == KERR_OK ? 1u : 0u;
    }
    p->it = (struct item){.tid = 0};
    return rc;
}

/* A region is complete. The first of a transaction may be its header,
 * which is not an item; the first of an item is its format, whose second
 * u16 says how many regions the item has. */
static int region_done(struct xfs *fs, struct pass *p, uint32_t tid) {
    struct item *it = &p->it;
    const uint8_t *first = itembuf + it->off[0];
    if (it->nregions == 1u && it->len[0] == TRANS_HDR_SIZE &&
        le32(first + TRANS_HDR_MAGIC) == XFS_TRANS_HEADER_MAGIC) {
        *it = (struct item){.tid = 0};
        return KERR_OK;
    }
    if (it->want == 0) {
        it->want = it->len[0] >= 4u ? le16(first + 2u) : 0;
        if (it->want == 0 || it->want > ITEM_REGIONS_MAX) {
            return KERR_IO;
        }
    }
    return it->nregions == it->want ? item_done(fs, p, tid) : KERR_OK;
}

/* One operation's payload, added to the item being assembled: a region of
 * its own, or more of the last one when the operation continues a region
 * split across records. */
static int add_payload(struct xfs *fs, struct pass *p, uint32_t tid, uint8_t flags,
                       const uint8_t *data, uint32_t len) {
    struct item *it = &p->it;
    bool more = (flags & XLOG_WAS_CONT_TRANS) != 0 && it->open_region;
    if ((it->nregions != 0 && it->tid != tid) || it->used + len > ITEM_MAX_BYTES ||
        (!more && it->nregions == ITEM_REGIONS_MAX)) {
        return KERR_NOTSUP; /* items of two transactions interleaved, or too large */
    }
    if (!more) {
        it->off[it->nregions] = it->used;
        it->len[it->nregions++] = 0;
    }
    it->tid = tid;
    xfs_copy_bytes(itembuf + it->used, data, len);
    it->used += len;
    it->len[it->nregions - 1u] += len;
    it->open_region = (flags & XLOG_CONTINUE_TRANS) != 0;
    return it->open_region ? KERR_OK : region_done(fs, p, tid);
}

/* A commit, recorded on the first pass and counted on the second. */
static int note_commit(struct pass *p, uint32_t tid) {
    p->in_txn = false;
    if (p->applying) {
        p->applied += committed(p, tid) ? 1u : 0u;
        return KERR_OK;
    }
    if (committed(p, tid)) {
        return KERR_OK;
    }
    if (p->ncommitted == COMMITTED_MAX) {
        return KERR_NOTSUP; /* more transactions than this tracks: refused, not guessed */
    }
    p->committed[p->ncommitted++] = tid;
    return KERR_OK;
}

/* One operation: the framing of a transaction (start, commit) or a piece
 * of one of its items. Transactions are `TRANS`'s; this kernel's writer
 * once stamped them `LOG`, so a log it left dirty says that, and both are
 * read. The unmount record carries nothing to apply, and another client's
 * operation is not one this knows. */
static int take_op(struct xfs *fs, struct pass *p, const uint8_t *op, uint32_t len) {
    uint32_t tid = xfs_be32(op + XLOG_OP_TID);
    uint8_t flags = op[XLOG_OP_FLAGS], client = op[XLOG_OP_CLIENTID];
    if ((flags & XLOG_UNMOUNT_TRANS) != 0) {
        return KERR_OK;
    }
    if (client != XLOG_CLIENT_TRANS && client != XLOG_CLIENT_LOG) {
        return KERR_NOTSUP;
    }
    if ((flags & XLOG_START_TRANS) != 0) {
        p->in_txn = true;
        p->txn_tid = tid;
        return KERR_OK;
    }
    if ((flags & XLOG_COMMIT_TRANS) != 0) {
        return note_commit(p, tid);
    }
    return len == 0 ? KERR_OK
                    : add_payload(fs, p, tid, flags, op + XLOG_OP_HDR_SIZE, len);
}

/* One record's operations, in order. */
static int walk_ops(struct xfs *fs, uint32_t bytes, uint32_t nops, struct pass *p) {
    uint32_t at = XLOG_REC_HDR_SIZE, end = XLOG_REC_HDR_SIZE + bytes;
    for (uint32_t i = 0; i < nops; i++) {
        uint32_t len;
        int rc;
        if (at + XLOG_OP_HDR_SIZE > end) {
            return KERR_IO;
        }
        len = xfs_be32(record + at + XLOG_OP_LEN);
        if (at + XLOG_OP_HDR_SIZE + len > end) {
            return KERR_IO;
        }
        rc = take_op(fs, p, record + at, len);
        if (rc != KERR_OK) {
            return rc;
        }
        at += XLOG_OP_HDR_SIZE + len;
    }
    return KERR_OK;
}

/* Every record from the tail the newest one names up to the head, round
 * the ring.
 *
 * **From the tail, not from block 0.** On a log Linux left dirty the walk
 * from block 0 would apply records already checkpointed — older metadata
 * over newer — and miss the ones written before the wrap. A block that is
 * not a record header where one must be ends the replay as a failure. */
static int replay_ring(struct xfs *fs, struct pass *p) {
    uint64_t blocks = log_blocks(fs), bb = fs->log_tail;
    p->seq = 0;
    p->it = (struct item){.tid = 0};
    for (uint64_t guard = 0; bb != fs->log_head; guard++) {
        uint32_t bytes = 0, nops = 0;
        int rc = guard < blocks ? read_record(fs, bb, &bytes, &nops) : KERR_IO;
        rc = rc == KERR_NOENT ? KERR_IO : rc;
        rc = rc != KERR_OK ? rc : walk_ops(fs, bytes, nops, p);
        if (rc != KERR_OK) {
            return rc;
        }
        bb = (bb + xfs_log_record_blocks(record)) % blocks;
    }
    return KERR_OK;
}

/* The extents of every committed extent-free intent that no done item
 * answers: the free was logged and its completion never was, so the
 * free-space trees do not yet have them. Freed there; the reverse map is
 * the business of its own intents, which a log with one unfinished is
 * refused for. */
static int finish_efis(struct xfs *fs, const struct pass *p) {
    int rc = KERR_OK;
    for (uint32_t i = 0; rc == KERR_OK && i < p->nintents; i++) {
        const struct intent *in = &p->intents[i];
        if (in->type != XFS_LI_EFI || !committed(p, in->tid) || finished(p, in->id)) {
            continue;
        }
        for (uint32_t k = 0; rc == KERR_OK && k < in->nextents; k++) {
            uint64_t mask = ((uint64_t)1 << fs->agblklog) - 1u;
            rc = xfs_free_extent(fs, xfs_fsb_ag(fs, in->start[k]),
                                 (uint32_t)(in->start[k] & mask), in->len[k]);
        }
    }
    return rc;
}

/* One group's share of the summary counters. */
static int count_group(struct xfs *fs, uint32_t agno, uint64_t *icount, uint64_t *ifree,
                       uint64_t *fdblocks) {
    struct bbuf *b;
    uint8_t *hdr;
    int rc = xfs_agi_get(fs, agno, &b, &hdr);
    if (rc != KERR_OK) {
        return rc;
    }
    *icount += xfs_be32(hdr + XFS_AGI_COUNT);
    *ifree += xfs_be32(hdr + XFS_AGI_FREECOUNT);
    bcache_put(fs->cache, b);
    rc = xfs_agf_get(fs, agno, &b, &hdr);
    if (rc != KERR_OK) {
        return rc;
    }
    *fdblocks += (uint64_t)xfs_be32(hdr + XFS_AGF_FREEBLKS) + xfs_be32(hdr + XFS_AGF_FLCOUNT) +
                 xfs_be32(hdr + XFS_AGF_BTREEBLKS);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* The superblock's summary counters, recounted from the groups' headers.
 * **Linux keeps them lazily** — in memory, written only at unmount — and
 * never logs them, so after its log is replayed the superblock's are from
 * whenever it was last unmounted; Linux recounts them at mount, and so
 * does this. Free blocks are what `xfs_repair` counts: free, on the free
 * list, and in the trees beyond their roots. */
static int recount_summary(struct xfs *fs) {
    uint64_t icount = 0, ifree = 0, fdblocks = 0;
    struct bbuf *b;
    int rc = KERR_OK;
    for (uint32_t agno = 0; rc == KERR_OK && agno < fs->agcount; agno++) {
        rc = count_group(fs, agno, &icount, &ifree, &fdblocks);
    }
    rc = rc != KERR_OK ? rc : bcache_get(fs->cache, 0, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_put_be64(b->data + XFS_SB_ICOUNT, icount);
    xfs_put_be64(b->data + XFS_SB_IFREE, ifree);
    xfs_put_be64(b->data + XFS_SB_FDBLOCKS, fdblocks);
    xfs_reseal(fs, b->data, fs->sectsize, XFS_SB_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

int xfs_log_replay(struct xfs *fs) {
    static struct pass p; /* tens of KiB of bookkeeping: not for a kernel stack */
    int rc;
    p = (struct pass){.applying = false};
    rc = replay_ring(fs, &p);
    rc = rc != KERR_OK ? rc : intents_finishable(&p);
    p.applying = true;
    rc = rc != KERR_OK ? rc : replay_ring(fs, &p);
    rc = rc != KERR_OK ? rc : finish_efis(fs, &p);
    rc = rc != KERR_OK ? rc : recount_summary(fs);
    if (rc != KERR_OK) {
        kprintf("xfs: replay abandoned after %u transaction(s): rc %d; nothing written\n",
                p.applied, rc);
        bcache_invalidate(fs->cache); /* drop every change the pass had made */
        return rc;
    }
    kprintf("xfs: replay applied %u transaction(s), %u item(s)\n", p.applied, p.items_applied);
    rc = bcache_flush(fs->cache);
    /* The log is clean once an unmount record says so. Without this the
     * next mount would replay the same transactions again — and the
     * filesystem would never come back writable. */
    return rc != KERR_OK ? rc : xfs_log_mark_clean(fs);
}
