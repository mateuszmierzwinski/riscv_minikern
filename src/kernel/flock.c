#include "kernel/flock.h"

#include <stddef.h>

#include "arch/riscv/timer.h"
#include "core/errno.h"
#include "core/reclock.h"
#include "kernel/fdobjects.h"
#include "kernel/sched.h"
#include "kernel/uspace.h"

/* `struct flock` on riscv64, by offset (this tree's rule for ABI structs):
 * short l_type, short l_whence, off_t l_start, off_t l_len, pid_t l_pid. */
#define FLOCK_BYTES 32u
#define FLOCK_TYPE 0u
#define FLOCK_WHENCE 2u
#define FLOCK_START 8u
#define FLOCK_LEN 16u
#define FLOCK_PID 24u

#define WHENCE_SET 0u
#define WHENCE_CUR 1u
#define WHENCE_END 2u

/* How long F_SETLKW sleeps between looks. Nothing wakes a waiter when a
 * lock is dropped, so it looks again; 10 ms is far below what a database
 * holds a lock for and far above the cost of looking. */
#define SETLKW_POLL_NS 10000000ull

struct lock_request {
    uint64_t key;
    uint32_t owner;
    uint32_t type;
    uint64_t start;
    uint64_t end;
};

static uint64_t get_le(const uint8_t *raw, uint32_t at, uint32_t bytes) {
    uint64_t v = 0;
    for (uint32_t i = bytes; i > 0; i--) {
        v = (v << 8) | raw[at + i - 1u];
    }
    return v;
}

static void put_le(uint8_t *raw, uint32_t at, uint64_t v, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i++) {
        raw[at + i] = (uint8_t)(v >> (8u * i));
    }
}

bool flock_handles(uint64_t cmd) {
    return cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW;
}

static uint32_t current_owner(void) {
    const struct uproc *p = uspace_current();
    return p == NULL ? 0u : p->pid;
}

/* Where `l_start` is measured from. */
static int base_of(int32_t fd, uint32_t whence, int64_t *base) {
    uint64_t at = 0, size = 0, ino = 0;
    uint32_t mode = 0, nlink = 0;
    int rc = KERR_OK;
    switch (whence) {
    case WHENCE_SET:
        break;
    case WHENCE_CUR:
        rc = fdobjects_seek(fd, 0, WHENCE_CUR, &at);
        break;
    case WHENCE_END:
        rc = fdobjects_stat_of(fd, &mode, &size, &nlink, &ino);
        at = size;
        break;
    default:
        return KERR_INVAL;
    }
    *base = (int64_t)at;
    return rc;
}

/* The caller's `struct flock` as a half-open range. A negative length
 * reaches backwards from the start, and zero means "to the end of the file
 * however far it grows", both as POSIX says. */
static int decode(int32_t fd, const uint8_t *raw, struct lock_request *req) {
    int64_t base = 0, start, len = (int64_t)get_le(raw, FLOCK_LEN, 8u);
    int rc = fdobjects_identity_of(fd, &req->key);
    rc = rc != KERR_OK ? KERR_NOLCK : base_of(fd, (uint32_t)get_le(raw, FLOCK_WHENCE, 2u), &base);
    if (rc != KERR_OK) {
        return rc;
    }
    start = base + (int64_t)get_le(raw, FLOCK_START, 8u);
    if (len < 0) {
        start += len;
        len = -len;
    }
    if (start < 0 || (len > 0 && start + len < start)) {
        return KERR_INVAL;
    }
    req->type = (uint32_t)get_le(raw, FLOCK_TYPE, 2u);
    req->owner = current_owner();
    req->start = (uint64_t)start;
    req->end = len == 0 ? RECLOCK_EOF : (uint64_t)(start + len);
    return KERR_OK;
}

/* F_GETLK: the lock in the way, or F_UNLCK written back when there is
 * none — the caller's own locks never count. */
static int64_t get_lock(const struct lock_request *req, uint8_t *raw, virt_t arg) {
    struct reclock_range r;
    if (req->type == RECLOCK_UNLOCK) {
        return KERR_INVAL;
    }
    if (!reclock_conflict(req->key, req->owner, req->type, req->start, req->end, &r)) {
        put_le(raw, FLOCK_TYPE, RECLOCK_UNLOCK, 2u);
    } else {
        put_le(raw, FLOCK_TYPE, r.type, 2u);
        put_le(raw, FLOCK_WHENCE, WHENCE_SET, 2u);
        put_le(raw, FLOCK_START, r.start, 8u);
        put_le(raw, FLOCK_LEN, r.end == RECLOCK_EOF ? 0u : r.end - r.start, 8u);
        put_le(raw, FLOCK_PID, r.owner, 4u);
    }
    return uspace_copy_out(arg, raw, FLOCK_BYTES) == KERR_OK ? 0 : KERR_FAULT;
}

/* F_SETLKW: looks again until the way is clear, a signal arrives, or the
 * answer is something other than "in the way". */
static int64_t set_lock_waiting(const struct lock_request *req) {
    for (;;) {
        int rc = reclock_set(req->key, req->owner, req->type, req->start, req->end);
        if (rc != KERR_AGAIN) {
            return rc;
        }
        if (sched_signal_pending()) {
            return KERR_INTR;
        }
        (void)sched_sleep_until(timer_ns() + SETLKW_POLL_NS);
    }
}

int64_t flock_fcntl(int32_t fd, uint32_t cmd, virt_t arg) {
    uint8_t raw[FLOCK_BYTES];
    struct lock_request req;
    int rc = uspace_copy_in(raw, arg, FLOCK_BYTES) == KERR_OK ? KERR_OK : KERR_FAULT;
    rc = rc != KERR_OK ? rc : decode(fd, raw, &req);
    if (rc != KERR_OK) {
        return rc;
    }
    if (cmd == F_GETLK) {
        return get_lock(&req, raw, arg);
    }
    if (cmd == F_SETLKW) {
        return set_lock_waiting(&req);
    }
    return reclock_set(req.key, req.owner, req.type, req.start, req.end);
}

void flock_closing(int32_t fd) {
    uint64_t key = 0;
    if (fdobjects_identity_of(fd, &key) == KERR_OK) {
        reclock_release(key, current_owner());
    }
}
