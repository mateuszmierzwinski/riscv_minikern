/* Mounting: the boot sector's geometry, checked before anything is read
 * with it, and the free count.
 *
 * **The type is the cluster count's, not the label's.** Microsoft's own
 * specification is explicit that FAT12, FAT16 and FAT32 are told apart only
 * by how many data clusters there are — fewer than 4085, fewer than 65525,
 * or more — and the "FAT16   " string in the boot sector is a comment. A
 * reader that believed the string would read a FAT32 volume's table with
 * the wrong width. */
#include <stddef.h>

#include "core/errno.h"
#include "core/fat/fat_internal.h"

/* The label, with the spaces it is padded to dropped. */
static void take_label(const uint8_t *at, char out[FAT_LABEL_BYTES]) {
    uint32_t n = 11u;
    while (n > 0 && (at[n - 1u] == ' ' || at[n - 1u] == 0)) {
        n--;
    }
    for (uint32_t i = 0; i < n; i++) {
        out[i] = (char)at[i];
    }
    out[n] = '\0';
}

/* Whether a sector is a boot sector this reads: the signature, a sector size
 * and cluster size that are powers of two, at least one table. */
static bool plausible(const uint8_t *s, uint32_t len) {
    uint32_t bps, spc;
    if (len < 512u || s[FAT_BS_SIGNATURE] != 0x55u || s[FAT_BS_SIGNATURE + 1u] != 0xaau) {
        return false;
    }
    bps = fat_le16(s + FAT_BS_BYTES_PER_SECTOR);
    spc = s[FAT_BS_SECTORS_PER_CLUST];
    return bps >= 512u && bps <= 4096u && (bps & (bps - 1u)) == 0 && spc != 0 &&
           (spc & (spc - 1u)) == 0 && s[FAT_BS_FATS] != 0 &&
           fat_le16(s + FAT_BS_RESERVED) != 0;
}

/* FAT32 has no fixed root and keeps its table size in a 32-bit field. */
static bool is_fat32_layout(const uint8_t *s) {
    return fat_le16(s + FAT_BS_ROOT_ENTRIES) == 0 && fat_le16(s + FAT_BS_FAT_SECTORS16) == 0;
}

bool fat_superblock_label(const uint8_t *sector, uint32_t len, char out[FAT_LABEL_BYTES]) {
    out[0] = '\0';
    if (sector == NULL || !plausible(sector, len)) {
        return false;
    }
    take_label(sector + (is_fat32_layout(sector) ? FAT_BS32_LABEL : FAT_BS16_LABEL), out);
    /* "NO NAME" is what `mkfs.vfat` writes for no label at all. */
    if (out[0] == 'N' && out[1] == 'O' && out[2] == ' ' && out[3] == 'N') {
        out[0] = '\0';
    }
    return true;
}

/* The layout, in bytes, from the boot sector. */
static int take_geometry(struct fat *fs, const uint8_t *s) {
    uint64_t total = fat_le16(s + FAT_BS_SECTORS16) != 0 ? fat_le16(s + FAT_BS_SECTORS16)
                                                         : fat_le32(s + FAT_BS_SECTORS32);
    uint64_t fat_sectors = is_fat32_layout(s) ? fat_le32(s + FAT_BS32_FAT_SECTORS)
                                              : fat_le16(s + FAT_BS_FAT_SECTORS16);
    uint64_t reserved = fat_le16(s + FAT_BS_RESERVED), root_bytes, data_sectors;
    fs->sector = fat_le16(s + FAT_BS_BYTES_PER_SECTOR);
    fs->cluster = fs->sector * s[FAT_BS_SECTORS_PER_CLUST];
    fs->fats = s[FAT_BS_FATS];
    fs->root_entries = fat_le16(s + FAT_BS_ROOT_ENTRIES);
    root_bytes = ((uint64_t)fs->root_entries * FAT_ENTRY + fs->sector - 1u) / fs->sector * fs->sector;
    fs->fat_start = reserved * fs->sector;
    fs->fat_bytes = fat_sectors * fs->sector;
    fs->root_start = fs->fat_start + fs->fats * fs->fat_bytes;
    fs->data_start = fs->root_start + root_bytes;
    if (fat_sectors == 0 || fs->data_start >= total * fs->sector) {
        return KERR_INVAL;
    }
    data_sectors = total - fs->data_start / fs->sector;
    fs->clusters = (uint32_t)(data_sectors / s[FAT_BS_SECTORS_PER_CLUST]);
    fs->type = fs->clusters < 4085u ? 12u : (fs->clusters < 65525u ? 16u : 32u);
    return KERR_OK;
}

