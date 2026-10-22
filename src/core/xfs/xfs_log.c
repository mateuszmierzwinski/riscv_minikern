#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs_internal.h"

/* The clean-log check.
 *
 * A log is clean when the newest thing in it is an unmount record: nothing
 * happened after the filesystem was last put down properly. Anything else
 * means the metadata on disk is older than the log, and reading it would be
 * reading a filesystem that is not there — so the mount is refused, and
 * Linux's xfs_repair is what fixes it until recovery is implemented (P9.4).
 *
 * Finding the newest record cheaply matters: the log of a 512 MiB
 * filesystem is 64 MiB, and over a network block device a linear scan would
 * be thousands of round trips. The log is a circular buffer written in
 * 512-byte basic blocks, and **the first word of every basic block carries
 * the cycle number** (a record header's own first word is its magic, with
 * the cycle just after it). So the blocks of the current cycle form a prefix
 * of the log, the head is the first block that is not in it, and that
 * boundary can be found by bisection in about seventeen reads.
 *
 * From the head, the newest record header is a few blocks back — an unmount
 * record is two basic blocks long — so a short backward scan finds it.
 */

/* How far back from the head to look for the newest record header. An
 * unmount record is one header block plus one data block; a generous
 * window still costs nothing. */
#define LOG_LOOKBACK_BB 64u

/* One basic block, shared by every step of the check.
 *
 * It is static because a 512-byte buffer per frame put this function's
 * stack over the kernel's 1 KiB limit, which the toolchain enforces after a
 * 16 KiB local wedged the network stack (CLAUDE.md §10). The consequence is
 * that checking a log is not reentrant — which is true of mounting a
 * filesystem anyway, and the kernel is not preemptible. */
static uint8_t scratch[XLOG_SECTOR];

/* One basic block of the log, into buf (XLOG_SECTOR bytes). */
static int read_bb(struct xfs *fs, uint64_t bb, uint8_t *buf) {
    uint64_t off = fs->logstart * fs->blocksize + bb * XLOG_SECTOR;
    return bcache_pread(fs->cache, off, buf, XLOG_SECTOR);
}

/* The cycle a basic block belongs to, or 0 when it has never been written.
 * A record header keeps its magic in the first word and the cycle in the
 * second; every other block has the cycle in the first. */
static uint32_t bb_cycle(const uint8_t *bb) {
    uint32_t first = xfs_be32(bb + XLOG_REC_MAGIC);
    return first == XLOG_MAGIC ? xfs_be32(bb + XLOG_REC_CYCLE) : first;
}

/* The first basic block that is not part of cycle, by bisection over the
 * prefix that is. The log's blocks of one cycle are contiguous from zero,
 * so the predicate is monotone and this is well defined. */
static int find_head(struct xfs *fs, uint64_t blocks, uint32_t cycle, uint64_t *head) {
    uint64_t low = 0, high = blocks; /* [low, high): low is in the cycle, high is not */
    while (high - low > 1u) {
        uint64_t mid = low + (high - low) / 2u;
        int rc = read_bb(fs, mid, scratch);
        if (rc != KERR_OK) {
            return rc;
        }
        if (bb_cycle(scratch) == cycle) {
            low = mid;
        } else {
            high = mid;
        }
    }
    *head = high;
    return KERR_OK;
}

/* True when the record header at bb is one, and belongs to cycle. */
static bool is_record_header(const uint8_t *bb, uint32_t cycle) {
    return xfs_be32(bb + XLOG_REC_MAGIC) == XLOG_MAGIC && xfs_be32(bb + XLOG_REC_CYCLE) == cycle &&
           xfs_be32(bb + XLOG_REC_LEN) >= XLOG_OP_HDR_SIZE;
}

/* True when the operation that follows a record header is the unmount
 * transaction.
 *
 * The operation header is big-endian, so its client and flags are read
 * plainly; the payload's magic is written in the **host's** byte order by
 * whoever wrote the log, so both orders are accepted. The flag is the real
 * evidence and the magic is the cross-check. */
static bool is_unmount_op(const uint8_t *op, uint32_t len) {
    uint16_t magic;
    if (len < XLOG_OP_HDR_SIZE + 2u) {
        return false;
    }
    if (op[XLOG_OP_CLIENTID] != XLOG_CLIENT_LOG || (op[XLOG_OP_FLAGS] & XLOG_UNMOUNT_TRANS) == 0) {
        return false;
    }
    magic = xfs_be16(op + XLOG_OP_HDR_SIZE);
    return magic == XLOG_UNMOUNT_MAGIC || magic == (uint16_t)((XLOG_UNMOUNT_MAGIC >> 8) |
                                                             (XLOG_UNMOUNT_MAGIC << 8));
}

/* Reads the record whose header is at bb and says whether it is an unmount
 * record. Its single operation follows the 512-byte header. */
static int record_is_unmount(struct xfs *fs, uint64_t bb, bool *unmount) {
    int rc = read_bb(fs, bb, scratch);
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfs_be32(scratch + XLOG_REC_NUM_OPS) != 1u) {
        *unmount = false; /* an unmount record carries exactly one operation */
        return KERR_OK;
    }
    rc = read_bb(fs, bb + 1u, scratch); /* the header is read out: reuse it */
    if (rc != KERR_OK) {
        return rc;
    }
    *unmount = is_unmount_op(scratch, XLOG_SECTOR);
    return KERR_OK;
}

/* Walks back from the head to the newest record header. */
static int find_newest_record(struct xfs *fs, uint64_t head, uint32_t cycle, uint64_t *at) {
    for (uint64_t back = 1u; back <= LOG_LOOKBACK_BB && back <= head; back++) {
        uint64_t candidate = head - back;
        int rc = read_bb(fs, candidate, scratch);
        if (rc != KERR_OK) {
            return rc;
        }
        if (is_record_header(scratch, cycle)) {
            *at = candidate;
            return KERR_OK;
        }
    }
    return KERR_NOENT;
}

int xfs_log_check(struct xfs *fs) {
    uint64_t blocks = (uint64_t)fs->logblocks * (fs->blocksize / XLOG_SECTOR);
    uint64_t head = 0, newest = 0;
    uint32_t cycle;
    bool unmount = false;
    int rc;
    if (fs->logstart == 0 || fs->logblocks == 0) {
        kprintf("xfs: an external log is not supported\n");
        return KERR_NOTSUP;
    }
    rc = read_bb(fs, 0, scratch);
    if (rc != KERR_OK) {
        return rc;
    }
    cycle = bb_cycle(scratch);
    if (cycle == 0) {
        return KERR_OK; /* never written: there is nothing to replay */
    }
    rc = find_head(fs, blocks, cycle, &head);
    if (rc == KERR_OK) {
        rc = find_newest_record(fs, head, cycle, &newest);
    }
    if (rc == KERR_NOENT) {
        kprintf("xfs: the log's head at %u has no record before it\n", (uint32_t)head);
        return KERR_NOTSUP;
    }
    if (rc == KERR_OK) {
        rc = record_is_unmount(fs, newest, &unmount);
    }
    if (rc != KERR_OK) {
        return rc;
    }
    if (!unmount) {
        kprintf("xfs: the log is dirty (newest record at %u is not an unmount): refusing to mount\n",
                (uint32_t)newest);
        return KERR_NOTSUP;
    }
    return KERR_OK;
}
