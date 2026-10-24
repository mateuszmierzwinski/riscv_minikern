#include "core/bcache.h"
#include "core/errno.h"
#include "core/xfs/xfs_internal.h"
#include "lib/crc32c.h"

#include <stddef.h>

/* ── writing the log ──────────────────────────────────────────────────────
 *
 * A metadata change goes to the log before it goes to its place, which is
 * the whole of what a journal is for. What this does **not** do is replay
 * one: the reader still refuses a dirty log (xfs.h), and Linux's
 * `xfs_repair` remains the recovery tool.
 *
 * That refusal buys a real simplification, and it is deliberate rather than
 * unfinished: because the log is always left clean, it is never a ring with
 * a head to maintain. Each batch is written from the log's **first block** —
 * the transaction record, then an unmount record after it — so the newest
 * thing in the log always says "put down properly" and the blocks beyond
 * are zeroes, whose cycle of 0 is what makes the head findable (xfs_log.c).
 * A kernel that replayed the log would need the ring; this one would gain
 * nothing from it but the bugs.
 *
 * The record format is the one the formatter already writes
 * (userland/internal/xfsmk/write_log.go) and the reader already parses: a
 * 512-byte header, then the operations, with every data sector's first word
 * replaced by the cycle number and the displaced word kept in the header.
 */

#define XLOG_REC_TAIL_LSN   24u
#define XLOG_REC_CRC        32u
#define XLOG_REC_PREV_BLOCK 36u
#define XLOG_REC_CYCLE_DATA 44u /* 64 entries, one per 512-byte data sector */
#define XLOG_REC_FMT        300u
#define XLOG_REC_FS_UUID    304u
#define XLOG_REC_SIZE       320u
#define XLOG_REC_STRUCT     324u /* what the CRC covers of the header */

#define XLOG_FMT_LINUX_LE 1u
#define XLOG_ICLOG_SIZE   32768u
#define XLOG_CYCLE        1u
#define XLOG_MAX_SECTORS  8u /* a transaction record: one header, seven of ops */

#define XLOG_UNMOUNT_TID  0xb0c0d0d0u /* what the formatter writes, and what xfs_logprint expects */
#define XFS_LOG_TID       0x6d6b7472u /* "mktr": ours, and a log of ours is the only one we write */
/* The transaction header, which is an **operation of its own**: the start
 * operation carries no data at all, and recovery checks this magic on the
 * first operation that does. Getting that wrong — putting the header on the
 * start operation — is what `xfs_logprint` was reporting as "bad header
 * magic number", and reading our own bytes back out of the image is what
 * settled where it belongs. Its three words are native-endian, like the
 * unmount record's magic, because recovery reads them as plain `uint`s. */
#define XFS_TRANS_HEADER_MAGIC 0x5452414eu /* "TRAN" */
#define XFS_TRANS_SETATTR_SIZE 2u          /* what a size change is */
#define TRANS_HDR_MAGIC     0u  /* u32 */
#define TRANS_HDR_TYPE      4u  /* u32 */
#define TRANS_HDR_TID       8u  /* u32 */
#define TRANS_HDR_NUM_ITEMS 12u /* u32 */
#define TRANS_HDR_SIZE      16u /* **measured**: recovery registers an item only
                                 * when the operation's length is exactly this,
                                 * and at 12 it silently did not — after which it
                                 * read the next operation as another header and
                                 * reported a bad magic number. */

#define XLOG_START_TRANS  0x01u
#define XLOG_COMMIT_TRANS 0x02u

/* The inode item's format header, as the log carries it (56 bytes in v5). */
#define XFS_LI_INODE     0x123bu
#define XFS_ILOG_CORE    0x0001u
#define ILF_TYPE         0u  /* u16 */
#define ILF_SIZE         2u  /* u16: how many operations this item occupies */
#define ILF_FIELDS       4u  /* u32 */
#define ILF_ASIZE        8u  /* u16 */
#define ILF_DSIZE        10u /* u16 */
#define ILF_INO          16u /* u64 */
#define ILF_BLKNO        40u /* u64: the inode chunk's device block */
#define ILF_LEN          48u /* u32: its length in 512-byte blocks */
#define ILF_BOFFSET      52u /* u32: the inode's byte offset within it */
#define ILF_STRUCT_SIZE  56u

/* The logged form of an inode core: 176 bytes at the **same offsets** as the
 * on-disk core, but **native-endian**. Confirmed field by field against
 * `xfs_logprint`, which reads back the mode, version, format, size, block
 * count and extent count it was given (XFS.md §9). */
