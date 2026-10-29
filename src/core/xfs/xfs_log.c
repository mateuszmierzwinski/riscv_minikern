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

/* One basic block of the log, into buf (XLOG_SECTOR bytes).
 *
 * **`logstart` is an AG-encoded block number, not a linear one.** It packs
 * the allocation group into its high bits — the same encoding as an inode
 * number — so it has to go through `xfs_fsb_to_block` like every other
 * fsblock. This read path multiplied the raw value instead, while the log
 * *write* path in the same filesystem already converted it: the two halves
 * disagreed about where the log was.
 *
 * It is invisible on any filesystem whose log sits in AG 0, because there
 * the encoding is the identity, and it does not fail loudly when it is
 * wrong — it reads some other part of the card. On a 7.6 GiB card whose log
 * landed in AG 2 it read 4 GiB too far, into never-written flash, and
 * 0xffffffff came back as a cycle number. The card before it had its log
 * read at a wrong offset too and mounted anyway, because the bytes there
 * were zero and zero means "this log was never written". */
static int read_bb(struct xfs *fs, uint64_t bb, uint8_t *buf) {
    if (fs->log_cache == NULL) {
        return KERR_NODEV; /* an external log whose device is not attached */
    }
    return bcache_pread(fs->log_cache, fs->log_base + bb * XLOG_SECTOR, buf, XLOG_SECTOR);
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

/* The newest record in a log whose every block carries the same cycle.
 *
 * The bisection above cannot find a head here, and the reason is worth
 * stating because it looks like a contradiction: it searches for the first
 * block that is **not** in the current cycle, and `mkfs` stamps the *whole*
 * log with cycle 1 — so the honest answer is "the end of the log", where
 * there is no record within any sensible lookback. A freshly made card that
 * has never been mounted hides this, because its first block reads cycle 0
 * and the check takes the "never written" exit; the first card formatted for
 * this project did exactly that and mounted, and the first card with files
 * on it did not.
 *
 * The records are contiguous from block 0 in this case, and each header
 * carries its own length, so the newest is reached by stepping from record
 * to record: as many steps as there are records, not as many as the log has
 * blocks. */
static int walk_to_newest(struct xfs *fs, uint64_t blocks, uint32_t cycle, uint64_t *at) {
    uint64_t bb = 0;
    bool any = false;
    while (bb < blocks) {
        int rc = read_bb(fs, bb, scratch);
        if (rc != KERR_OK) {
            return rc;
        }
        if (!is_record_header(scratch, cycle)) {
            break;
        }
        *at = bb;
        any = true;
        bb += xfs_log_record_blocks(scratch);
    }
    return any ? KERR_OK : KERR_NOENT;
}

/* The newest record, by whichever search the log's shape allows.
 *
 * A log that has wrapped partway has a cycle boundary to bisect for; one
 * stamped uniformly does not, and the two are told apart by the only
 * evidence available — whether the last block belongs to the same cycle as
 * the first. */
static int find_newest(struct xfs *fs, uint64_t blocks, uint32_t cycle, uint64_t *newest) {
    uint64_t head = 0;
    int rc = read_bb(fs, blocks - 1u, scratch);
    if (rc != KERR_OK) {
        return rc;
    }
    if (bb_cycle(scratch) == cycle) {
        return walk_to_newest(fs, blocks, cycle, newest);
    }
    rc = find_head(fs, blocks, cycle, &head);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = find_newest_record(fs, head, cycle, newest);
    if (rc == KERR_NOENT) {
        kprintf("xfs: the log's head at %u has no record before it\n", (uint32_t)head);
    }
    return rc;
}

uint64_t xfs_log_record_blocks(const uint8_t *hdr) {
    uint32_t size = xfs_be32(hdr + XLOG_REC_SIZE);
    uint32_t len = xfs_be32(hdr + XLOG_REC_LEN);
    uint32_t hblks = xfs_be32(hdr + XLOG_REC_VERSION) == 2u && size > XLOG_HEADER_CYCLE_SIZE
                         ? (size + XLOG_HEADER_CYCLE_SIZE - 1u) / XLOG_HEADER_CYCLE_SIZE
                         : 1u;
    return hblks + (len + XLOG_SECTOR - 1u) / XLOG_SECTOR;
}

/* Where the log goes on after its newest record, in a log of `blocks`: the
 * block after it, in the cycle after it when it reached the end; and the
 * tail it names, which is where recovery would begin. */
static int take_position(struct xfs *fs, uint64_t newest, uint64_t blocks, uint32_t cycle) {
    uint64_t next;
    int rc = read_bb(fs, newest, scratch);
    if (rc != KERR_OK) {
        return rc;
    }
    next = newest + xfs_log_record_blocks(scratch);
    fs->log_last = newest;
    fs->log_tail = xfs_be64(scratch + XLOG_REC_TAIL_LSN) & 0xffffffffu;
    fs->log_cycle = next >= blocks ? cycle + 1u : cycle;
    fs->log_head = next >= blocks ? next - blocks : next;
    return fs->log_tail < blocks ? KERR_OK : KERR_IO;
}

/* A log nothing has written: the first record goes at block 0, cycle 1. */
static void fresh_position(struct xfs *fs) {
    fs->log_head = 0;
    fs->log_tail = 0;
    fs->log_last = 0;
    fs->log_cycle = 1;
}

int xfs_log_check(struct xfs *fs) {
    uint64_t blocks = (uint64_t)fs->logblocks * (fs->blocksize / XLOG_SECTOR);
    uint64_t newest = 0;
    uint32_t cycle;
    bool unmount = false;
    int rc;
    if (fs->logblocks == 0) {
        kprintf("xfs: the superblock describes no log at all\n");
        return KERR_INVAL;
    }
    /* **An external log is a configuration, not a defect.** Its device is
     * not named anywhere in the superblock, so until somebody attaches one
     * the log cannot be read — and a log that cannot be read is exactly the
     * situation a dirty log puts the filesystem in: readable, and not to be
     * written. Refusing the mount instead would make a deliberate choice
     * look like a corrupt disk. */
    /* `KERR_NODEV`, not `KERR_DIRTY`: those are different situations and
     * the caller acts differently on them. A dirty log has work in it that
     * can be checked and replayed; a log on an unattached device has
     * nothing that *can* be read, so running the checker would produce a
     * verdict nothing could act on and replaying is not even a question. */
    if (fs->log_cache == NULL) {
        kprintf("xfs: the log is on a separate device, which is not attached\n");
        return KERR_NODEV;
    }
    rc = read_bb(fs, 0, scratch);
    if (rc != KERR_OK) {
        return rc;
    }
    cycle = bb_cycle(scratch);
    if (cycle == 0) {
        fresh_position(fs);
        return KERR_OK; /* never written: there is nothing to replay */
    }
    rc = find_newest(fs, blocks, cycle, &newest);
    if (rc == KERR_NOENT) {
        kprintf("xfs: the log carries cycle %u but no record this reader can find\n", cycle);
        return KERR_NOTSUP;
    }
    rc = rc != KERR_OK ? rc : take_position(fs, newest, blocks, cycle);
    if (rc == KERR_OK) {
        rc = record_is_unmount(fs, newest, &unmount);
    }
    if (rc != KERR_OK) {
        return rc;
    }
    if (!unmount) {
        /* **Reported, not refused.** The caller decides what a dirty log
         * means: it checks the filesystem and mounts it read-only if the
         * check is sound, because metadata that is behind the log is a
         * reason not to write and not a reason to be unreadable. */
        kprintf("xfs: the log is dirty (newest record at %u is not an unmount)\n",
                (uint32_t)newest);
        return KERR_DIRTY;
    }
    return KERR_OK;
}
