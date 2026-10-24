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
 *     rather than being skipped. Skipping one item of a transaction and
 *     applying the rest is the one outcome worse than not replaying at
 *     all, because it produces a state the filesystem never had;
 *   - the check (`xfs_check`) runs *first*. Replaying into metadata that
 *     is already broken multiplies the damage.
 *
 * An abort is not a failure: the caller falls back to the read-only mount,
 * which is where this filesystem was before any of this existed.
 *
 * **What it understands.** Inode items, which is what this kernel's own
 * writer produces, and buffer items, which is what Linux logs most
 * metadata as. Anything else — extent-freeing intents, inode creation,
 * quotas — is refused, and the filesystem stays read-only until a kernel
 * that understands them has a turn.
 */
#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs_format.h"
#include "core/xfs/xfs_internal.h"

/* One record's worth of the log, which is what an operation may span.
 *
 * Static for the reason `xfs_log.c` gives about its own buffer: a kernel
 * stack is 1 KiB and this is 32 KiB. Mounting is not reentrant and the
 * kernel is not preemptible, so one buffer is enough — and it is the same
 * trade the check next door already makes. */
#define REPLAY_MAX_BYTES (64u * XLOG_SECTOR)
static uint8_t record[REPLAY_MAX_BYTES];

/* What the walk is in the middle of.
 *
 * `tid` identifies the transaction whose items are being collected, and
 * `items` is how many its header promised. The *pending* count is what
 * makes "committed" meaningful: a record can end mid-transaction and the
 * next one continues it, so the decision to apply cannot be made per
 * record. */
struct txn {
    uint32_t tid;
    uint32_t items;
    bool open;
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
    int rc = fs->log_cache == NULL
                 ? KERR_NODEV
                 : bcache_pread(fs->log_cache, fs->log_base + bb * XLOG_SECTOR, record,
                                XLOG_SECTOR);
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfs_be32(record + XLOG_REC_MAGIC) != XLOG_MAGIC) {
        return KERR_NOENT; /* not a record: the walk has run off the end */
    }
    len = xfs_be32(record + XLOG_REC_LEN);
    sectors = (len + XLOG_SECTOR - 1u) / XLOG_SECTOR;
    if (len == 0 || sectors * XLOG_SECTOR > REPLAY_MAX_BYTES - XLOG_REC_HDR_SIZE ||
        sectors > 64u) {
        return KERR_NOTSUP; /* a record larger than this can hold */
    }
    rc = bcache_pread(fs->log_cache, fs->log_base + (bb + 1u) * XLOG_SECTOR,
                      record + XLOG_REC_HDR_SIZE, sectors * XLOG_SECTOR);
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint32_t i = 0; i < sectors; i++) {
        uint8_t *sector = record + XLOG_REC_HDR_SIZE + i * XLOG_SECTOR;
        xfs_copy_bytes(sector, record + XLOG_REC_CYCLE_DATA + i * 4u, 4u);
    }
    *bytes = len;
    *nops = xfs_be32(record + XLOG_REC_NUM_OPS);
    return KERR_OK;
}

/* ── applying one item ───────────────────────────────────────────────── */

/* Writes a logged inode core back into its inode.
 *
 * Only the **core** is applied. An inode item can carry its data fork, its
 * attribute fork and more besides, each in its own operation, and a replay
 * that applied the core while silently dropping a fork would produce an
 * inode whose size and extent list disagreed — the single nastiest shape a
 * filesystem can be in, because every read of it looks valid. So a fork
 * that is present and not understood aborts the replay instead.
 *
 * The item's own `blkno`/`boffset` locate the inode, rather than computing
 * them from the inode number: the log says where it was, and that is the
 * authority here. */