#define XFS_LOG_DINODE_SIZE 176u

static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v) {
    put_be16(p, (uint16_t)(v >> 16));
    put_be16(p + 2, (uint16_t)v);
}

static void put_be64(uint8_t *p, uint64_t v) {
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

static void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le64(uint8_t *p, uint64_t v) {
    for (uint32_t i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void copy(uint8_t *dst, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

static void zero(uint8_t *dst, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = 0;
    }
}

/* Every multi-byte field of an inode core, so that each can be turned round
 * on its own. The timestamps are **two 32-bit words each**, not one 64-bit
 * one, and reversing them as a single quantity would swap the seconds with
 * the nanoseconds. The two gaps — the pad at 24 and the on-disk
 * `di_next_unlinked`/`di_crc` at 96 — have no counterpart in the logged form
 * and are left as zeroes. */
static const struct {
    uint8_t off;
    uint8_t len;
} CORE_FIELDS[] = {
    {0, 2},   {2, 2},   {4, 1},   {5, 1},   {6, 2},   {8, 4},   {12, 4},  {16, 4},  {20, 2},  {22, 2},
    {30, 2},  {32, 4},  {36, 4},  {40, 4},  {44, 4},  {48, 4},  {52, 4},  {56, 8},  {64, 8},  {72, 4},
    {76, 4},  {80, 2},  {82, 1},  {83, 1},  {84, 4},  {88, 2},  {90, 2},  {92, 4},  {104, 8}, {112, 8},
    {120, 8}, {128, 4}, {144, 4}, {148, 4}, {152, 8},
};

/* Turns an on-disk inode core into the form the log carries: the same
 * fields in the same places, each one reversed, and the UUID copied as the
 * bytes it is. */
static void log_dinode_from_disk(uint8_t *out, const uint8_t *di) {
    zero(out, XFS_LOG_DINODE_SIZE);
    for (uint32_t f = 0; f < sizeof CORE_FIELDS / sizeof CORE_FIELDS[0]; f++) {
        uint32_t off = CORE_FIELDS[f].off, len = CORE_FIELDS[f].len;
        for (uint32_t i = 0; i < len; i++) {
            out[off + i] = di[off + len - 1u - i];
        }
    }
    copy(out + XFS_DI_UUID, di + XFS_DI_UUID, 16);
}

/* A record under construction: the header sector, then the operation
 * sectors. Static because it is larger than a kernel stack may carry
 * (CLAUDE.md §10) and safe for the same reason every static buffer here is:
 * one core is inside the kernel at a time, and nothing in a record's
 * construction sleeps. */
static uint8_t record[XLOG_MAX_SECTORS * XLOG_SECTOR];
static uint32_t record_used; /* bytes of operations, past the header */

/* One operation header plus its payload. */
static int put_op(uint32_t tid, uint8_t flags, const uint8_t *data, uint32_t len) {
    uint8_t *op = record + XLOG_REC_HDR_SIZE + record_used;
    if (XLOG_REC_HDR_SIZE + record_used + XLOG_OP_HDR_SIZE + len > sizeof record) {
        return KERR_2BIG;
    }
    put_be32(op + XLOG_OP_TID, tid);
    put_be32(op + XLOG_OP_LEN, len);
    op[XLOG_OP_CLIENTID] = XLOG_CLIENT_LOG;
    op[XLOG_OP_FLAGS] = flags;
    if (len > 0) {
        copy(op + XLOG_OP_HDR_SIZE, data, len);
    }
    record_used += XLOG_OP_HDR_SIZE + len;
    return KERR_OK;
}

/* Replaces each data sector's first word with the cycle number, keeping the
 * word it displaced in the header. This is how a reader finds the head of
 * the log by reading one word per sector, and the record's checksum covers
 * the result, not the original. */
static void pack_cycles(uint8_t *hdr, uint32_t sectors) {
    for (uint32_t i = 0; i < sectors; i++) {
        uint8_t *sector = record + XLOG_REC_HDR_SIZE + i * XLOG_SECTOR;
        copy(hdr + XLOG_REC_CYCLE_DATA + i * 4u, sector, 4);
        put_be32(sector, XLOG_CYCLE);
    }
}

/* The record header. lsn and tail are the same: nothing is left
 * outstanding, because the unmount record that follows says so. */
static void put_header(const struct xfs *fs, uint64_t block, uint32_t sectors, uint32_t nops) {
    uint8_t *hdr = record;
    put_be32(hdr + XLOG_REC_MAGIC, XLOG_MAGIC);
    put_be32(hdr + XLOG_REC_CYCLE, XLOG_CYCLE);
    put_be32(hdr + XLOG_REC_VERSION, 2);
    put_be32(hdr + XLOG_REC_LEN, sectors * XLOG_SECTOR);
    put_be64(hdr + XLOG_REC_LSN, ((uint64_t)XLOG_CYCLE << 32) | block);
    put_be64(hdr + XLOG_REC_TAIL_LSN, ((uint64_t)XLOG_CYCLE << 32) | block);
    put_be32(hdr + XLOG_REC_PREV_BLOCK, 0xffffffffu);
    put_be32(hdr + XLOG_REC_NUM_OPS, nops);
    put_be32(hdr + XLOG_REC_FMT, XLOG_FMT_LINUX_LE);
    copy(hdr + XLOG_REC_FS_UUID, fs->uuid, 16);
    put_be32(hdr + XLOG_REC_SIZE, XLOG_ICLOG_SIZE);
}

/* The record's checksum: over the header as a structure — with the checksum
 * field itself counted as zero — and then over the packed data. */
static void seal(uint32_t sectors) {
    uint32_t crc = CRC32C_SEED;
    put_le32(record + XLOG_REC_CRC, 0);
    crc = crc32c_update(crc, record, XLOG_REC_STRUCT);
    crc = crc32c_update(crc, record + XLOG_REC_HDR_SIZE, sectors * XLOG_SECTOR);
    put_le32(record + XLOG_REC_CRC, ~crc);
}

/* Writes a built record at a log block.
 *
 * **The log is addressed in 512-byte basic blocks**, not in filesystem
 * blocks, and a record's `h_lsn` names one of those. Writing at a
 * filesystem block while the header claimed a basic block put the unmount
 * record eight blocks from where it said it was: our own reader still found
 * it, because it scans every block, but `xfs_repair` compared the head with
 * the tail, found them inconsistent, and declared the log to hold metadata
 * changes that had to be replayed. */
static int flush_record(struct xfs *fs, uint64_t bb, uint32_t sectors) {
    uint64_t base = xfs_fsb_to_block(fs, fs->logstart) * (uint64_t)fs->blocksize;
    uint32_t bytes = XLOG_REC_HDR_SIZE + sectors * XLOG_SECTOR;
    return bcache_pwrite(fs->cache, base + bb * XLOG_SECTOR, record, bytes);
}

/* Zeroes the blocks after a batch, so that nothing a *previous* batch wrote
 * is still there.
 *
 * The head of the log is found by looking at one word per basic block for
 * the cycle number (xfs_log.c), so a block left over from a longer batch
 * still carries that cycle and still looks like part of the log. A smaller
 * batch written over a larger one duly left one, and `xfs_repair`
 * reported a log with "valuable metadata changes" — it had found that
 * orphan past the unmount record and had no reason to think it was rubbish.
 *
 * The window is as long as the longest record this writer can produce plus
 * its unmount record, which is what makes it enough: nothing it could have
 * written before reaches further. */
#define XLOG_CLEAR_BLOCKS (XLOG_MAX_SECTORS + 1u + 2u)

static int clear_after(struct xfs *fs, uint64_t bb) {
    uint64_t base = xfs_fsb_to_block(fs, fs->logstart) * (uint64_t)fs->blocksize;
    uint32_t left = XLOG_CLEAR_BLOCKS;
    /* In passes the size of the record buffer, because the window to clear
     * is longer than that buffer: zeroing all of it in one go wrote past
     * the end of `record` and over whatever static followed it, which came
     * back as a kernel that spun instead of finishing. */
    zero(record, sizeof record);
    while (left > 0) {
        uint32_t now = left < XLOG_MAX_SECTORS ? left : XLOG_MAX_SECTORS;
        int rc = bcache_pwrite(fs->cache, base + bb * XLOG_SECTOR, record, now * XLOG_SECTOR);
        if (rc != KERR_OK) {
            return rc;
        }
        bb += now;
        left -= now;
    }
    return KERR_OK;
}

/* The unmount record, which says the filesystem was put down properly. It
 * is the same shape the formatter writes, and writing it after every batch
 * is what keeps the log clean — and so what keeps our own reader, which
 * refuses a dirty one, able to mount what it just wrote. */
static int write_unmount(struct xfs *fs, uint64_t bb) {
    uint8_t magic[8];
    int rc;
    zero(record, sizeof record);
    record_used = 0;
    zero(magic, sizeof magic);
    put_be16(magic, XLOG_UNMOUNT_MAGIC);
    rc = put_op(XLOG_UNMOUNT_TID, XLOG_UNMOUNT_TRANS, magic, sizeof magic);
    if (rc != KERR_OK) {
        return rc;
    }
    put_header(fs, bb, 1, 1);
    pack_cycles(record, 1);
    seal(1);
    rc = flush_record(fs, bb, 1);
    return rc != KERR_OK ? rc : clear_after(fs, bb + 2u);
}

/* How many 512-byte sectors the built operations occupy. */
static uint32_t used_sectors(void) {
    return (record_used + XLOG_SECTOR - 1u) / XLOG_SECTOR;
}

/* Logs one inode core and commits, then leaves the log clean.
 *
 * The transaction is five operations: a start that carries nothing, the
 * transaction header, the item's format header (which inode, and which
 * parts of it), the item's data — the core itself — and a commit. Runs to 29 lines: the sequence is one record built in order and
 * then two writes that must happen in that order, and splitting it would
 * hand a half-built static record to a second function (Rule 11, Naming/
 * Locking). */
/* One inode item: the format header that says which inode and which parts
 * of it, then the core itself. Two operations, as its ilf_size says. */
static int put_inode_item(const struct xfs *fs, const struct xfs_log_inode_item *item) {
    uint8_t ilf[ILF_STRUCT_SIZE];
    uint8_t logged[XFS_LOG_DINODE_SIZE];
    int rc;
    zero(ilf, sizeof ilf);
    put_le16(ilf + ILF_TYPE, XFS_LI_INODE);
    put_le16(ilf + ILF_SIZE, 2);
    put_le32(ilf + ILF_FIELDS, XFS_ILOG_CORE);
    put_le64(ilf + ILF_INO, item->ino);
    put_le64(ilf + ILF_BLKNO, item->chunk_block * (fs->blocksize / XLOG_SECTOR));
    put_le32(ilf + ILF_LEN, fs->blocksize / XLOG_SECTOR);
    put_le32(ilf + ILF_BOFFSET, item->boffset);
    rc = put_op(XFS_LOG_TID, 0, ilf, sizeof ilf);
    log_dinode_from_disk(logged, item->core);
    return rc != KERR_OK ? rc : put_op(XFS_LOG_TID, 0, logged, XFS_LOG_DINODE_SIZE);
}

int xfs_log_inodes(struct xfs *fs, const struct xfs_log_inode_item *items, uint32_t n) {
    uint8_t thdr[TRANS_HDR_SIZE];
    uint32_t sectors;
    int rc;
    if (n == 0 || n > XFS_LOG_MAX_ITEMS) {
        return KERR_INVAL;
    }
    zero(record, sizeof record);
    record_used = 0;
    zero(thdr, sizeof thdr);
    put_le32(thdr + TRANS_HDR_MAGIC, XFS_TRANS_HEADER_MAGIC);
    put_le32(thdr + TRANS_HDR_TYPE, XFS_TRANS_SETATTR_SIZE);
    put_le32(thdr + TRANS_HDR_TID, XFS_LOG_TID);
    put_le32(thdr + TRANS_HDR_NUM_ITEMS, n);
    rc = put_op(XFS_LOG_TID, XLOG_START_TRANS, NULL, 0);
    rc = rc != KERR_OK ? rc : put_op(XFS_LOG_TID, 0, thdr, sizeof thdr);
    for (uint32_t i = 0; rc == KERR_OK && i < n; i++) {
        rc = put_inode_item(fs, &items[i]);
    }
    rc = rc != KERR_OK ? rc : put_op(XFS_LOG_TID, XLOG_COMMIT_TRANS, NULL, 0);
    if (rc != KERR_OK) {
        return rc;
    }
    sectors = used_sectors();
    /* Three operations frame the transaction and two carry each item. */
    put_header(fs, 0, sectors, 3u + 2u * n);
    pack_cycles(record, sectors);
    seal(sectors);
    rc = flush_record(fs, 0, sectors);
    /* The unmount record goes immediately after this one: one basic block
     * for the header and one for each sector of operations — unless a test
     * has asked for it to be left off, which is the shape a power cut
     * leaves behind. */
    if (fs->skip_unmount_record) {
        return rc;
    }
    return rc != KERR_OK ? rc : write_unmount(fs, 1u + sectors);
}
