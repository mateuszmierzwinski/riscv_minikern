/* fat_internal.h — what the parts of the FAT code share. */
#ifndef MINIKERN_CORE_FAT_FAT_INTERNAL_H
#define MINIKERN_CORE_FAT_FAT_INTERNAL_H

#include "core/fat/fat.h"

/* ── the on-disk format ───────────────────────────────────────────────── */

#define FAT_BS_BYTES_PER_SECTOR  11u /* u16 */
#define FAT_BS_SECTORS_PER_CLUST 13u /* u8 */
#define FAT_BS_RESERVED          14u /* u16 */
#define FAT_BS_FATS              16u /* u8 */
#define FAT_BS_ROOT_ENTRIES      17u /* u16, 0 on FAT32 */
#define FAT_BS_SECTORS16         19u /* u16, 0 when the 32-bit count is used */
#define FAT_BS_FAT_SECTORS16     22u /* u16, 0 on FAT32 */
#define FAT_BS_SECTORS32         32u /* u32 */
#define FAT_BS32_FAT_SECTORS     36u /* u32 */
#define FAT_BS32_ROOT_CLUSTER    44u /* u32 */
#define FAT_BS32_FSINFO          48u /* u16: sector of FSInfo */
#define FAT_BS32_LABEL           71u /* 11 bytes */
#define FAT_BS16_LABEL           43u /* 11 bytes */
#define FAT_BS_SIGNATURE         510u /* 0x55 0xaa */

#define FAT_FSI_LEAD       0u   /* "RRaA" */
#define FAT_FSI_STRUCT     484u /* "rrAa" */
#define FAT_FSI_FREE       488u /* u32, 0xffffffff when unknown */
#define FAT_FSI_NEXT       492u /* u32 */

#define FAT_ENTRY 32u           /* a directory entry */
#define FAT_DE_NAME    0u        /* 11 bytes: 8 + 3, space padded */
#define FAT_DE_ATTR    11u
#define FAT_DE_NTRES   12u       /* lower-case flags for the 8.3 name */
#define FAT_DE_CTIME   14u
#define FAT_DE_CDATE   16u
#define FAT_DE_ADATE   18u
#define FAT_DE_HICLUST 20u       /* u16: high half of the first cluster (FAT32) */
#define FAT_DE_MTIME   22u
#define FAT_DE_MDATE   24u
#define FAT_DE_LOCLUST 26u       /* u16 */
#define FAT_DE_SIZE    28u       /* u32 */

#define FAT_ATTR_RO     0x01u
#define FAT_ATTR_HIDDEN 0x02u
#define FAT_ATTR_SYSTEM 0x04u
#define FAT_ATTR_VOLUME 0x08u
#define FAT_ATTR_DIR    0x10u
#define FAT_ATTR_ARCH   0x20u
#define FAT_ATTR_LFN    0x0fu     /* the four low bits together: a long-name piece */

#define FAT_DE_END     0x00u      /* first byte: no entry here or after */
#define FAT_DE_DELETED 0xe5u      /* first byte: a free slot */

#define FAT_LFN_ORD     0u
#define FAT_LFN_LAST    0x40u     /* in the ordinal: the last piece, which comes first */
#define FAT_LFN_CHKSUM  13u
#define FAT_LFN_CHARS   13u       /* UTF-16 units per piece */

/* The table's end-of-chain and bad marks are the high values of each width. */
#define FAT_CLUSTER_FREE 0u

/* ── byte access ──────────────────────────────────────────────────────── */

static inline uint16_t fat_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t fat_le32(const uint8_t *p) {
    return (uint32_t)fat_le16(p) | ((uint32_t)fat_le16(p + 2) << 16);
}

static inline void fat_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void fat_put32(uint8_t *p, uint32_t v) {
    fat_put16(p, (uint16_t)v);
    fat_put16(p + 2, (uint16_t)(v >> 16));
}

int fat_pread(struct fat *fs, uint64_t off, void *buf, uint32_t len);
int fat_pwrite(struct fat *fs, uint64_t off, const void *buf, uint32_t len);

/* ── the table (fat_table.c) ──────────────────────────────────────────── */

/* Whether a table value ends a chain, for this width. */
bool fat_is_end(const struct fat *fs, uint32_t value);

/* The value that ends a chain, for this width. */
uint32_t fat_end_mark(const struct fat *fs);

/* Whether `c` names a data cluster. */
bool fat_cluster_ok(const struct fat *fs, uint32_t c);

