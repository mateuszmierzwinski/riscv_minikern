/* fat.h — the FAT filesystem: FAT12, FAT16 and FAT32, with long names.
 *
 * **Why it is here at all.** The board's U-Boot reads FAT and not XFS, so
 * the kernel it boots lives on a FAT partition (docs/BOOTING.md), and a
 * kernel that cannot see that partition cannot replace itself. This is
 * that partition, mounted at `/boot`: read and written, so a new image
 * copied there from a running system is what the next boot loads.
 *
 * Pure over the buffer cache, like the XFS core, and judged the same way:
 * by the vendor's checker (`fsck.fat -n`) on images `mkfs.vfat` made
 * (test/fat_write_test.sh), with mtools reading back what was written.
 *
 * A node is a file or directory found by name; it carries where its own
 * directory entry is, so a write that grows a file can record the new size
 * there. Names are UTF-8 in and out; on disk they are the short 8.3 name
 * every FAT has and, where the name needs one, the long name beside it. */
#ifndef MINIKERN_CORE_FAT_FAT_H
#define MINIKERN_CORE_FAT_FAT_H

#include <stdbool.h>
#include <stdint.h>

#include "core/bcache.h"

#define FAT_NAME_MAX 255u   /* a long name, in UTF-16 units */
#define FAT_LABEL_BYTES 12u /* eleven characters and a NUL */

struct fat {
    struct bcache *cache;
    uint8_t type; /* 12, 16 or 32 */
    uint32_t sector;          /* bytes per sector */
    uint32_t cluster;         /* bytes per cluster */
    uint32_t clusters;        /* data clusters: numbered 2 to clusters + 1 */
    uint32_t fats;            /* copies of the table, all kept identical */
    uint64_t fat_start;       /* byte offset of the first table */
    uint64_t fat_bytes;       /* bytes per table */
    uint64_t root_start;      /* FAT12/16: byte offset of the fixed root directory */
    uint32_t root_entries;    /* FAT12/16: how many entries it holds */
    uint32_t root_cluster;    /* FAT32: the root directory's first cluster */
    uint64_t data_start;      /* byte offset of cluster 2 */
    uint64_t fsinfo;          /* FAT32: byte offset of the FSInfo sector, 0 if none */
    uint32_t free_clusters;   /* counted at mount, kept as clusters move */
    uint32_t next_free;       /* where the next search for a free cluster starts */
    char label[FAT_LABEL_BYTES];
    uint64_t now_s;           /* the wall clock for timestamps, set by whoever mounts it */
    bool writable;
    bool mounted;
};

/* A file or directory, and where its directory entry is. The root
 * directory has no entry (`entry` is 0). */
struct fat_node {
    uint32_t first;    /* first cluster, 0 for an empty file or the FAT12/16 root */
    uint32_t size;     /* bytes; 0 for a directory */
    uint8_t attr;
    bool dir;
    uint64_t entry;    /* byte offset of its short entry on the volume */
    uint16_t mtime;    /* DOS time and date, as stored */
    uint16_t mdate;
};

struct fat_dirent {
    char name[FAT_NAME_MAX * 3u + 1u]; /* UTF-8, NUL-terminated */
    struct fat_node node;
};

typedef int (*fat_dirent_fn)(void *self, const struct fat_dirent *e);

/* A node's stored DOS time and date (`mtime`, `mdate`) as Unix seconds,
 * read as UTC (FAT keeps no zone; this kernel writes UTC). 0 for a date
 * never set. Two-second resolution, as stored. */
uint64_t fat_unix_time(uint16_t time, uint16_t date);

/* Whether a volume's first sector is a FAT boot sector, and its label — the
 * volume label in the boot sector, as `mkfs.vfat -n` writes it, trailing
 * spaces dropped. Reads nothing beyond the sector it is given. */
bool fat_superblock_label(const uint8_t *sector, uint32_t len, char out[FAT_LABEL_BYTES]);

/* Mounts the FAT on `cache`, counting its free clusters. Writable when the
 * device is. KERR_INVAL for a boot sector whose geometry does not add up. */
int fat_mount(struct fat *fs, struct bcache *cache);

int fat_root(const struct fat *fs, struct fat_node *out);
int fat_lookup(struct fat *fs, const struct fat_node *dir, const char *name, uint32_t len,
               struct fat_node *out);
int fat_path_lookup(struct fat *fs, const char *path, struct fat_node *out);
int fat_list(struct fat *fs, const struct fat_node *dir, fat_dirent_fn fn, void *self);

int fat_read(struct fat *fs, const struct fat_node *node, uint64_t off, void *buf, uint32_t len,
             uint32_t *got);

/* Writes at `off`, growing the file — and its clusters — as needed, and
 * records the new size in its entry; `node` is updated to match. */
int fat_write(struct fat *fs, struct fat_node *node, uint64_t off, const void *buf, uint32_t len,
              uint32_t *put);

/* Makes the file `size` bytes long: shorter frees the clusters past the
 * end, longer fills with zeroes, as FAT has no holes. */
int fat_truncate(struct fat *fs, struct fat_node *node, uint32_t size, uint64_t now_s);

/* Makes a file (or, with `dir`, a directory with its `.` and `..`) called
 * `name` in `dir`. KERR_EXIST when the name is taken. */
int fat_create(struct fat *fs, const struct fat_node *dir, const char *name, uint32_t len,
               bool is_dir, uint64_t now_s, struct fat_node *out);

/* Removes `name` from `dir`, freeing its clusters. A directory must be
 * empty (KERR_NOTEMPTY). */
int fat_unlink(struct fat *fs, const struct fat_node *dir, const char *name, uint32_t len);

/* Moves a name, replacing a file at the destination. */
int fat_rename(struct fat *fs, const struct fat_node *from_dir, const char *from,
               uint32_t from_len, const struct fat_node *to_dir, const char *to,
               uint32_t to_len, uint64_t now_s);

/* Writes the free count back to FSInfo and makes everything durable. */
int fat_flush(struct fat *fs);

#endif
