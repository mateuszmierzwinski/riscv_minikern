/* reclock.h — POSIX record locks: the table `fcntl(F_SETLK)` keeps.
 *
 * **What SQLite needs before it will open a database.** Its unix VFS takes
 * a read lock on a range of the file before every read transaction and a
 * write lock before every write, and an `fcntl` that answers EINVAL is a
 * "disk I/O error" on the first statement. Advisory, like Linux's: nothing
 * stops a program reading a range another holds — the table only answers
 * the programs that ask.
 *
 * A lock belongs to an **owner** (the process, as POSIX says) and a **key**
 * (the file). Ranges are half-open, `[start, end)`, with `RECLOCK_EOF` as
 * the end of a lock that runs to the end of the file however long it grows.
 * One owner's locks on one key never overlap: setting a range first cuts
 * the owner's own locks out of it, which is how an upgrade, a downgrade
 * and an unlock of the middle of a lock all work.
 *
 * Pure and host-tested; the caller holds the kernel lock.
 */
#ifndef MINIKERN_CORE_RECLOCK_H
#define MINIKERN_CORE_RECLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define RECLOCK_MAX 64u
#define RECLOCK_EOF UINT64_MAX

/* Linux's `l_type` values, so the syscall passes them straight through. */
#define RECLOCK_READ 0u
#define RECLOCK_WRITE 1u
#define RECLOCK_UNLOCK 2u

struct reclock_range {
    uint64_t start;
    uint64_t end; /* exclusive; RECLOCK_EOF for "to the end" */
    uint32_t type;
    uint32_t owner;
};

/* Forgets every lock (tests, and a kernel starting over). */
void reclock_reset(void);

/* The identity a path's locks are kept under: FNV-1a over the bytes, never
 * zero. Also what `stat` reports as the inode, because SQLite compares the
 * two to decide whether its database file was moved under it. */
uint64_t reclock_identity(const char *path);

/* Whether another owner holds a lock on `key` that `type` over
 * `[start, end)` would conflict with — any overlap where either side is a
 * write lock. The first such lock goes to `out` when it is not NULL. */
bool reclock_conflict(uint64_t key, uint32_t owner, uint32_t type, uint64_t start, uint64_t end,
                      struct reclock_range *out);

/* Sets `owner`'s lock of `type` over `[start, end)`, or with
 * RECLOCK_UNLOCK clears it. KERR_AGAIN when another owner's lock is in the
 * way, KERR_NOLCK when the table has no room, KERR_INVAL for an empty range
 * or an unknown type; nothing changes unless the answer is KERR_OK. */
int reclock_set(uint64_t key, uint32_t owner, uint32_t type, uint64_t start, uint64_t end);

/* Drops every lock `owner` holds on `key`: what closing any descriptor of
 * the file does, by POSIX's (much criticised, still the rule) definition. */
void reclock_release(uint64_t key, uint32_t owner);

/* Drops every lock `owner` holds, on anything: the process has exited. */
void reclock_release_owner(uint32_t owner);

/* How many locks are held, for tests and the report. */
uint32_t reclock_count(void);

#endif