/* The table must hold an entry for every cluster, or a chain could name a
 * cluster whose entry is past the table's end; and FAT32 must say so. */
static int check_geometry(const struct fat *fs, const uint8_t *s) {
    uint64_t entries_bytes = fs->type == 12 ? ((uint64_t)fs->clusters + 2u) * 3u / 2u
                                            : ((uint64_t)fs->clusters + 2u) * (fs->type / 8u);
    if (entries_bytes > fs->fat_bytes) {
        return KERR_INVAL;
    }
    if ((fs->type == 32) != is_fat32_layout(s)) {
        return KERR_INVAL; /* a FAT32 count with a FAT16 layout, or the reverse */
    }
    return KERR_OK;
}

/* FAT32's root cluster and FSInfo sector. */
static void take_fat32(struct fat *fs, const uint8_t *s) {
    uint32_t fsi = fat_le16(s + FAT_BS32_FSINFO);
    fs->root_cluster = fat_le32(s + FAT_BS32_ROOT_CLUSTER);
    fs->fsinfo = fsi != 0 && fsi != 0xffffu ? (uint64_t)fsi * fs->sector : 0;
}

int fat_mount(struct fat *fs, struct bcache *cache) {
    uint8_t s[512];
    int rc;
    *fs = (struct fat){.cache = cache};
    rc = fat_pread(fs, 0, s, sizeof s);
    if (rc != KERR_OK) {
        return rc;
    }
    if (!plausible(s, sizeof s)) {
        return KERR_INVAL;
    }
    rc = take_geometry(fs, s);
    rc = rc != KERR_OK ? rc : check_geometry(fs, s);
    if (rc != KERR_OK) {
        return rc;
    }
    if (fs->type == 32) {
        take_fat32(fs, s);
    }
    (void)fat_superblock_label(s, sizeof s, fs->label);
    fs->next_free = 2;
    rc = fat_count_free(fs, &fs->free_clusters);
    fs->writable = !cache->dev->read_only && cache->dev->write != NULL;
    fs->mounted = rc == KERR_OK;
    return rc;
}

int fat_root(const struct fat *fs, struct fat_node *out) {
    *out = (struct fat_node){.first = fs->type == 32 ? fs->root_cluster : 0,
                             .attr = FAT_ATTR_DIR, .dir = true};
    return fs->mounted ? KERR_OK : KERR_INVAL;
}

/* The free count FSInfo keeps is a hint; it is written back so the next
 * mount elsewhere — Linux, U-Boot — has a correct one, which `fsck.fat`
 * checks. */
static int write_fsinfo(struct fat *fs) {
    uint8_t lead[4], word[8];
    int rc;
    if (fs->type != 32 || fs->fsinfo == 0 || !fs->writable) {
        return KERR_OK;
    }
    rc = fat_pread(fs, fs->fsinfo + FAT_FSI_LEAD, lead, sizeof lead);
    if (rc != KERR_OK || fat_le32(lead) != 0x41615252u) {
        return rc; /* not an FSInfo sector: nothing to keep current */
    }
    fat_put32(word, fs->free_clusters);
    fat_put32(word + 4u, fs->next_free);
    return fat_pwrite(fs, fs->fsinfo + FAT_FSI_FREE, word, sizeof word);
}

int fat_flush(struct fat *fs) {
    int rc = fs->mounted ? write_fsinfo(fs) : KERR_INVAL;
    return rc != KERR_OK ? rc : bcache_flush(fs->cache);
}