int fat_get(struct fat *fs, uint32_t c, uint32_t *value);
/* Sets entry `c` in **every** copy of the table: `fsck.fat` compares them. */
int fat_set(struct fat *fs, uint32_t c, uint32_t value);

/* A free cluster, marked as the end of a chain and zero-filled, linked after
 * `prev` when `prev` is a cluster. KERR_NOSPC when none is left. */
int fat_take_cluster(struct fat *fs, uint32_t prev, uint32_t *out);

/* Frees the chain from `first` on. */
int fat_free_chain(struct fat *fs, uint32_t first);

/* The `n`th cluster of the chain from `first`, or KERR_NOENT past its end. */
int fat_nth_cluster(struct fat *fs, uint32_t first, uint32_t n, uint32_t *out);

/* Counts the free clusters, which the mount trusts more than FSInfo's hint. */
int fat_count_free(struct fat *fs, uint32_t *out);

static inline uint64_t fat_cluster_offset(const struct fat *fs, uint32_t c) {
    return fs->data_start + (uint64_t)(c - 2u) * fs->cluster;
}

/* ── directories (fat_dir.c) ─────────────────────────────────────────── */

/* A position in a directory: the fixed root of FAT12/16, or a chain. Slots
 * are numbered from 0 across the whole directory, whatever cluster they
 * fall in. */
struct fat_walk {
    const struct fat_node *dir;
    uint32_t cluster;   /* current cluster of a chain directory */
    uint32_t index;     /* the next slot's number in the directory */
};

void fat_walk_start(const struct fat *fs, const struct fat_node *dir, struct fat_walk *w);

/* The next 32-byte slot and its byte offset; KERR_NOENT past the last. */
int fat_walk_next(struct fat *fs, struct fat_walk *w, uint8_t *slot, uint64_t *off);

/* The byte offset of slot `index` of `dir`. */
int fat_slot_offset(struct fat *fs, const struct fat_node *dir, uint32_t index, uint64_t *off);

/* Makes a node from a short entry found at `off`. */
void fat_node_from_entry(const struct fat *fs, const uint8_t *e, uint64_t off,
                         struct fat_node *out);

/* A name found in a directory: its node, and the slots its entries take —
 * the long-name pieces, then the short entry. */
struct fat_found {
    struct fat_node node;
    uint32_t first;  /* the first slot: a long-name piece, or the short entry */
    uint32_t slots;  /* slots in all, short entry included */
};

int fat_find(struct fat *fs, const struct fat_node *dir, const char *name, uint32_t len,
             struct fat_found *out);

/* The first of `slots` free slots in a row in `dir`, growing a chain
 * directory by a cluster when it has none. KERR_NOSPC for a full fixed
 * root. */
int fat_dir_room(struct fat *fs, const struct fat_node *dir, uint32_t slots, uint32_t *first);

/* Rewrites a node's short entry — first cluster, size, time (fat_file.c). */
int fat_update_entry(struct fat *fs, const struct fat_node *node, uint64_t now_s);

/* ── names (fat_name.c) ───────────────────────────────────────────────── */

/* Whether `name` can be stored as an 8.3 name alone, and if so, that name;
 * otherwise a long name is needed. */
bool fat_short_exact(const char *name, uint32_t len, uint8_t out[11]);

/* A short name for a long one — its first letters, `~n`, its extension —
 * that `dir` does not already have. */
int fat_short_for(struct fat *fs, const struct fat_node *dir, const char *name, uint32_t len,
                  uint8_t out[11]);

/* The checksum of a short name, which every long-name piece repeats. */
uint8_t fat_short_checksum(const uint8_t name[11]);

/* UTF-8 ↔ UTF-16. KERR_INVAL for a name that is not valid UTF-8, or one
 * past FAT_NAME_MAX units. */
int fat_utf8_to_utf16(const char *in, uint32_t len, uint16_t *out, uint32_t *units);
uint32_t fat_utf16_to_utf8(const uint16_t *in, uint32_t units, char *out);

/* Whether two names are the same name to FAT: ASCII letters compare
 * without case, as Windows and Linux's vfat both do. */
bool fat_same_name(const char *a, uint32_t alen, const char *b, uint32_t blen);

/* The 8.3 name of an entry, rendered as `NAME.EXT`, lower-cased where its
 * NT flags say so. */
uint32_t fat_short_render(const uint8_t *e, char *out);

/* The DOS time and date for a unix time — UTC, there being no zone. */
void fat_dos_time(uint64_t now_s, uint16_t *time, uint16_t *date);

#endif
