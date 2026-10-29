/* The file allocation table: one entry per cluster, naming the next cluster
 * of its chain, the end of one, or that the cluster is free.
 *
 * Three widths. FAT12 packs two entries into three bytes, so an entry may
 * straddle a sector — which byte-granular access through the cache absorbs
 * — and its nibble order depends on whether the cluster number is odd.
 * FAT32's entries are 28 bits in 32: the top four belong to nobody and are
 * kept as they were found.
 *
 * Every copy of the table is written alike. `fsck.fat` compares them and
 * reports the first difference as a damaged filesystem. */
#include "core/errno.h"
#include "core/fat/fat_internal.h"

int fat_pread(struct fat *fs, uint64_t off, void *buf, uint32_t len) {
    return bcache_pread(fs->cache, off, buf, len);
}

int fat_pwrite(struct fat *fs, uint64_t off, const void *buf, uint32_t len) {
    return fs->writable ? bcache_pwrite(fs->cache, off, buf, len) : KERR_ROFS;
}

static uint32_t width_mask(const struct fat *fs) {
    return fs->type == 12 ? 0xfffu : (fs->type == 16 ? 0xffffu : 0x0fffffffu);
}

bool fat_is_end(const struct fat *fs, uint32_t value) {
    return (value & width_mask(fs)) >= (width_mask(fs) & ~7u);
}

uint32_t fat_end_mark(const struct fat *fs) {
    return width_mask(fs);
}

bool fat_cluster_ok(const struct fat *fs, uint32_t c) {
    return c >= 2u && c < fs->clusters + 2u;
}

/* Where entry `c` starts in a table, in bytes. */
static uint64_t entry_offset(const struct fat *fs, uint32_t c) {
    return fs->type == 12 ? (uint64_t)c + c / 2u : (uint64_t)c * (fs->type == 16 ? 2u : 4u);
}

int fat_get(struct fat *fs, uint32_t c, uint32_t *value) {
    uint8_t b[4] = {0, 0, 0, 0};
    int rc = fat_pread(fs, fs->fat_start + entry_offset(fs, c), b, fs->type == 32 ? 4u : 2u);
    if (rc != KERR_OK) {
        return rc;
    }
    if (fs->type == 12) {
        uint16_t pair = fat_le16(b);
        *value = (c & 1u) != 0 ? (uint32_t)(pair >> 4) : (uint32_t)(pair & 0xfffu);
    } else {
        *value = (fs->type == 16 ? fat_le16(b) : fat_le32(b)) & width_mask(fs);
    }
    return KERR_OK;
}

/* The bytes entry `c` occupies in one table, with `value` put in them and
 * whatever else shares those bytes kept: the other half of a FAT12 pair, or
 * FAT32's top four bits. */
static int merge_entry(struct fat *fs, uint32_t c, uint32_t value, uint8_t *b, uint32_t *n) {
    int rc;
    *n = fs->type == 32 ? 4u : 2u;
    rc = fat_pread(fs, fs->fat_start + entry_offset(fs, c), b, *n);
    if (rc != KERR_OK) {
        return rc;
    }
    if (fs->type == 12) {
        uint16_t pair = fat_le16(b);
        pair = (c & 1u) != 0 ? (uint16_t)((pair & 0x000fu) | (value << 4))
                             : (uint16_t)((pair & 0xf000u) | (value & 0xfffu));
        fat_put16(b, pair);
    } else if (fs->type == 16) {
        fat_put16(b, (uint16_t)value);
    } else {
        fat_put32(b, (fat_le32(b) & 0xf0000000u) | (value & 0x0fffffffu));
    }
    return KERR_OK;
}

int fat_set(struct fat *fs, uint32_t c, uint32_t value) {
    uint8_t b[4];
    uint32_t n = 0;
    int rc = merge_entry(fs, c, value, b, &n);
    for (uint32_t i = 0; rc == KERR_OK && i < fs->fats; i++) {
        rc = fat_pwrite(fs, fs->fat_start + i * fs->fat_bytes + entry_offset(fs, c), b, n);
    }
    return rc;
}

/* Fills a cluster with zeroes, so that a directory's new cluster ends its
 * listing and a file's new cluster reads as nothing rather than as what was
 * there before — which would be another file's bytes. */
static int zero_cluster(struct fat *fs, uint32_t c) {
    static const uint8_t zeroes[512];
    int rc = KERR_OK;
    for (uint32_t done = 0; rc == KERR_OK && done < fs->cluster; done += sizeof zeroes) {
        rc = fat_pwrite(fs, fat_cluster_offset(fs, c) + done, zeroes, sizeof zeroes);
    }
    return rc;
}

/* The first free cluster from `next_free` on, round to the start. */
static int find_free(struct fat *fs, uint32_t *out) {
    for (uint32_t i = 0; i < fs->clusters; i++) {
        uint32_t c = 2u + (fs->next_free - 2u + i) % fs->clusters, v = 0;
        int rc = fat_get(fs, c, &v);
        if (rc != KERR_OK) {
            return rc;
        }
        if (v == FAT_CLUSTER_FREE) {
            *out = c;
            return KERR_OK;
        }
    }
    return KERR_NOSPC;
}

int fat_take_cluster(struct fat *fs, uint32_t prev, uint32_t *out) {
    int rc = fs->free_clusters == 0 ? KERR_NOSPC : find_free(fs, out);
    rc = rc != KERR_OK ? rc : zero_cluster(fs, *out);
    rc = rc != KERR_OK ? rc : fat_set(fs, *out, fat_end_mark(fs));
    rc = rc != KERR_OK || prev == 0 ? rc : fat_set(fs, prev, *out);
    if (rc == KERR_OK) {
        fs->free_clusters--;
        fs->next_free = *out + 1u < fs->clusters + 2u ? *out + 1u : 2u;
    }
    return rc;
}

int fat_free_chain(struct fat *fs, uint32_t first) {
    uint32_t c = first;
    for (uint32_t hops = 0; fat_cluster_ok(fs, c) && hops < fs->clusters; hops++) {
        uint32_t next = 0;
        int rc = fat_get(fs, c, &next);
        rc = rc != KERR_OK ? rc : fat_set(fs, c, FAT_CLUSTER_FREE);
        if (rc != KERR_OK) {
            return rc;
        }
        fs->free_clusters++;
        if (fat_is_end(fs, next)) {
            return KERR_OK;
        }
        c = next;
    }
    return fat_cluster_ok(fs, c) ? KERR_IO : KERR_OK; /* a loop, or a chain into nowhere */
}

int fat_nth_cluster(struct fat *fs, uint32_t first, uint32_t n, uint32_t *out) {
    uint32_t c = first;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t next = 0;
        int rc = fat_cluster_ok(fs, c) ? fat_get(fs, c, &next) : KERR_IO;
        if (rc != KERR_OK) {
            return rc;
        }
        if (fat_is_end(fs, next)) {
            return KERR_NOENT;
        }
        c = next;
    }
    *out = c;
    return fat_cluster_ok(fs, c) ? KERR_OK : KERR_IO;
}

int fat_count_free(struct fat *fs, uint32_t *out) {
    *out = 0;
    for (uint32_t c = 2; c < fs->clusters + 2u; c++) {
        uint32_t v = 0;
        int rc = fat_get(fs, c, &v);
        if (rc != KERR_OK) {
            return rc;
        }
        *out += v == FAT_CLUSTER_FREE ? 1u : 0u;
    }
    return KERR_OK;
}
