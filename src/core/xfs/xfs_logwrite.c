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
 * **The log is a ring, and it is appended to as Linux appends to it.** Each
 * batch — the transaction record, then an unmount record — goes at the head
 * the mount found (xfs_log.c), in the head's cycle, padded to the log's
 * stripe unit, and a record that runs past the end continues at the start
 * with its blocks stamped one cycle on. This was once simpler: every batch
 * was written from block 0, in cycle 1, with zeroes after it. That held on a
 * log `mkfs` had just made, and nowhere else — on a log Linux had wrapped it
 * left a cycle-1 record ahead of newer ones, and `xfs_repair` called the log
 * dirty; on a 4 KiB-sector drive it left the head where the drive cannot
 * write.
 *
 * The record format is the one the formatter already writes
 * (userland/internal/xfsmk/write_log.go) and the reader already parses: a
 * 512-byte header, then the operations, with every data sector's first word
 * replaced by the cycle number and the displaced word kept in the header.
 */


#define XLOG_FMT_LINUX_LE 1u
#define XLOG_ICLOG_SIZE   32768u

#define XLOG_UNMOUNT_TID  0xb0c0d0d0u /* what the formatter writes, and what xfs_logprint expects */
#define XFS_LOG_TID       0x6d6b7472u /* "mktr": ours, and a log of ours is the only one we write */
/* The transaction header, which is an **operation of its own**: the start
 * operation carries no data at all, and recovery checks this magic on the
 * first operation that does. Getting that wrong — putting the header on the
 * start operation — is what `xfs_logprint` was reporting as "bad header
 * magic number", and reading our own bytes back out of the image is what
 * settled where it belongs. Its three words are native-endian, like the
 * unmount record's magic, because recovery reads them as plain `uint`s. */
#define XFS_TRANS_SETATTR_SIZE 2u          /* what a size change is */


/* The inode item's format header, as the log carries it (56 bytes in v5). */

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
/* **The table runs past a v4 inode's core**, so `core` bounds it.
 *
 * `CORE_FIELDS` describes the v3 core: its last seven entries, from offset
 * 104 to 152, and the UUID at 160, are fields v5 added. On a v2 inode the
 * core ends at 100 and every one of those offsets is inside the *literal
 * area* — the file's own fork. Reading them into the log is merely wrong;
 * writing them back on replay would overwrite the fork of each inode being
 * recovered, which is the same shape as the bug that made the first version
 * of replay corrupt every inode it touched while reporting success. */
static void copy_core_fields(uint8_t *to, const uint8_t *from, uint32_t core) {
    for (uint32_t f = 0; f < sizeof CORE_FIELDS / sizeof CORE_FIELDS[0]; f++) {
        uint32_t off = CORE_FIELDS[f].off, len = CORE_FIELDS[f].len;
        if (off + len > core) {
            continue; /* a field this inode's core does not have */
        }
        for (uint32_t i = 0; i < len; i++) {
            to[off + i] = from[off + len - 1u - i];
        }
    }
    if (XFS_DI_UUID + 16u <= core) {
        copy(to + XFS_DI_UUID, from + XFS_DI_UUID, 16);
    }
}

static void log_dinode_from_disk(uint8_t *out, const uint8_t *di, uint32_t core) {
    zero(out, XFS_LOG_DINODE_SIZE);
    copy_core_fields(out, di, core);
}

/* The inverse, for replay: a logged inode core back into an on-disk one.
 *
 * **A logged inode is not an on-disk inode.** Each field keeps its offset
 * but is written in the host's byte order rather than big-endian, because
 * the log is replayed by the machine that wrote it. Copying the logged
 * bytes straight into the inode therefore writes a little-endian core over
 * a big-endian one — which is what the first version of replay did, and it
 * corrupted every inode it touched while reporting success. The board said
 * so by failing to read a file that had been written *before* the crash
 * being recovered from.
 *
 * Field by field, and **only** the fields in the table: a wholesale
 * 176-byte copy would also zero `di_next_unlinked`, which the log does not
 * carry and the unlinked list needs. Everything outside the table is left
 * as the inode already has it, and the caller reseals. */
