/* volinfo: names in Linux's form, and what a first block says the volume
 * is. The blocks are built byte by byte from the on-disk formats (XFS's
 * superblock, a FAT boot sector), so the test states the formats rather
 * than restating the reader. */
#include "core/volinfo.h"

#include "ktest.h"

static uint8_t block[4096];

static void clear(void) {
    memset(block, 0, sizeof block);
}

static void put_le16(uint32_t at, uint32_t v) {
    block[at] = (uint8_t)v;
    block[at + 1u] = (uint8_t)(v >> 8);
}

/* An XFS superblock: magic "XFSB", sb_uuid at 32, sb_fname at 108. */
static void make_xfs(const char *label) {
    clear();
    memcpy(block, "XFSB", 4);
    for (uint32_t i = 0; i < 16u; i++) {
        block[32u + i] = (uint8_t)(0x10u + i);
    }
    memcpy(block + 108, label, strlen(label));
}

/* A FAT32 boot sector: 512-byte sectors, 8 per cluster, 32 reserved, two
 * FATs, no fixed root (FAT32), serial at 67, label at 71, signature. */
static void make_fat32(const char *label) {
    clear();
    put_le16(11, 512);
    block[13] = 8;
    put_le16(14, 32);
    block[16] = 2;
    block[67] = 0x4d;
    block[68] = 0x3c;
    block[69] = 0x2b;
    block[70] = 0x1a;
    memset(block + 71, ' ', 11);
    memcpy(block + 71, label, strlen(label));
    block[510] = 0x55;
    block[511] = 0xaa;
}

static void test_names(void) {
    char out[VOLINFO_NAME_MAX];
    volinfo_name(volinfo_base("nvme"), 1, out);
    KTEST_CHECK_STR(out, "nvme0n1p1");
    volinfo_name(volinfo_base("sd (designware)"), 2, out);
    KTEST_CHECK_STR(out, "mmcblk1p2");
    volinfo_name(volinfo_base("virtio"), 3, out);
    KTEST_CHECK_STR(out, "vda3");
    volinfo_name(volinfo_base("virtio"), 0, out);
    KTEST_CHECK_STR(out, "vda");
    volinfo_name(volinfo_base("ram"), 12, out);
    KTEST_CHECK_STR(out, "ram0p12");
    KTEST_CHECK_STR(volinfo_base("tape"), "disk");
    KTEST_CHECK_STR(volinfo_base(0), "disk");
}

static void test_xfs(void) {
    struct volinfo v = {0};
    make_xfs("DATA");
    volinfo_probe(block, sizeof block, &v);
    KTEST_CHECK_STR(v.fstype, "xfs");
    KTEST_CHECK_STR(v.label, "DATA");
    KTEST_CHECK_STR(v.uuid, "10111213-1415-1617-1819-1a1b1c1d1e1f");
}

static void test_fat(void) {
    struct volinfo v = {0};
    make_fat32("BOOT");
    volinfo_probe(block, sizeof block, &v);
    KTEST_CHECK_STR(v.fstype, "vfat");
    KTEST_CHECK_STR(v.label, "BOOT");
    KTEST_CHECK_STR(v.uuid, "1A2B-3C4D");
}

static void test_unknown(void) {
    struct volinfo v = {0};
    clear();
    memcpy(block + 1080, "\x53\xef", 2); /* an ext4 magic: not ours */
    volinfo_probe(block, sizeof block, &v);
    KTEST_CHECK_STR(v.fstype, "unknown");
    KTEST_CHECK_STR(v.label, "");
    volinfo_probe(block, 100, &v); /* too short to be anything */
    KTEST_CHECK_STR(v.fstype, "unknown");
    volinfo_probe(0, 4096, &v);
    KTEST_CHECK_STR(v.fstype, "unknown");
}

static void test_same(void) {
    KTEST_CHECK(volinfo_same("a", "a"));
    KTEST_CHECK(!volinfo_same("a", "ab"));
    KTEST_CHECK(!volinfo_same("ab", "a"));
}

int main(void) {
    KTEST_RUN(test_names);
    KTEST_RUN(test_xfs);
    KTEST_RUN(test_fat);
    KTEST_RUN(test_unknown);
    KTEST_RUN(test_same);
    return ktest_failures();
}
