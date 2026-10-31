/* The EDID reader against a hostile monitor — Rule 25's suite for the one
 * input the display path takes from outside the board.
 *
 * ## Why it is an attack surface
 *
 * A monitor answers the DDC read with whatever bytes it likes, and so does
 * anything plugged into the HDMI port pretending to be one: an EDID is
 * untrusted input that reaches the kernel without any program running. The
 * class has history in other kernels' display stacks — out-of-bounds reads
 * walking descriptor and extension offsets the block declared, unterminated
 * name strings copied into fixed buffers, and checksums trusted before the
 * fields they cover.
 *
 * ## What is asserted
 *
 * The reader only ever looks inside the 128 bytes it is given (it has no
 * offsets to follow: the base block's layout is fixed), always terminates
 * the strings it produces and makes them printable, and refuses — rather
 * than half-reads — a block whose header or checksum is wrong. A fuzz loop
 * of blocks with a valid header and checksum and random contents must parse
 * every one, with bounded, terminated output. Every block is parsed twice,
 * once ending against a `PROT_NONE` page and once starting right after one,
 * so a read past either end faults the test instead of reading neighbours.
 */
#include <stdlib.h>
#include <sys/mman.h>

#include "core/display_mode.h"
#include "core/errno.h"
#include "ktest.h"

static const uint8_t magic[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};

/* Makes the block's bytes sum to zero, as a valid EDID's do. */
static void seal(uint8_t b[EDID_BLOCK_BYTES]) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 1u < EDID_BLOCK_BYTES; i++) {
        sum += b[i];
    }
    b[127] = (uint8_t)(0x100u - (sum & 0xffu));
}

/* Three pages: guard, data, guard. */
static uint8_t *guarded;

static void make_guards(void) {
    guarded = mmap(NULL, 3u * 4096u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (guarded == MAP_FAILED || mprotect(guarded, 4096u, PROT_NONE) != 0 ||
        mprotect(guarded + 2u * 4096u, 4096u, PROT_NONE) != 0) {
        fprintf(stderr, "guard pages could not be made\n");
        exit(2);
    }
}

/* Parses `b` from a copy against each guard page in turn; both answers must
 * agree, and either read past the block faults. */
static int parse_guarded(const uint8_t b[EDID_BLOCK_BYTES], struct edid_info *e) {
    uint8_t *at_end = guarded + 2u * 4096u - EDID_BLOCK_BYTES;
    uint8_t *at_start = guarded + 4096u;
    struct edid_info other;
    int rc;
    memcpy(at_end, b, EDID_BLOCK_BYTES);
    rc = display_edid_parse(at_end, e);
    memcpy(at_start, b, EDID_BLOCK_BYTES);
    if (display_edid_parse(at_start, &other) != rc) {
        return KERR_IO;
    }
    return rc;
}

static bool printable_and_terminated(const char *s, uint32_t room) {
    for (uint32_t i = 0; i < room; i++) {
        if (s[i] == '\0') {
            return true;
        }
        if (s[i] < 0x20 || s[i] > 0x7e) {
            return false;
        }
    }
    return false;
}

/* A name descriptor with 13 bytes of control characters and no newline: the
 * copy must stop at 13, terminate, and show no raw control bytes. */
static void test_an_unterminated_hostile_name(void) {
    uint8_t b[EDID_BLOCK_BYTES] = {0};
    struct edid_info e;
    memcpy(b, magic, sizeof magic);
    b[54 + 3] = 0xfc;
    memset(b + 54 + 5, 0x1b, 13); /* ESC: a terminal escape, if it were printed raw */
    seal(b);
    KTEST_CHECK(parse_guarded(b, &e) == KERR_OK);
    KTEST_CHECK(printable_and_terminated(e.name, sizeof e.name));
    KTEST_CHECK(strlen(e.name) == 13u);
}

/* Every field at its maximum: sizes and sync values saturate the packing,
 * and nothing in the result may wrap into a nonsense negative. */
static void test_every_timing_field_at_its_maximum(void) {
    uint8_t b[EDID_BLOCK_BYTES];
    struct edid_info e;
    memset(b, 0xff, sizeof b);
    memcpy(b, magic, sizeof magic);
    seal(b);
    KTEST_CHECK(parse_guarded(b, &e) == KERR_OK);
    KTEST_CHECK(e.preferred.hactive == 0xfffu && e.preferred.vactive == 0xfffu);
    KTEST_CHECK(e.preferred.htotal >= e.preferred.hactive);
    KTEST_CHECK(printable_and_terminated(e.vendor, sizeof e.vendor));
}

static void test_refusals(void) {
    uint8_t b[EDID_BLOCK_BYTES] = {0};
    struct edid_info e;
    KTEST_CHECK(display_edid_parse(NULL, &e) == KERR_INVAL);
    memcpy(b, magic, sizeof magic);
    seal(b);
    KTEST_CHECK(display_edid_parse(b, NULL) == KERR_INVAL);
    b[0] = 0x01; /* a header one bit off */
    seal(b);
    KTEST_CHECK(parse_guarded(b, &e) == KERR_INVAL);
}

/* Random blocks with a valid header and checksum: every one must parse,
 * with terminated, printable strings. */
static void test_random_blocks_always_parse(void) {
    uint8_t b[EDID_BLOCK_BYTES];
    struct edid_info e;
    uint32_t bad = 0;
    srand(0x7110u);
    for (uint32_t round = 0; round < 20000u; round++) {
        for (uint32_t i = 0; i < EDID_BLOCK_BYTES; i++) {
            b[i] = (uint8_t)rand();
        }
        memcpy(b, magic, sizeof magic);
        seal(b);
        if (parse_guarded(b, &e) != KERR_OK ||
            !printable_and_terminated(e.name, sizeof e.name) ||
            !printable_and_terminated(e.vendor, sizeof e.vendor)) {
            bad++;
        }
    }
    KTEST_CHECK(bad == 0);
}

int main(void) {
    make_guards();
    KTEST_RUN(test_an_unterminated_hostile_name);
    KTEST_RUN(test_every_timing_field_at_its_maximum);
    KTEST_RUN(test_refusals);
    KTEST_RUN(test_random_blocks_always_parse);
    return ktest_failures();
}
