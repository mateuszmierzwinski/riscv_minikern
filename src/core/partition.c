#include "core/partition.h"

#include <stddef.h>

#include "core/errno.h"

/* The protective MBR's partition type, and where the MBR's entries live. */
#define MBR_TABLE_OFFSET 446u
#define MBR_ENTRY_BYTES 16u
#define MBR_TYPE_GPT_PROTECTIVE 0xeeu
#define MBR_TYPE_EMPTY 0x00u
#define MBR_SIGNATURE_OFFSET 510u

/* GPT: its header is in the second sector, and names itself. */
#define GPT_HEADER_LBA 1u
#define GPT_ENTRY_BYTES 128u
#define GPT_FIRST_LBA_AT 32u
#define GPT_LAST_LBA_AT 40u
#define GPT_TABLE_LBA_AT 72u
#define GPT_ENTRY_COUNT_AT 80u
#define GPT_ENTRY_SIZE_AT 84u

/* One sector's worth, shared by the two readers: they run one after the
 * other and never hold a sector across the handover. */
static uint8_t sector[4096];

/* Whether a read during the current scan failed. A scan that could not read
 * the table has not learned that there is none, and saying "no table" for it
 * made a partitioned card look like a whole-disk one — the first SD read
 * after bring-up fails now and then, and the kernel then mounted the FAT
 * boot partition as its root. */
static bool read_failed;

/* Reads one sector into `sector`, remembering a failure. */
static bool read_sector(struct blockdev *dev, uint64_t lba) {
    if (dev->read(dev->self, lba, 1, sector) == KERR_OK) {
        return true;
    }
    read_failed = true;
    return false;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

/* True when the 16 bytes at `p` are all zero — an unused GPT entry, whose
 * type GUID is zero. */
static bool guid_is_zero(const uint8_t *p) {
    for (uint32_t i = 0; i < 16u; i++) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool is_efi_part(const uint8_t *p) {
    static const char magic[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    for (uint32_t i = 0; i < 8u; i++) {
        if ((char)p[i] != magic[i]) {
            return false;
        }
    }
    return true;
}

/* Reads the GPT's entry array and keeps the entries that describe a real
 * span. The array's location, stride and count all come from the header
 * rather than being assumed: a disk may use 128-byte entries starting at
 * LBA 2, and nothing says it must. */
static uint32_t read_gpt_entries(struct blockdev *dev, uint64_t table_lba, uint32_t count,
                                 uint32_t stride, struct partition *out, uint32_t cap) {
    uint32_t per_sector = dev->block_size / stride;
    uint32_t found = 0;
    if (per_sector == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < count && found < cap; i++) {
        const uint8_t *e;
        if (i % per_sector == 0 && !read_sector(dev, table_lba + i / per_sector)) {
            return found;
        }
        e = sector + (i % per_sector) * stride;
        if (guid_is_zero(e)) {
            continue;
        }
        {
            uint64_t first = le64(e + GPT_FIRST_LBA_AT);
            uint64_t last = le64(e + GPT_LAST_LBA_AT);
            if (last < first) {
                continue;
            }
            out[found].first = first;
            out[found].sectors = last - first + 1u;
            out[found].present = true;
            found++;
        }
    }
    return found;
}

static uint32_t scan_gpt(struct blockdev *dev, struct partition *out, uint32_t cap) {
    uint64_t table_lba;
    uint32_t count, stride;
    if (!read_sector(dev, GPT_HEADER_LBA) || !is_efi_part(sector)) {
        return 0;
    }
    table_lba = le64(sector + GPT_TABLE_LBA_AT);
    count = le32(sector + GPT_ENTRY_COUNT_AT);
    stride = le32(sector + GPT_ENTRY_SIZE_AT);
    /* A header that describes an impossible array is a corrupt header, and
     * reading it anyway would hand the caller an offset into the middle of
     * somebody's data. */
    if (stride < GPT_ENTRY_BYTES || stride > dev->block_size || count == 0 || count > 1024u) {
        return 0;
    }
    return read_gpt_entries(dev, table_lba, count, stride, out, cap);
}

static uint32_t scan_mbr(struct blockdev *dev, struct partition *out, uint32_t cap) {
    uint32_t found = 0;
    if (!read_sector(dev, 0)) {
        return 0;
    }
    if (sector[MBR_SIGNATURE_OFFSET] != 0x55u || sector[MBR_SIGNATURE_OFFSET + 1u] != 0xaau) {
        return 0; /* no table: the filesystem starts at sector 0 */
    }
    for (uint32_t i = 0; i < 4u && found < cap; i++) {
        const uint8_t *e = sector + MBR_TABLE_OFFSET + i * MBR_ENTRY_BYTES;
        uint32_t first = le32(e + 8u);
        uint32_t sectors = le32(e + 12u);
        /* The protective entry of a GPT disk is skipped, not followed: it
         * spans the whole disk and would hide the real table. */
        if (e[4] == MBR_TYPE_EMPTY || e[4] == MBR_TYPE_GPT_PROTECTIVE || sectors == 0) {
            continue;
        }
        out[found].first = first;
        out[found].sectors = sectors;
        out[found].present = true;
        found++;
    }
    return found;
}

int partition_scan_checked(struct blockdev *dev, struct partition *out, uint32_t cap,
                           uint32_t *found) {
    *found = 0;
    if (dev->block_size > sizeof sector || cap == 0) {
        return KERR_OK;
    }
    read_failed = false;
    *found = scan_gpt(dev, out, cap);
    if (*found == 0) {
        *found = scan_mbr(dev, out, cap);
    }
    return read_failed && *found == 0 ? KERR_IO : KERR_OK;
}

uint32_t partition_scan(struct blockdev *dev, struct partition *out, uint32_t cap) {
    uint32_t found = 0;
    (void)partition_scan_checked(dev, out, cap, &found);
    return found;
}

/* Every access is shifted, and one past the end is refused rather than
 * clamped: a filesystem that reads past its partition has a bug, and letting
 * it read the next partition's data instead of failing turns that bug into
 * silent corruption. */
static bool inside(const struct partdev *p, uint64_t lba, uint32_t count) {
    return count > 0 && lba <= p->sectors && count <= p->sectors - lba;
}

static int part_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    struct partdev *p = self;
    if (!inside(p, lba, count)) {
        return KERR_INVAL;
    }
    return p->under->read(p->under->self, p->first + lba, count, buf);
}

static int part_write(void *self, uint64_t lba, uint32_t count, const void *buf) {
    struct partdev *p = self;
    if (!inside(p, lba, count)) {
        return KERR_INVAL;
    }
    if (p->under->write == NULL) {
        return KERR_ROFS;
    }
    return p->under->write(p->under->self, p->first + lba, count, buf);
}

static int part_flush(void *self) {
    struct partdev *p = self;
    return p->under->flush == NULL ? KERR_OK : p->under->flush(p->under->self);
}

int partition_wrap(struct blockdev *dev, const struct partition *p, struct partdev *state,
                   struct blockdev *out) {
    if (!p->present || p->sectors == 0 || p->first >= dev->blocks ||
        p->sectors > dev->blocks - p->first) {
        return KERR_INVAL;
    }
    *state = (struct partdev){.under = dev, .first = p->first, .sectors = p->sectors};
    *out = (struct blockdev){.read = part_read,
                             .write = part_write,
                             .flush = part_flush,
                             .self = state,
                             .block_size = dev->block_size,
                             .blocks = p->sectors,
                             .read_only = dev->read_only};
    return KERR_OK;
}
