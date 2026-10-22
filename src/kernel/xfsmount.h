/* xfsmount — mounting the filesystem that is on the disk.
 *
 * The XFS implementation came across whole and host-tested against
 * filesystems `mkfs.xfs` made. What it has never had on this architecture is
 * a **real device** beneath it, and this is what connects the two: the
 * buffer cache over whichever storage adapter the board turned out to have
 * — NVMe, an SD card, or the emulator's virtio disk — and XFS over that.
 *
 * It mounts **read-only by default**, and that is not timidity. A
 * filesystem's log tells you whether it was unmounted cleanly, and this
 * implementation refuses a dirty one rather than replaying it: replaying
 * another implementation's log correctly is a larger piece of work than
 * writing one, and getting it subtly wrong corrupts a filesystem that was
 * merely dirty. Read-only is the state in which that refusal costs nothing.
 */
#ifndef MINIKERN_KERNEL_XFSMOUNT_H
#define MINIKERN_KERNEL_XFSMOUNT_H

#include <stdbool.h>
#include <stdint.h>

/* Mounts the filesystem on the attached disk. KERR_NODEV when there is no
 * disk, and whatever the filesystem says when there is one it will not
 * accept — which includes a dirty log, a feature set it does not fully
 * understand, and something that is not XFS at all. Each of those is a
 * different diagnosis and is reported as one. */
int xfsmount_attach(void);

/* True once a filesystem is mounted. */
bool xfsmount_present(void);

/* Walks the root directory and prints what is there, with sizes: the
 * cheapest proof that a filesystem has really been read rather than merely
 * opened. Returns how many entries it saw, or a negative error. */
int32_t xfsmount_list_root(void);

/* Reads a file whole and reports its first bytes and its length, which
 * proves the extent map and the data path rather than just the metadata. */
int xfsmount_show_file(const char *path);

#endif