void xfs_log_dinode_to_disk(uint8_t *di, const uint8_t *logged, uint32_t core) {
    copy_core_fields(di, logged, core);
}

/* A record under construction: the header block, then as many data blocks
 * as one header can describe. Static because it is larger than a kernel
 * stack may carry (CLAUDE.md §10) and safe for the same reason every static
 * buffer here is: one core is inside the kernel at a time, and nothing in a
 * record's construction sleeps. */
static uint8_t record[XLOG_REC_HDR_SIZE + XLOG_DATA_MAX_BB * XLOG_SECTOR];
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

static uint64_t log_blocks(const struct xfs *fs) {
    return (uint64_t)fs->logblocks * (fs->blocksize / XLOG_SECTOR);
}

/* The data blocks a record of the built operations takes: enough for them,
 * and then enough more that header and data together are a whole number of
 * the log's roundoff — the padding Linux adds, and counts in `h_len`, so
 * that every record starts where a 4 KiB-sector drive can write. */
static uint32_t data_blocks(const struct xfs *fs) {
    uint32_t need = (record_used + XLOG_SECTOR - 1u) / XLOG_SECTOR;
    uint32_t unit = fs->log_roundoff == 0 ? 1u : fs->log_roundoff;
    return ((1u + need + unit - 1u) / unit) * unit - 1u;
}

/* Replaces each data block's first word with its cycle, keeping the word it
 * displaced in the header. The cycle is the record's, **plus one** for a
 * block that wraps past the end of the log to its start: that is the cycle
 * those blocks belong to, and what Linux stamps there (`xlog_sync`). */
static void pack_cycles(const struct xfs *fs, uint32_t sectors) {
    uint64_t blocks = log_blocks(fs);
    for (uint32_t i = 0; i < sectors; i++) {
        uint8_t *sector = record + XLOG_REC_HDR_SIZE + i * XLOG_SECTOR;
        bool wrapped = fs->log_head + 1u + i >= blocks;
        copy(record + XLOG_REC_CYCLE_DATA + i * 4u, sector, 4);
        put_be32(sector, fs->log_cycle + (wrapped ? 1u : 0u));
    }
}

/* The record header, at the head in the head's cycle. Its tail is itself:
 * everything before it has reached its place, which is what this writer
 * guarantees by writing metadata in place as it logs it. */
