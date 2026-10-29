/* The mount table: which filesystem is mounted where.
 *
 * A machine has more than one filesystem on it, and until now this kernel
 * had exactly one — `xfsmount`'s single `struct xfs`, reached by every path
 * lookup regardless of what the path said. This is the table that makes a
 * path mean something: where it is mounted decides which filesystem
 * answers for it.
 *
 * **The policy is the label.** A filesystem labelled `ROOT` is the root
 * filesystem and is mounted at `/`; anything else is mounted under
 * `/volumes/<label>`. That is the operator's instruction and it is a good
 * rule for a reason worth stating: the alternative — mounting whatever the
 * firmware happened to boot from at `/` — makes the root filesystem a
 * property of the boot order rather than of the disk, so adding a second
 * drive can silently change which one is the system.
 *
 * `/volumes` itself is not a filesystem. It is a **virtual directory**
 * synthesised from this table, so that a program can list what is mounted
 * without the kernel needing a filesystem to hold the list.
 */
#ifndef KERNEL_VOLUMES_H
#define KERNEL_VOLUMES_H

#include <stdbool.h>
#include <stdint.h>

struct xfs;
struct fat;

/* Where a volume without a usable label is mounted, and the label that
 * means "this is the root filesystem". */
#define VOLUMES_ROOT_LABEL "ROOT"
#define VOLUMES_DIR "/volumes"
#define VOLUMES_BOOT_LABEL "BOOT"
#define VOLUMES_BOOT_DIR "/boot"

/* Mounts every filesystem the disk offers, each where its label says.
 *
 * Returns the number mounted, which may be zero on a machine whose disk
 * holds nothing this kernel can read — not an error, because a kernel that
 * refused to finish booting over an unreadable second partition would be
 * less useful than one that says so and carries on.
 *
 * A filesystem whose log is dirty is checked first and mounted read-only if
 * it is sound (`xfs_check`); one that fails the check is left unmounted and
 * named in the log. */
uint32_t volumes_automount(void);

/* Adds a filesystem somebody else mounted to the table, at the mount point
 * its label asks for.
 *
 * This is how the **root filesystem** gets into the table: `xfsmount` owns
 * that mount, because every caller written before this table existed goes
 * through it, so the table points at it rather than mounting the same
 * device a second time. `volumes_automount` starts from the second
 * candidate for the same reason. `index` names it when it has no label. */
void volumes_register(struct xfs *fs, uint32_t index);

/* The filesystem that owns `path`, and the remainder of the path within it.
 *
 * Longest prefix wins, so `/volumes/DATA/x` reaches the DATA volume and not
 * the root — which is the whole reason a table is ordered rather than
 * searched linearly for a first hit. NULL when nothing owns it. */
struct xfs *volumes_resolve(const char *path, const char **within);

/* The FAT volume a path lies on, and the path inside it; NULL when the path
 * is on an XFS volume or none (`volumes_resolve` answers then). */
struct fat *volumes_resolve_fat(const char *path, const char **within);

/* Flushes every FAT volume: its free count back to FSInfo, its cache out. */
int volumes_flush_fat(void);

/* The table, for listing `/volumes` and for the boot log. */
uint32_t volumes_count(void);
const char *volumes_at(uint32_t i);
const char *volumes_label(uint32_t i);
bool volumes_readonly(uint32_t i);

/* Whether a path names the virtual `/volumes` directory itself. */
bool volumes_is_the_dir(const char *path);

#endif
