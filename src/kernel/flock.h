/* flock.h — `fcntl`'s record locks: F_GETLK, F_SETLK and F_SETLKW over the
 * table in core/reclock.h, for descriptors opened for writing.
 *
 * A lock is the **process's**, as POSIX has it: closing any descriptor of the
 * file drops every lock the process holds on it (`flock_closing`), and an
 * exit drops them all (`reclock_release_owner`). */
#ifndef MINIKERN_KERNEL_FLOCK_H
#define MINIKERN_KERNEL_FLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "core/types.h"

#define F_GETLK 5u
#define F_SETLK 6u
#define F_SETLKW 7u

/* Whether `cmd` is one of the three. */
bool flock_handles(uint64_t cmd);

/* Runs one of them; `arg` is the caller's `struct flock`. KERR_NOLCK for a
 * descriptor that has no file identity (one opened read-only, a pipe),
 * KERR_AGAIN for F_SETLK in the way of another process's lock, KERR_INTR for
 * an F_SETLKW a signal ended. */
int64_t flock_fcntl(int32_t fd, uint32_t cmd, virt_t arg);

/* Descriptor `fd` is about to be closed: drop the process's locks on its
 * file. */
void flock_closing(int32_t fd);

#endif
