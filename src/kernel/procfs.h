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

/* The full path of the `index`th file, or NULL past the end: what a
 * directory listing of `/proc`, `/sys` and below is built from. */
const char *procfs_path_at(uint32_t index);

/* The longest path a per-process translation produces. */
#define PROCFS_PATH_MAX 128u

/* Per-process paths are served by the `/proc/self/...` generators, run with
 * `procfs_subject()` returning the pid (0 for the caller's own):
 *   /proc/<pid>/X                     -> /proc/self/X
 *   /proc/ps/by-pid/<pid>/X           -> /proc/self/ps/X
 *   /proc/ps/by-user/<uid>/<pid>/X    -> /proc/self/ps/X (only that uid's)
 * `uid_of` names a live pid's uid, UINT32_MAX for none; without it no
 * per-process path exists. */
void procfs_set_uid_of(uint32_t (*source)(uint32_t pid));

/* `name_of` names a live pid's program (`uproc.name`), NULL for none:
 *   /proc/ps/by-name/<name>/<pid>/X   -> /proc/self/ps/X (only that name) */
void procfs_set_name_of(const char *(*source)(uint32_t pid));
const char *procfs_name_of(uint32_t pid);
uint32_t procfs_subject(void);
uint32_t procfs_uid_of(uint32_t pid);

/* The translation above: the /proc/self path in out (PROCFS_PATH_MAX) and
 * the pid, or false when path is not a live process's. */
bool procfs_translate(const char *path, char *out, uint32_t *pid);

#endif
