/* volinfo — what a volume is, read from its first block: the name a
 * program uses for it (`mmcblk1p2`, `nvme0n1p1`), its filesystem type, label
 * and identity (UUID or FAT serial).
 *
 * Pure and host-tested. `/proc/volumes` is made of these, and a mount by
 * device name (`mount("/dev/nvme0n1p1", ...)`) finds its volume through the
 * name. Names follow Linux's so a program written for Linux reads them
 * without translation: the controller's base name, then `p<n>` when the
 * base ends in a digit (`nvme0n1p1`, `mmcblk1p2`) or `<n>` when it does not
 * (`vda1`), and the base alone for a disk with no partition table. */
#ifndef MINIKERN_CORE_VOLINFO_H
#define MINIKERN_CORE_VOLINFO_H

#include <stdbool.h>
#include <stdint.h>

#define VOLINFO_NAME_MAX 16u  /* "mmcblk1p4" and a NUL, with room */
#define VOLINFO_LABEL_MAX 16u /* XFS keeps 12 characters, FAT 11 */
#define VOLINFO_UUID_MAX 40u  /* 36 characters of a UUID and a NUL */

struct volinfo {
    char name[VOLINFO_NAME_MAX];
    const char *fstype; /* "xfs", "vfat" or "unknown" */
    char label[VOLINFO_LABEL_MAX];
    char uuid[VOLINFO_UUID_MAX];
    uint64_t bytes;
    bool root_disk; /* on the disk the root filesystem was taken from */
};

/* The base name for a controller kind as disk.c names them ("nvme",
 * "sd (designware)", "sd", "virtio", "ram"); "disk" for anything else. */
const char *volinfo_base(const char *kind);

/* `base` with partition `part` (1-based), or the base alone for part 0. */
void volinfo_name(const char *base, uint32_t part, char out[VOLINFO_NAME_MAX]);

/* Fills fstype, label and uuid from the volume's first `len` bytes (at
 * least 512). Anything that is neither XFS nor FAT is "unknown", empty. */
void volinfo_probe(const uint8_t *block, uint32_t len, struct volinfo *out);

/* Whether two names are equal. */
bool volinfo_same(const char *a, const char *b);

#endif
