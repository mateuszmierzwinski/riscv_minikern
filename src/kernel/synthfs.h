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

/* Who a device node belongs to and the permission bits it has: what
 * `stat` reports for it and what `open` checks against. */
struct device_owner {
    uint32_t mode; /* the 0777 bits */
    uint32_t uid;
    uint32_t gid;
};

/* Registers a device node's full path ("/dev/null"), so it stats as a
 * character device and lists in its directory. `owner` NULL is root's and
 * open to everyone (0666), which is what the memory devices and the
 * console are. */
int synthfs_add_device(const char *path, const struct device_owner *owner);

/* Where the live pids come from (`uspace_pid_at`): 0 for a free slot,
 * UINT32_MAX past the last. Without it /proc lists no processes. */
void synthfs_set_pids(uint32_t (*source)(uint32_t index));

/* Mounts the tree at /proc, /sys and /dev. */
int synthfs_mount(void);

#endif
