/* What a volume is, from its first block. See `volinfo.h`. */
#include "core/volinfo.h"

#include "core/fat/fat.h"
#include "core/xfs/xfs.h"

/* Where the identity lives: XFS's `sb_uuid`, and the FAT boot sector's
 * volume serial, whose offset depends on the FAT32 layout (BPB_FATSz16 0). */
#define XFS_SB_UUID 32u
#define FAT_SERIAL_16 39u
#define FAT_SERIAL_32 67u
#define FAT_FATSZ16 22u

struct base_of {
    const char *kind;
    const char *base;
};

/* Linux's names: the board's card is the second MMC controller (mmc1), so
 * its disk is `mmcblk1`; an emulator's SDHCI is the first. */
static const struct base_of bases[] = {
    {"nvme", "nvme0n1"}, {"sd (designware)", "mmcblk1"}, {"sd", "mmcblk0"},
    {"virtio", "vda"},   {"ram", "ram0"},
};

bool volinfo_same(const char *a, const char *b) {
    uint32_t i = 0;
    while (a[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    return a[i] == b[i];
}

const char *volinfo_base(const char *kind) {
    for (uint32_t i = 0; kind != 0 && i < sizeof bases / sizeof bases[0]; i++) {
        if (volinfo_same(kind, bases[i].kind)) {
            return bases[i].base;
        }
    }
    return "disk";
}

void volinfo_name(const char *base, uint32_t part, char out[VOLINFO_NAME_MAX]) {
    uint32_t n = 0;
    while (base[n] != '\0' && n + 4u < VOLINFO_NAME_MAX) {
        out[n] = base[n];
        n++;
    }
    if (part > 0u && part < 100u) {
        if (n > 0u && out[n - 1u] >= '0' && out[n - 1u] <= '9') {
            out[n++] = 'p';
        }
        if (part >= 10u) {
            out[n++] = (char)('0' + part / 10u);
        }
        out[n++] = (char)('0' + part % 10u);
    }
    out[n] = '\0';
}

static const char hex[] = "0123456789abcdef";

/* 16 bytes as the 8-4-4-4-12 form blkid prints. */
static void uuid_text(const uint8_t *u, char *out) {
    uint32_t at = 0;
    for (uint32_t i = 0; i < 16u; i++) {
        if (i == 4u || i == 6u || i == 8u || i == 10u) {
            out[at++] = '-';
        }
        out[at++] = hex[u[i] >> 4];
        out[at++] = hex[u[i] & 15u];
    }
    out[at] = '\0';
}

/* A FAT serial as blkid prints it: "1A2B-3C4D", high word first. */
static void serial_text(const uint8_t *s, char *out) {
    static const char up[] = "0123456789ABCDEF";
    uint32_t v = (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 |
                 (uint32_t)s[3] << 24;
    uint32_t at = 0;
    for (int shift = 28; shift >= 0; shift -= 4) {
        out[at++] = up[(v >> (uint32_t)shift) & 15u];
        if (shift == 16) {
            out[at++] = '-';
        }
    }
    out[at] = '\0';
}

static void copy_label(const char *from, char *to) {
    uint32_t i = 0;
    while (from[i] != '\0' && i + 1u < VOLINFO_LABEL_MAX) {
        to[i] = from[i];
        i++;
    }
    to[i] = '\0';
}

void volinfo_probe(const uint8_t *block, uint32_t len, struct volinfo *out) {
    char xl[XFS_LABEL_BYTES];
    char fl[FAT_LABEL_BYTES];
    out->fstype = "unknown";
    out->label[0] = '\0';
    out->uuid[0] = '\0';
    if (block == 0 || len < 512u) {
        return;
    }
    if (xfs_superblock_label(block, len, xl)) {
        out->fstype = "xfs";
        copy_label(xl, out->label);
        uuid_text(block + XFS_SB_UUID, out->uuid);
        return;
    }
    if (fat_superblock_label(block, len, fl)) {
        bool fat32 = block[FAT_FATSZ16] == 0 && block[FAT_FATSZ16 + 1u] == 0;
        out->fstype = "vfat";
        copy_label(fl, out->label);
        serial_text(block + (fat32 ? FAT_SERIAL_32 : FAT_SERIAL_16), out->uuid);
    }
}
