/* Partition tables, against a block device made of bytes.
 *
 * The cases are built by writing the table a disk would actually carry, so
 * the test states the format rather than restating the parser. Both layouts
 * are here, and so is the one that matters most: a **GPT disk's protective
 * MBR**, which a reader that trusts MBR first will happily follow into
 * presenting the whole disk as one partition — hiding the real table and
 * putting every later read at the wrong offset.
 */
#include "core/partition.h"

#include "core/errno.h"
#include "ktest.h"

#define SECTOR 512u
#define SECTORS 64u

static uint8_t disk[SECTORS * SECTOR];

static int fake_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    uint8_t *to = buf;
    (void)self;
    if (lba + count > SECTORS) {
        return KERR_INVAL;
    }
    for (uint32_t i = 0; i < count * SECTOR; i++) {
        to[i] = disk[lba * SECTOR + i];
    }
    return KERR_OK;
}

static int fake_write(void *self, uint64_t lba, uint32_t count, const void *buf) {
    const uint8_t *from = buf;
    (void)self;
    if (lba + count > SECTORS) {
        return KERR_INVAL;
    }
    for (uint32_t i = 0; i < count * SECTOR; i++) {
        disk[lba * SECTOR + i] = from[i];
    }
    return KERR_OK;
}

static struct blockdev dev;

/* The next `failing_reads` reads of LBA `failing_lba` fail, as the board's SD
 * controller's first reads sometimes do. */
static uint64_t failing_lba;
static uint32_t failing_reads;

static int flaky_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    if (lba == failing_lba && failing_reads > 0) {
        failing_reads--;
        return KERR_IO;
    }
    return fake_read(self, lba, count, buf);
}

static void fresh(void) {
    for (uint32_t i = 0; i < sizeof disk; i++) {
        disk[i] = 0;
    }
    dev = (struct blockdev){.read = fake_read, .write = fake_write, .flush = NULL, .self = NULL,
                            .block_size = SECTOR, .blocks = SECTORS, .read_only = false};
}

static void put32(uint32_t at, uint32_t v) {
    disk[at] = (uint8_t)v;
    disk[at + 1u] = (uint8_t)(v >> 8);
    disk[at + 2u] = (uint8_t)(v >> 16);
    disk[at + 3u] = (uint8_t)(v >> 24);
}

static void put64(uint32_t at, uint64_t v) {
    put32(at, (uint32_t)v);
    put32(at + 4u, (uint32_t)(v >> 32));
}

/* An MBR entry: type, first sector, length. */
static void write_mbr_entry(uint32_t index, uint8_t type, uint32_t first, uint32_t sectors) {
    uint32_t at = 446u + index * 16u;
    disk[at + 4u] = type;
    put32(at + 8u, first);
    put32(at + 12u, sectors);
    disk[510] = 0x55u;
    disk[511] = 0xaau;
}

