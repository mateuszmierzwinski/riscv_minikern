/* synthfs — `/proc`, `/sys` and `/dev` as filesystems.
 *
 * The files themselves are `procfs`'s generators and the descriptor layer's
 * device objects; this gives them a tree, so `stat` and a directory listing
 * answer as they do on Linux instead of `ENOENT`. A path is a file when it
 * is registered, and a directory when a registered path lies below it.
 * Nothing here can be written, created or removed (`EPERM`).
 */
#ifndef MINIKERN_KERNEL_SYNTHFS_H
#define MINIKERN_KERNEL_SYNTHFS_H

#include <stdint.h>

/* Registers a device node's full path ("/dev/null"), so it stats as a
 * character device and lists in its directory. */
int synthfs_add_device(const char *path);

/* Where the live pids come from (`uspace_pid_at`): 0 for a free slot,
 * UINT32_MAX past the last. Without it /proc lists no processes. */
void synthfs_set_pids(uint32_t (*source)(uint32_t index));

/* Mounts the tree at /proc, /sys and /dev. */
int synthfs_mount(void);

#endif
