/* pathres — resolving `.`, `..` and symbolic links against the tree.
 *
 * `..` is the classic way out of a sandbox when it is collapsed as text:
 * `/jail/link/..` is not `/jail` when `link` points elsewhere. So the walk
 * goes a component at a time, and **every component before the last is
 * asked whether it is a link**; a link's target replaces it (relative to
 * the directory holding it, or from the root when absolute) before the
 * walk goes on. A `..` then only ever pops a component already proven to
 * be a real directory, which is what makes popping it as text safe.
 *
 * The last component is not followed: whether a call acts on a link or on
 * what it points to is that call's business (`lstat`, `unlink` and
 * `readlink` act on the link). Pure: links are read through a port.
 */
#ifndef MINIKERN_CORE_PATHRES_H
#define MINIKERN_CORE_PATHRES_H

#include <stdint.h>

/* Linux's limit on links followed in one resolution. */
#define PATHRES_MAX_LINKS 40u

/* The longest path the walk holds, and the scratch a caller lends it: the
 * queue, a link's target, and room to rebuild the queue — 1.5 KiB that a
 * kernel stack cannot spare, and that must not be shared between two
 * callers, since reading a link can sleep. */
#define PATHRES_WORK 512u
#define PATHRES_SCRATCH (3u * PATHRES_WORK)

/* Reads the link at an absolute path into buf (not NUL-terminated):
 * KERR_OK with *got when it is a link; anything else means it is not one
 * the walk can follow, and the walk goes on through it. */
typedef int (*pathres_readlink_fn)(void *self, const char *path, char *buf, uint32_t max,
                                   uint32_t *got);

/* Resolves the absolute path `in` into `out` (`room` bytes): no `.`, no
 * `..`, no empty components, no links before the last component.
 * KERR_OK, KERR_INVAL for a relative path, KERR_NAMETOOLONG, KERR_LOOP
 * after PATHRES_MAX_LINKS links. `scratch` is PATHRES_SCRATCH bytes, the caller's. */
int path_resolve(const char *in, char *out, uint32_t room, pathres_readlink_fn readlink,
                 void *self, char *scratch);

#endif
