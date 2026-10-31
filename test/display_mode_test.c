/* Video timings, and the numbers the display hardware is given for them.
 *
 * The expected register words are not derived here from the same formula —
 * that would test the formula against itself. They are the values working
 * bring-ups on this SoC wrote (StarFive's U-Boot for 1080p, docs/DISPLAY.md
 * §4.3), and the transmitter bytes worked out in §3.3. A wrong shift or a
 * swapped field shows as a mismatch against an independent source.
 */
#include "core/display_mode.h"

#include "core/errno.h"
#include "ktest.h"

static void test_the_two_cea_modes_are_known(void) {
    const struct display_timing *p1080 = display_mode_cea(1920, 1080);
    const struct display_timing *p720 = display_mode_cea(1280, 720);
    KTEST_CHECK(p1080 != NULL && p1080->vic == 16u && p1080->pixel_khz == 148500u);
    KTEST_CHECK(p720 != NULL && p720->vic == 4u && p720->pixel_khz == 74250u);
    KTEST_CHECK(display_mode_cea(640, 480) == NULL);
    KTEST_CHECK(display_mode_cea(1920, 1200) == NULL);
}

/* StarFive U-Boot's hard-coded 1080p writes (`sf_vop.c` L541–544). */
static void test_dc_words_match_the_vendor_bootloader_at_1080p(void) {
    struct dc_timing_words w;
    display_dc_timing(display_mode_cea(1920, 1080), &w);
    KTEST_CHECK(w.hsize == 0x08980780u);
    KTEST_CHECK(w.hsync == 0x440207D8u);
    KTEST_CHECK(w.vsize == 0x04650438u);
    KTEST_CHECK(w.vsync == 0x4220843Cu);
    KTEST_CHECK(w.plane_size == 0x021C0780u);
}

static void test_dc_words_at_720p(void) {
    struct dc_timing_words w;
    display_dc_timing(display_mode_cea(1280, 720), &w);
    KTEST_CHECK(w.hsize == 0x06720500u);
    KTEST_CHECK(w.hsync == 0x42CB056Eu);
    KTEST_CHECK(w.vsize == 0x02EE02D0u);
    KTEST_CHECK(w.vsync == 0x416D02D5u);
    KTEST_CHECK(w.plane_size == 0x01680500u);
}

/* A negative sync sets bit 31 and nothing else. */
static void test_negative_sync_sets_the_polarity_bit(void) {
    struct display_timing t = *display_mode_cea(1280, 720);
    struct dc_timing_words w;
    t.hsync_positive = false;
    display_dc_timing(&t, &w);
    KTEST_CHECK(w.hsync == (0x42CB056Eu | 0x80000000u));
    KTEST_CHECK(w.vsync == 0x416D02D5u);
}

static void test_hdmi_timing_bytes(void) {
    static const uint8_t want1080[HDMI_TIMING_BYTES] = {0x0d, 0x98, 0x08, 0x18, 0x01, 0xc0, 0x00,
                                                        0x2c, 0x00, 0x65, 0x04, 0x2d, 0x29, 0x05};
    static const uint8_t want720[HDMI_TIMING_BYTES] = {0x0d, 0x72, 0x06, 0x72, 0x01, 0x04, 0x01,
                                                       0x28, 0x00, 0xee, 0x02, 0x1e, 0x19, 0x05};
    uint8_t got[HDMI_TIMING_BYTES];
    display_hdmi_timing(display_mode_cea(1920, 1080), got);
    KTEST_CHECK(memcmp(got, want1080, sizeof got) == 0);
    display_hdmi_timing(display_mode_cea(1280, 720), got);
    KTEST_CHECK(memcmp(got, want720, sizeof got) == 0);
}

static void test_the_avi_infoframe_sums_to_zero(void) {
    uint8_t f[AVI_INFOFRAME_BYTES];
    uint32_t sum = 0;
    display_avi_infoframe(16u, f);
    for (uint32_t i = 0; i < AVI_INFOFRAME_BYTES; i++) {
        sum += f[i];
    }
    KTEST_CHECK((sum & 0xffu) == 0);
    KTEST_CHECK(f[0] == 0x82u && f[1] == 0x02u && f[2] == 0x0du && f[7] == 16u);
}

/* A plausible monitor: vendor "TST", product 0x1234, preferred 1080p60,
 * named "TESTMON". */
static void make_edid(uint8_t b[EDID_BLOCK_BYTES]) {
    static const uint8_t dtd[18] = {0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58,
                                    0x2c, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e};
    static const uint8_t name[18] = {0, 0, 0, 0xfc, 0, 'T', 'E', 'S', 'T', 'M', 'O', 'N', '\n',
                                     ' ', ' ', ' ', ' ', ' '};
    static const uint8_t magic[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    uint32_t sum = 0;
    memset(b, 0, EDID_BLOCK_BYTES);
    memcpy(b, magic, sizeof magic);
    b[8] = 0x52; /* T=20, S=19, T=20: 10100 10011 10100 */
    b[9] = 0x74;
    b[10] = 0x34;
    b[11] = 0x12;
    memcpy(b + 54, dtd, sizeof dtd);
    memcpy(b + 72, name, sizeof name);
    for (uint32_t i = 0; i + 1u < EDID_BLOCK_BYTES; i++) {
        sum += b[i];
    }
    b[127] = (uint8_t)(0x100u - (sum & 0xffu));
}

static void test_an_edid_is_read(void) {
    uint8_t b[EDID_BLOCK_BYTES];
    struct edid_info e;
    make_edid(b);
    KTEST_CHECK(display_edid_parse(b, &e) == KERR_OK);
    KTEST_CHECK_STR(e.vendor, "TST");
    KTEST_CHECK(e.product == 0x1234u);
    KTEST_CHECK_STR(e.name, "TESTMON");
    KTEST_CHECK(e.preferred.pixel_khz == 148500u);
    KTEST_CHECK(e.preferred.hactive == 1920u && e.preferred.vactive == 1080u);
    KTEST_CHECK(e.preferred.htotal == 2200u && e.preferred.vtotal == 1125u);
    KTEST_CHECK(e.preferred.hsync_start == 2008u && e.preferred.hsync_end == 2052u);
    KTEST_CHECK(e.preferred.vsync_start == 1084u && e.preferred.vsync_end == 1089u);
    KTEST_CHECK(e.preferred.hsync_positive && e.preferred.vsync_positive);
}

/* What a DDC line reads with no monitor, or a monitor with no power. */
static void test_an_empty_line_is_not_an_edid(void) {
    uint8_t b[EDID_BLOCK_BYTES];
    struct edid_info e;
    memset(b, 0xff, sizeof b);
    KTEST_CHECK(display_edid_parse(b, &e) == KERR_INVAL);
    memset(b, 0x00, sizeof b);
    KTEST_CHECK(display_edid_parse(b, &e) == KERR_INVAL);
    make_edid(b);
    b[100] ^= 1u;
    KTEST_CHECK(display_edid_parse(b, &e) == KERR_INVAL);
}

int main(void) {
    KTEST_RUN(test_the_two_cea_modes_are_known);
    KTEST_RUN(test_dc_words_match_the_vendor_bootloader_at_1080p);
    KTEST_RUN(test_dc_words_at_720p);
    KTEST_RUN(test_negative_sync_sets_the_polarity_bit);
    KTEST_RUN(test_hdmi_timing_bytes);
    KTEST_RUN(test_the_avi_infoframe_sums_to_zero);
    KTEST_RUN(test_an_edid_is_read);
    KTEST_RUN(test_an_empty_line_is_not_an_edid);
    return ktest_failures();
}