/* A GPT header in LBA 1 and one entry in LBA 2. */
static void write_gpt(uint64_t first, uint64_t last) {
    static const char magic[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    uint32_t entry = 2u * SECTOR;
    for (uint32_t i = 0; i < 8u; i++) {
        disk[SECTOR + i] = (uint8_t)magic[i];
    }
    put64(SECTOR + 72u, 2u); /* the entry array is in LBA 2 */
    put32(SECTOR + 80u, 4u); /* four entries declared */
    put32(SECTOR + 84u, 128u);
    disk[entry] = 0x0cu; /* a non-zero type GUID: this entry is used */
    put64(entry + 32u, first);
    put64(entry + 40u, last);
}

static void test_no_table_is_not_an_error(void) {
    struct partition p[PARTITION_MAX];
    fresh();
    KTEST_CHECK(partition_scan(&dev, p, PARTITION_MAX) == 0);
}

static void test_mbr_entry_is_found(void) {
    struct partition p[PARTITION_MAX];
    fresh();
    write_mbr_entry(0, 0x83u, 2048u, 100u);
    KTEST_CHECK(partition_scan(&dev, p, PARTITION_MAX) == 1);
    KTEST_CHECK(p[0].first == 2048u);
    KTEST_CHECK(p[0].sectors == 100u);
}

static void test_gpt_is_preferred_over_its_protective_mbr(void) {
    struct partition p[PARTITION_MAX];
    fresh();
    /* Exactly what a GPT disk carries: a protective entry spanning the disk,
     * and the real table in LBA 1. Following the MBR would report one
     * partition at sector 1 covering everything, and every read after that
     * would be one sector out. */
    write_mbr_entry(0, 0xeeu, 1u, SECTORS - 1u);
    write_gpt(2048u, 4095u);
    KTEST_CHECK(partition_scan(&dev, p, PARTITION_MAX) == 1);
    KTEST_CHECK(p[0].first == 2048u);
    KTEST_CHECK(p[0].sectors == 2048u);
}

static void test_a_protective_mbr_alone_yields_nothing(void) {
    struct partition p[PARTITION_MAX];
    fresh();
    write_mbr_entry(0, 0xeeu, 1u, SECTORS - 1u);
    /* No GPT behind it: the protective entry is still not a partition, and
     * reporting it would be worse than reporting none. */
    KTEST_CHECK(partition_scan(&dev, p, PARTITION_MAX) == 0);
}

/* **An unreadable table is not an absent one.** The GPT header's read fails
 * and the protective MBR alone yields nothing — which `partition_scan` has
 * to call "no table", and the checked scan must call an error. On the board
 * the first answer made a GPT card look like one whole-disk volume, and the
 * FAT boot partition was mounted as the root. */
static void test_an_unreadable_table_is_an_error(void) {
    struct partition p[PARTITION_MAX];
    uint32_t found = 99;
    fresh();
    write_mbr_entry(0, 0xeeu, 1u, SECTORS - 1u);
    write_gpt(2048u, 4095u);
    dev.read = flaky_read;
    failing_lba = 1u;
    failing_reads = 1u;
    KTEST_CHECK(partition_scan_checked(&dev, p, PARTITION_MAX, &found) == KERR_IO);
    KTEST_CHECK(found == 0);
    /* Read again, it is found: the error was the read's, not the disk's. */
    KTEST_CHECK(partition_scan_checked(&dev, p, PARTITION_MAX, &found) == KERR_OK);
    KTEST_CHECK(found == 1 && p[0].first == 2048u);
    /* And a disk that genuinely has no table is still not an error. */
    fresh();
    KTEST_CHECK(partition_scan_checked(&dev, p, PARTITION_MAX, &found) == KERR_OK);
    KTEST_CHECK(found == 0);
}

static void test_wrapping_shifts_every_access(void) {
    struct partition p = {.first = 4u, .sectors = 8u, .present = true};
    struct partdev state;
    struct blockdev view;
    uint8_t buf[SECTOR];
    fresh();
    disk[4u * SECTOR] = 0xa5u; /* the first byte of the partition */
    KTEST_CHECK(partition_wrap(&dev, &p, &state, &view) == KERR_OK);
    KTEST_CHECK(view.blocks == 8u);
    KTEST_CHECK(view.read(view.self, 0, 1, buf) == KERR_OK);
    KTEST_CHECK(buf[0] == 0xa5u);
}

/* Past the end is refused, not clamped and not passed through: a filesystem
 * that reads beyond its partition would otherwise read the next one's data
 * and treat it as its own. */
static void test_reading_past_the_end_is_refused(void) {
    struct partition p = {.first = 4u, .sectors = 8u, .present = true};
    struct partdev state;
    struct blockdev view;
    uint8_t buf[8u * SECTOR]; /* the whole partition: the last case reads it all */
    fresh();
    KTEST_CHECK(partition_wrap(&dev, &p, &state, &view) == KERR_OK);
    KTEST_CHECK(view.read(view.self, 8u, 1, buf) == KERR_INVAL);
    KTEST_CHECK(view.read(view.self, 7u, 2, buf) == KERR_INVAL);
    KTEST_CHECK(view.read(view.self, 0, 8u, buf) == KERR_OK);
}

static void test_a_partition_outside_the_disk_is_refused(void) {
    struct partition p = {.first = SECTORS - 2u, .sectors = 10u, .present = true};
    struct partdev state;
    struct blockdev view;
    fresh();
    KTEST_CHECK(partition_wrap(&dev, &p, &state, &view) == KERR_INVAL);
}

static void test_a_corrupt_gpt_header_reports_nothing(void) {
    struct partition p[PARTITION_MAX];
    fresh();
    write_gpt(2048u, 4095u);
    put32(SECTOR + 84u, 4u); /* an entry size smaller than an entry */
    KTEST_CHECK(partition_scan(&dev, p, PARTITION_MAX) == 0);
}

int main(void) {
    KTEST_RUN(test_no_table_is_not_an_error);
    KTEST_RUN(test_mbr_entry_is_found);
    KTEST_RUN(test_gpt_is_preferred_over_its_protective_mbr);
    KTEST_RUN(test_a_protective_mbr_alone_yields_nothing);
    KTEST_RUN(test_an_unreadable_table_is_an_error);
    KTEST_RUN(test_wrapping_shifts_every_access);
    KTEST_RUN(test_reading_past_the_end_is_refused);
    KTEST_RUN(test_a_partition_outside_the_disk_is_refused);
    KTEST_RUN(test_a_corrupt_gpt_header_reports_nothing);
    return ktest_failures();
}
