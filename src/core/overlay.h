/* overlay — the pure half of the overlay filesystem: its mount options, the
 * names it keeps, and how a path inside it maps onto its two layers.
 *
 * An overlay shows two directories as one. Reads find a name in the UPPER
 * layer first and the LOWER layer otherwise; every change lands in the
 * upper one, copying a lower file up first; removing a name the lower layer
 * has leaves a **whiteout** in the upper one, so the lower file stays where
 * it is and is no longer seen. A whiteout here is an empty file named
 * `.wh.<name>` (aufs's spelling): Linux's own is a 0:0 character device,
 * which this kernel's filesystems cannot make.
 *
 * Nothing here touches a filesystem; `kernel/overlayfs.c` does.
 */
#ifndef MINIKERN_CORE_OVERLAY_H
#define MINIKERN_CORE_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>

#define OVERLAY_PATH_MAX 256u
#define OVERLAY_WHITEOUT_PREFIX ".wh."

/* Reads Linux's option string, "lowerdir=<abs>,upperdir=<abs>[,workdir=…]".
 * Both directories are required and absolute; workdir is accepted and not
 * used (this overlay copies up in place). KERR_INVAL for anything else, a
 * repeated or unknown option included. */
int overlay_parse_options(const char *data, char lower[OVERLAY_PATH_MAX],
                          char upper[OVERLAY_PATH_MAX]);

/* `base` joined with a path inside the overlay ("/" is `base` itself). */
int overlay_join(const char *base, const char *in, char out[OVERLAY_PATH_MAX]);

/* The whiteout that hides `in` ("/a/b" -> "/a/.wh.b"); KERR_INVAL for "/". */
int overlay_whiteout_of(const char *in, char out[OVERLAY_PATH_MAX]);

/* Whether a directory entry is a whiteout, which a listing never shows. */
bool overlay_is_whiteout(const char *name, uint32_t len);

/* Whether `inner` is `outer` or below it ("/a/b" is under "/a", "/ab" is
 * not). A mount whose layer is under its own mount point would answer
 * every lookup by looking itself up. */
bool overlay_path_under(const char *inner, const char *outer);

#endif
