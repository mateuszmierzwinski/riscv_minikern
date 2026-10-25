/* `/proc`: files the kernel makes up when they are read.
 *
 * **A table of paths and generators, not a special case per file.** The
 * kernel log already had a synthesised path — `/dev/virtual/kernel_logger` —
 * wired as a branch in the open path, and the roadmap wants several more
 * (`/proc/ps/by-pid/...` and the rest). A second branch would have been
 * tolerable and a fifth would not, so this is the mechanism those go in.
 *
 * Each file is generated **whole, at open**, into a snapshot the descriptor
 * then reads. That is deliberate: a `/proc` file read in two `read` calls
 * must not change between them, or a program parsing it sees a line from
 * before and a line from after. It is the same reasoning the kernel log's
 * snapshot already used, generalised.
 *
 * The cost is that a file has to fit in the snapshot, which bounds what
 * belongs here: counters and a few lines of state, not a data stream.
 */
#ifndef KERNEL_PROCFS_H
#define KERNEL_PROCFS_H

#include <stdbool.h>

#include "core/types.h"

/* How much one `/proc` file may produce. Four kilobytes is a page and far
 * more than any of these need; a generator that would exceed it is
 * truncated and says so, rather than silently losing its tail. */
#define PROCFS_MAX_BYTES 4096u

/* A generator writes text and returns how many bytes it wrote. It is given
 * the room it has and must not exceed it. */
typedef uint32_t (*procfs_fn)(char *out, uint32_t room);

/* Registers a path. Called from the composition root, before anything can
 * open one — a port installed late is a port that was not installed, which
 * this tree has paid for twice. */
int procfs_add(const char *path, procfs_fn fn);

/* Fills `out` with the file at `path`, or returns KERR_NOENT. */
int procfs_generate(const char *path, char *out, uint32_t room, uint32_t *wrote);

/* Whether `path` is one of these, so the open path can route it without
 * generating anything. */
bool procfs_has(const char *path);

/* Lists the registered names under `/proc`, so that a directory read of it
 * answers rather than being empty. `index` walks from zero; NULL ends it. */
const char *procfs_name_at(uint32_t index);

#endif