static int apply_inode(struct xfs *fs, const uint8_t *ilf, const uint8_t *core,
                       uint32_t core_len) {
    uint64_t blkno = le64(ilf + ILF_BLKNO);
    uint32_t boffset = le32(ilf + ILF_BOFFSET);
    uint32_t fields = le32(ilf + ILF_FIELDS);
    struct bbuf *b;
    int rc;
    if ((fields & ~(uint32_t)XFS_ILOG_CORE) != 0) {
        kprintf("xfs: replay: an inode item logs fields this reader cannot apply (0x%x)\n",
                fields);
        return KERR_NOTSUP;
    }
    if (core_len > fs->inodesize || boffset + core_len > fs->blocksize) {
        return KERR_IO;
    }
    /* The item's block is a 512-byte device block; the cache works in
     * filesystem blocks, so the offset is in bytes either way. */
    rc = bcache_get(fs->cache, blkno * XLOG_SECTOR / fs->blocksize, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    /* **Converted, not copied.** See `xfs_log_dinode_to_disk`: the logged
     * core is in the host's byte order and the inode is big-endian. */
    xfs_log_dinode_to_disk(b->data + boffset, core);
    xfs_reseal(b->data + boffset, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* Writes a logged buffer's dirty chunks back into its block.
 *
 * A buffer item names a block and carries a **bitmap** of which 128-byte
 * chunks of it the record holds, followed by those chunks back to back. So
 * the data is unpacked by walking the bitmap, and the count of set bits has
 * to match the data that follows — a mismatch means the item is not what it
 * says, and writing the chunks that did line up would scatter them at the
 * wrong offsets.
 *
 * Runs to 29 lines: it is one derivation — the bitmap, the chunk it names,
 * the source offset that advances with it, and the destination block — and
 * no part of it can be checked without the others in front of you (Rule 11,
 * Single derivation). */
static int apply_buffer(struct xfs *fs, const uint8_t *blf, const uint8_t *data,
                        uint32_t data_len) {
    uint64_t blkno = le64(blf + BLF_BLKNO);
    uint32_t maps = le32(blf + BLF_MAP_SIZE);
    uint32_t bits = maps * 32u;
    uint32_t at = 0;
    struct bbuf *b;
    int rc;
    if (maps == 0 || maps > 64u) {
        return KERR_NOTSUP;
    }
    rc = bcache_get(fs->cache, blkno * XLOG_SECTOR / fs->blocksize, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint32_t i = 0; i < bits; i++) {
        uint32_t word = le32(blf + BLF_DATA_MAP + (i / 32u) * 4u);
        uint32_t into = i * BLF_CHUNK;
        if ((word & ((uint32_t)1 << (i % 32u))) == 0) {
            continue;
        }
        if (at + BLF_CHUNK > data_len || into + BLF_CHUNK > fs->blocksize) {
            bcache_put(fs->cache, b);
            return KERR_IO; /* the bitmap and the data disagree */
        }
        xfs_copy_bytes(b->data + into, data + at, BLF_CHUNK);
        at += BLF_CHUNK;
    }
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return at == data_len ? KERR_OK : KERR_IO;
}

/* ── walking the log ─────────────────────────────────────────────────── */

/* What a pass over the records is accumulating. `applied` is reported, not
 * for interest, but because a replay that applied nothing and a replay that
 * never ran look identical from outside. */
struct pass {
    struct txn txn;
    uint32_t applied;
    uint32_t items_seen;
};

/* Applies one item, which is two operations: the format structure and the
 * data it describes. Returns how many operations it consumed. */
static int apply_item(struct xfs *fs, const uint8_t *fmt, uint32_t fmt_len,
                      const uint8_t *data, uint32_t data_len) {
    uint16_t type = le16(fmt + ILF_TYPE);
    if (type == XFS_LI_INODE && fmt_len >= ILF_STRUCT_SIZE) {
        return apply_inode(fs, fmt, data, data_len);
    }
    if (type == XFS_LI_BUF && fmt_len >= BLF_DATA_MAP) {
        return apply_buffer(fs, fmt, data, data_len);
    }
    /* **Refused, not skipped.** Applying a transaction's other items and
     * dropping this one leaves the filesystem in a state it was never in,
     * which is worse than leaving it as the log found it. */
    kprintf("xfs: replay: item type 0x%x is not one this reader applies\n", type);
    return KERR_NOTSUP;
}

/* One record's operations, in order.
 *
 * The transaction framing is what this is really reading: `START_TRANS`
 * opens one, a header names how many items it has, pairs of operations
 * carry them, and `COMMIT_TRANS` is the point at which any of it may be
 * believed. An unfinished transaction at the end of the log — which is what
 * a crash leaves — reaches the end of the walk still open, and is dropped.
 *
 * Runs to 30 lines: it is one cursor over a byte range whose every step
 * depends on the operation header it just read, and splitting it would need
 * the buffer, the cursor, the remaining count and the transaction state
 * passed across the boundary (Rule 11, Parameters/Control flow). */
static int walk_ops(struct xfs *fs, uint32_t bytes, uint32_t nops, struct pass *p) {
    uint32_t at = XLOG_REC_HDR_SIZE;
    uint32_t end = XLOG_REC_HDR_SIZE + bytes;
    for (uint32_t i = 0; i < nops; i++) {
        uint32_t len, tid;
        uint8_t flags;
        if (at + XLOG_OP_HDR_SIZE > end) {
            return KERR_IO;
        }
        tid = xfs_be32(record + at + XLOG_OP_TID);
        len = xfs_be32(record + at + XLOG_OP_LEN);
        flags = record[at + XLOG_OP_FLAGS];
        at += XLOG_OP_HDR_SIZE;
        if (at + len > end) {
            return KERR_IO;
        }
        if ((flags & XLOG_START_TRANS) != 0) {
            p->txn = (struct txn){tid, 0, true};
        } else if ((flags & XLOG_COMMIT_TRANS) != 0) {
            p->applied += p->txn.open && p->txn.tid == tid ? 1u : 0u;
            p->txn.open = false;
        } else if (p->txn.open && len == TRANS_HDR_SIZE &&
                   le32(record + at + TRANS_HDR_MAGIC) == XFS_TRANS_HEADER_MAGIC) {
            p->txn.items = le32(record + at + TRANS_HDR_NUM_ITEMS);
        } else if (p->txn.open && len >= ILF_TYPE + 2u && i + 1u < nops) {
            uint32_t dlen = xfs_be32(record + at + len + XLOG_OP_LEN);
            int rc;
            if (at + len + XLOG_OP_HDR_SIZE + dlen > end) {
                return KERR_IO;
            }
            rc = apply_item(fs, record + at, len, record + at + len + XLOG_OP_HDR_SIZE, dlen);
            if (rc != KERR_OK) {
                return rc;
            }
            p->items_seen++;
            at += len + XLOG_OP_HDR_SIZE + dlen;
            i++;
            continue;
        }
        at += len;
    }
    return KERR_OK;
}

int xfs_log_replay(struct xfs *fs) {
    struct pass p = {{0, 0, false}, 0, 0};
    uint64_t bb = 0;
    uint64_t blocks = (uint64_t)fs->logblocks * (fs->blocksize / XLOG_SECTOR);
    int rc = KERR_OK;
    /* **Applied as the walk goes, and committed only at the end.** The
     * writes land in the block cache and the cache is flushed once, after
     * the whole log has been read without complaint — so a replay that
     * aborts halfway has changed nothing on the disk. That is the
     * all-or-nothing rule this file opens with, and it is the cache that
     * makes it cheap enough to keep. */
    while (bb + 1u < blocks) {
        uint32_t bytes = 0, nops = 0;
        rc = read_record(fs, bb, &bytes, &nops);
        if (rc == KERR_NOENT) {
            rc = KERR_OK;
            break; /* the end of what was written */
        }
        if (rc != KERR_OK) {
            break;
        }
        rc = walk_ops(fs, bytes, nops, &p);
        if (rc != KERR_OK) {
            break;
        }
        bb += 1u + (bytes + XLOG_SECTOR - 1u) / XLOG_SECTOR;
    }
    if (rc != KERR_OK) {
        kprintf("xfs: replay abandoned after %u transaction(s): rc %d; nothing written\n",
                p.applied, rc);
        bcache_invalidate(fs->cache); /* drop every change the pass had made */
        return rc;
    }
    kprintf("xfs: replay applied %u transaction(s), %u item(s)\n", p.applied, p.items_seen);
    rc = bcache_flush(fs->cache);
    /* The log is clean once an unmount record says so. Without this the
     * next mount would replay the same transactions again — harmless,
     * because applying them twice gives the same metadata, and still wrong:
     * the filesystem would never come back writable. */
    return rc != KERR_OK ? rc : xfs_log_mark_clean(fs);
}
