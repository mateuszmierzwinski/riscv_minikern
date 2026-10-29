/* Larger blocks over smaller sectors, on the host.
 *
 * This is address arithmetic, which is either exactly right or quietly reads
 * the wrong part of a disk — so the fake device below records every request
 * and the test checks the translation rather than just the data.
 */
#include "core/blockscale.h"

#include <stdint.h>
#include <string.h>

#include "core/errno.h"
#include "ktest.h"

/* A fake device of 512-byte sectors that fills each sector with its own
 * number, so a misaddressed read is visible in the data rather than merely
 * in a counter. */
#define FAKE_SECTORS 64u
#define FAKE_SECTOR 512u

static struct {
    uint64_t last_lba;
    uint32_t last_count;
    uint32_t reads;
    uint32_t writes;
    uint32_t flushes;
    uint32_t discards;
} seen;

static int fake_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    (void)self;
    seen.last_lba = lba;
    seen.last_count = count;
    seen.reads++;
    if (lba + count > FAKE_SECTORS) {
        return KERR_INVAL;
    }
    for (uint32_t s = 0; s < count; s++) {
        memset((uint8_t *)buf + s * FAKE_SECTOR, (int)(lba + s), FAKE_SECTOR);
    }
    return KERR_OK;
}

static int fake_write(void *self, uint64_t lba, uint32_t count, const void *buf) {
    (void)self;
    (void)buf;
    seen.last_lba = lba;
    seen.last_count = count;
    seen.writes++;
    return lba + count > FAKE_SECTORS ? KERR_INVAL : KERR_OK;
}

static int fake_flush(void *self) {
    (void)self;
    seen.flushes++;
    return KERR_OK;
}

static int fake_discard(void *self, uint64_t lba, uint32_t count) {
    (void)self;
    seen.last_lba = lba;
    seen.last_count = count;
    seen.discards++;
    return KERR_OK;
}

static const struct blockdev fake = {
    .read = fake_read, .write = fake_write, .flush = fake_flush, .discard = fake_discard,
    .self = NULL,
    .block_size = FAKE_SECTOR, .blocks = FAKE_SECTORS, .read_only = false,
};

/* Eight sectors to a 4 KiB block, which is the case that matters: XFS's
 * block size over every controller's sector size. */
static void test_four_kibibyte_blocks_over_sectors(void) {
    struct blockscale state;
    struct blockdev scaled;
    /* Two logical blocks' worth: the second read below asks for two, and a
     * 4 KiB buffer there was this test's own buffer overflow. */
    static uint8_t buf[8192];
    memset(&seen, 0, sizeof seen);
    KTEST_CHECK(blockscale_wrap(&fake, 4096u, &state, &scaled) == KERR_OK);
    KTEST_CHECK(scaled.block_size == 4096u);
    KTEST_CHECK(scaled.blocks == FAKE_SECTORS / 8u);
    /* Logical block 3 is sectors 24..31, and the data proves it rather than
     * the counter alone. */
    KTEST_CHECK(scaled.read(scaled.self, 3, 1, buf) == KERR_OK);
    KTEST_CHECK(seen.last_lba == 24u && seen.last_count == 8u);
    KTEST_CHECK(buf[0] == 24u && buf[4095] == 31u);
    /* Two logical blocks is sixteen sectors, in one request. */
    KTEST_CHECK(scaled.read(scaled.self, 1, 2, buf) == KERR_OK);
    KTEST_CHECK(seen.last_lba == 8u && seen.last_count == 16u);
}

/* A ratio that is not a whole number cannot be a block device: rounding one
 * into place is how a filesystem writes over the sector after the one it
 * meant. */
static void test_impossible_ratios_are_refused(void) {
    struct blockscale state;
    struct blockdev scaled;
    KTEST_CHECK(blockscale_wrap(&fake, 256u, &state, &scaled) == KERR_INVAL);  /* smaller */
    KTEST_CHECK(blockscale_wrap(&fake, 768u, &state, &scaled) == KERR_INVAL);  /* not a power of two */
    KTEST_CHECK(blockscale_wrap(&fake, 0u, &state, &scaled) == KERR_INVAL);
    KTEST_CHECK(blockscale_wrap(NULL, 4096u, &state, &scaled) == KERR_INVAL);
    /* The same size is allowed and is a ratio of one. */
    KTEST_CHECK(blockscale_wrap(&fake, FAKE_SECTOR, &state, &scaled) == KERR_OK);
    KTEST_CHECK(scaled.blocks == FAKE_SECTORS);
}

/* A device whose sector count is not a multiple of the ratio has a tail the
 * scaled view cannot address, and it must be **dropped** rather than rounded
 * up: a partial last block would read past the end of the device. */
static void test_a_partial_last_block_is_dropped(void) {
    struct blockdev odd = fake;
    struct blockscale state;
    struct blockdev scaled;
    odd.blocks = 63u; /* seven blocks of eight, with seven sectors spare */
    KTEST_CHECK(blockscale_wrap(&odd, 4096u, &state, &scaled) == KERR_OK);
    KTEST_CHECK(scaled.blocks == 7u);
}

static void test_flush_and_read_only_pass_through(void) {
    struct blockdev ro = fake;
    struct blockscale state;
    struct blockdev scaled;
    memset(&seen, 0, sizeof seen);
    ro.read_only = true;
    KTEST_CHECK(blockscale_wrap(&ro, 4096u, &state, &scaled) == KERR_OK);
    KTEST_CHECK(scaled.read_only);
    KTEST_CHECK(scaled.flush(scaled.self) == KERR_OK && seen.flushes == 1u);
}

/* A discard is scaled like any other access, and absent when the device
 * beneath cannot discard. */
static void test_discard_is_scaled_or_absent(void) {
    struct blockscale state;
    struct blockdev view, plain = fake;
    memset(&seen, 0, sizeof seen);
    KTEST_CHECK(blockscale_wrap(&fake, 4096u, &state, &view) == KERR_OK);
    KTEST_CHECK(view.discard != NULL);
    KTEST_CHECK(view.discard(view.self, 3u, 2u) == KERR_OK);
    KTEST_CHECK(seen.last_lba == 24u && seen.last_count == 16u && seen.discards == 1u);
    plain.discard = NULL;
    KTEST_CHECK(blockscale_wrap(&plain, 4096u, &state, &view) == KERR_OK);
    KTEST_CHECK(view.discard == NULL);
}

int main(void) {
    KTEST_RUN(test_four_kibibyte_blocks_over_sectors);
    KTEST_RUN(test_impossible_ratios_are_refused);
    KTEST_RUN(test_a_partial_last_block_is_dropped);
    KTEST_RUN(test_flush_and_read_only_pass_through);
    KTEST_RUN(test_discard_is_scaled_or_absent);
    return ktest_failures();
}