static void put_header(const struct xfs *fs, uint32_t sectors, uint32_t nops) {
    uint64_t lsn = ((uint64_t)fs->log_cycle << 32) | fs->log_head;
    put_be32(record + XLOG_REC_MAGIC, XLOG_MAGIC);
    put_be32(record + XLOG_REC_CYCLE, fs->log_cycle);
    put_be32(record + XLOG_REC_VERSION, 2);
    put_be32(record + XLOG_REC_LEN, sectors * XLOG_SECTOR);
    put_be64(record + XLOG_REC_LSN, lsn);
    put_be64(record + XLOG_REC_TAIL_LSN, lsn);
    put_be32(record + XLOG_REC_PREV_BLOCK, (uint32_t)fs->log_last);
    put_be32(record + XLOG_REC_NUM_OPS, nops);
    put_be32(record + XLOG_REC_FMT, XLOG_FMT_LINUX_LE);
    copy(record + XLOG_REC_FS_UUID, fs->uuid, 16);
    put_be32(record + XLOG_REC_SIZE, XLOG_ICLOG_SIZE);
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

/* Writes the built record at the head — in two pieces when it runs past
 * the end of the log — and moves the head past it.
 *
 * **The log is addressed in 512-byte basic blocks**, not in filesystem
 * blocks, and a record's `h_lsn` names one of those. Writing at a
 * filesystem block while the header claimed a basic block put the unmount
 * record eight blocks from where it said it was, and `xfs_repair` declared
 * the log to hold changes that had to be replayed. */
static int flush_record(struct xfs *fs, uint32_t sectors) {
    uint64_t blocks = log_blocks(fs), total = 1u + sectors;
    uint64_t first = blocks - fs->log_head < total ? blocks - fs->log_head : total;
    int rc = bcache_pwrite(fs->log_cache, fs->log_base + fs->log_head * XLOG_SECTOR, record,
                           (uint32_t)(first * XLOG_SECTOR));
    if (rc == KERR_OK && first < total) {
        rc = bcache_pwrite(fs->log_cache, fs->log_base, record + first * XLOG_SECTOR,
                           (uint32_t)((total - first) * XLOG_SECTOR));
    }
    if (rc != KERR_OK) {
        return rc;
    }
    fs->log_last = fs->log_head;
    fs->log_head += total;
    if (fs->log_head >= blocks) {
        fs->log_head -= blocks;
        fs->log_cycle++;
    }
    return KERR_OK;
}

/* Turns the operations built so far into a record at the head. */
static int emit(struct xfs *fs, uint32_t nops) {
    uint32_t sectors = data_blocks(fs);
    if (sectors > XLOG_DATA_MAX_BB || 1u + sectors > log_blocks(fs)) {
        return KERR_NOTSUP; /* a roundoff past one header's reach: a striped log */
    }
    put_header(fs, sectors, nops);
    pack_cycles(fs, sectors);
    seal(sectors);
    return flush_record(fs, sectors);
}

/* Starts a record: nothing in the buffer, padding included. */
static void begin_record(void) {
    zero(record, sizeof record);
    record_used = 0;
}

/* The unmount record, which says the filesystem was put down properly. It
 * is the same shape the formatter writes, and writing it after every batch
 * is what keeps the log clean — and so what keeps our own reader, which
 * refuses a dirty one, able to mount what it just wrote. */
static int write_unmount(struct xfs *fs) {
    uint8_t magic[8];
    int rc;
    begin_record();
    zero(magic, sizeof magic);
    put_be16(magic, XLOG_UNMOUNT_MAGIC);
    rc = put_op(XLOG_UNMOUNT_TID, XLOG_UNMOUNT_TRANS, magic, sizeof magic);
    return rc != KERR_OK ? rc : emit(fs, 1);
}

/* Puts an unmount record at the head, which is what makes the log clean.
 *
 * Exposed for replay: once it has applied everything the log held, the log
 * must stop saying so — otherwise the next mount replays the same
 * transactions again. */
int xfs_log_mark_clean(struct xfs *fs) {
    return write_unmount(fs);
}

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
    log_dinode_from_disk(logged, item->core, fs->dinode_size);
    return rc != KERR_OK ? rc : put_op(XFS_LOG_TID, 0, logged, XFS_LOG_DINODE_SIZE);
}

int xfs_log_inodes(struct xfs *fs, const struct xfs_log_inode_item *items, uint32_t n) {
    uint8_t thdr[TRANS_HDR_SIZE];
    int rc;
    if (n == 0 || n > XFS_LOG_MAX_ITEMS) {
        return KERR_INVAL;
    }
    begin_record();
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
    if (!fs->skip_commit_record) {
        rc = rc != KERR_OK ? rc : put_op(XFS_LOG_TID, XLOG_COMMIT_TRANS, NULL, 0);
    }
    /* Three operations frame the transaction and two carry each item. */
    rc = rc != KERR_OK ? rc : emit(fs, (fs->skip_commit_record ? 2u : 3u) + 2u * n);
    fs->skip_commit_record = false;
    /* The unmount record follows it — unless a test has asked for it to be
     * left off, which is the shape a power cut leaves behind: the newest
     * record is then this transaction, and the next mount replays it. */
    return rc != KERR_OK || fs->skip_unmount_record ? rc : write_unmount(fs);
}
