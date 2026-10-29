/* fatmount — a FAT volume behind the kernel's path-based filesystem calls.
 *
 * `xfsmount.c` is the one place the system calls reach for a filesystem, and
 * every one of its path-based calls asks the volume table first whether the
 * path lies on a FAT volume (`volumes_resolve_fat`); when it does, the call
 * is answered here instead. So a program sees `/boot` as a directory like
 * any other — `ls`, `cat`, `cp`, `mv`, `rm` — and nothing above the
 * filesystem layer learns that it is FAT.
 *
 * What FAT cannot hold is refused rather than pretended: no owners (all is
 * root's), no permission bits (directories 0755, files 0644), no links, no
 * symbolic links. `within` is the path inside the volume. */
#ifndef KERNEL_FATMOUNT_H
#define KERNEL_FATMOUNT_H

#include <stdint.h>

#include "core/fat/fat.h"
#include "core/xfs/xfs.h"

int fatmount_stat_full(struct fat *fs, const char *within, uint32_t *mode, uint64_t *size,
                       uint32_t *nlink);
int fatmount_list(struct fat *fs, const char *within, xfs_dirent_fn fn, void *self);
int fatmount_read_file(struct fat *fs, const char *within, uint32_t limit, uint8_t **out,
                       uint32_t *size);
int fatmount_write_at(struct fat *fs, const char *dir, const char *leaf, uint64_t off,
                      const void *bytes, uint32_t len, uint64_t now_s);
int fatmount_mkdir_p(struct fat *fs, const char *within, uint64_t now_s);
int fatmount_unlink_path(struct fat *fs, const char *within);
int fatmount_rename_path(struct fat *fs, const char *from, const char *to, uint64_t now_s);
int fatmount_truncate_path(struct fat *fs, const char *within, uint64_t size, uint64_t now_s);
int fatmount_counters(struct fat *fs, struct xfs_counters *out);

#endif
