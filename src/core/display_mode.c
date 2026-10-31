#include "core/display_mode.h"

#include <stddef.h>

#include "core/errno.h"

/* CEA-861 timings, the two the transmitter has PLL settings for on this SoC
 * (docs/DISPLAY.md §3.2). Both are positive-sync. */
static const struct display_timing cea_modes[] = {
    {148500u, 1920u, 2008u, 2052u, 2200u, 1080u, 1084u, 1089u, 1125u, true, true, 16u},
    {74250u, 1280u, 1390u, 1430u, 1650u, 720u, 725u, 730u, 750u, true, true, 4u},
};

const struct display_timing *display_mode_cea(uint32_t width, uint32_t height) {
    for (uint32_t i = 0; i < sizeof cea_modes / sizeof cea_modes[0]; i++) {
        if (cea_modes[i].hactive == width && cea_modes[i].vactive == height) {
            return &cea_modes[i];
        }
    }
    return NULL;
}

#define DC_SYNC_ENABLE (1u << 30)
#define DC_SYNC_NEGATIVE (1u << 31)
#define DC_COORD_SHIFT 15u

static uint32_t dc_sync(uint16_t start, uint16_t end, bool positive) {
    return start | (uint32_t)end << DC_COORD_SHIFT | DC_SYNC_ENABLE |
           (positive ? 0u : DC_SYNC_NEGATIVE);
}

void display_dc_timing(const struct display_timing *t, struct dc_timing_words *out) {
    out->hsize = t->hactive | (uint32_t)t->htotal << 16;
    out->hsync = dc_sync(t->hsync_start, t->hsync_end, t->hsync_positive);
    out->vsize = t->vactive | (uint32_t)t->vtotal << 16;
    out->vsync = dc_sync(t->vsync_start, t->vsync_end, t->vsync_positive);
    out->plane_size = t->hactive | (uint32_t)t->vactive << DC_COORD_SHIFT;
}

/* Register 0x08: external timing (bit 0), positive hsync (bit 2), positive
 * vsync (bit 3). */
#define HDMI_TIMING_EXTERNAL 0x01u
#define HDMI_TIMING_HSYNC_POSITIVE 0x04u
#define HDMI_TIMING_VSYNC_POSITIVE 0x08u

static void put_pair(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)(value & 0xffu);
    at[1] = (uint8_t)(value >> 8);
}

void display_hdmi_timing(const struct display_timing *t, uint8_t out[HDMI_TIMING_BYTES]) {
    out[0] = (uint8_t)(HDMI_TIMING_EXTERNAL | (t->hsync_positive ? HDMI_TIMING_HSYNC_POSITIVE : 0u) |
                       (t->vsync_positive ? HDMI_TIMING_VSYNC_POSITIVE : 0u));
    put_pair(&out[1], t->htotal);
    put_pair(&out[3], (uint32_t)(t->htotal - t->hactive));
    put_pair(&out[5], (uint32_t)(t->htotal - t->hsync_start));
    put_pair(&out[7], (uint32_t)(t->hsync_end - t->hsync_start));
    put_pair(&out[9], t->vtotal);
    out[11] = (uint8_t)(t->vtotal - t->vactive);
    out[12] = (uint8_t)(t->vtotal - t->vsync_start);
    out[13] = (uint8_t)(t->vsync_end - t->vsync_start);
}

/* Header: AVI (0x82), version 2, 13 payload bytes. Payload byte 1: RGB, no
 * scan information; byte 2: aspect "same as the picture"; byte 4: the VIC. */
#define AVI_TYPE 0x82u
#define AVI_VERSION 0x02u
#define AVI_LENGTH 0x0du
#define AVI_ASPECT_AS_PICTURE 0x08u

void display_avi_infoframe(uint8_t vic, uint8_t out[AVI_INFOFRAME_BYTES]) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < AVI_INFOFRAME_BYTES; i++) {
        out[i] = 0;
    }
    out[0] = AVI_TYPE;
    out[1] = AVI_VERSION;
    out[2] = AVI_LENGTH;
    out[5] = AVI_ASPECT_AS_PICTURE;
    out[7] = vic;
    for (uint32_t i = 0; i < AVI_INFOFRAME_BYTES; i++) {
        sum += out[i];
    }
    out[3] = (uint8_t)(0x100u - (sum & 0xffu));
}

/* ── EDID ──────────────────────────────────────────────────────────────── */

#define EDID_VENDOR_AT 8u
#define EDID_PRODUCT_AT 10u
#define EDID_DESCRIPTORS_AT 54u
#define EDID_DESCRIPTOR_BYTES 18u
#define EDID_DESCRIPTORS 4u
#define EDID_TAG_MONITOR_NAME 0xfcu
#define EDID_TEXT_AT 5u
#define EDID_TEXT_BYTES 13u
#define EDID_SYNC_DIGITAL_SEPARATE 0x18u

static bool header_is_edid(const uint8_t *b) {
    static const uint8_t magic[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    for (uint32_t i = 0; i < sizeof magic; i++) {
        if (b[i] != magic[i]) {
            return false;
        }
    }
    return true;
}

static bool checksum_is_zero(const uint8_t *b) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < EDID_BLOCK_BYTES; i++) {
        sum += b[i];
    }
    return (sum & 0xffu) == 0;
}

/* Three letters, five bits each, 1 = 'A', in a big-endian word. A value
 * outside 1..26 is not a letter and reads as '?'. */
static void read_vendor(const uint8_t *b, char out[4]) {
    uint32_t word = (uint32_t)b[EDID_VENDOR_AT] << 8 | b[EDID_VENDOR_AT + 1u];
    for (uint32_t i = 0; i < 3u; i++) {
        uint32_t letter = (word >> (10u - 5u * i)) & 0x1fu;
        out[i] = letter >= 1u && letter <= 26u ? (char)('A' + letter - 1u) : '?';
    }
    out[3] = '\0';
}

/* A monitor-name descriptor's text: up to 13 bytes, ended by a newline,
 * printable ASCII only (anything else becomes '?'). */
static void read_name(const uint8_t *d, char out[14]) {
    uint32_t n = 0;
    for (; n < EDID_TEXT_BYTES && d[EDID_TEXT_AT + n] != '\n'; n++) {
        uint8_t c = d[EDID_TEXT_AT + n];
        out[n] = c >= 0x20u && c < 0x7fu ? (char)c : '?';
    }
    while (n > 0 && out[n - 1u] == ' ') {
        n--;
    }
    out[n] = '\0';
}

/* A detailed timing descriptor: 12-bit sizes split across a low byte and a
 * shared nibble, sync offsets and widths from a 10/6-bit packing. */
static void read_detailed_timing(const uint8_t *d, struct display_timing *t) {
    uint32_t hactive = d[2] | (uint32_t)(d[4] & 0xf0u) << 4;
    uint32_t hblank = d[3] | (uint32_t)(d[4] & 0x0fu) << 8;
    uint32_t vactive = d[5] | (uint32_t)(d[7] & 0xf0u) << 4;
    uint32_t vblank = d[6] | (uint32_t)(d[7] & 0x0fu) << 8;
    uint32_t hoff = d[8] | (uint32_t)(d[11] & 0xc0u) << 2;
    uint32_t hwidth = d[9] | (uint32_t)(d[11] & 0x30u) << 4;
    uint32_t voff = (uint32_t)(d[10] >> 4) | (uint32_t)(d[11] & 0x0cu) << 2;
    uint32_t vwidth = (uint32_t)(d[10] & 0x0fu) | (uint32_t)(d[11] & 0x03u) << 4;
    bool separate = (d[17] & EDID_SYNC_DIGITAL_SEPARATE) == EDID_SYNC_DIGITAL_SEPARATE;
    t->pixel_khz = ((uint32_t)d[0] | (uint32_t)d[1] << 8) * 10u;
    t->hactive = (uint16_t)hactive;
    t->hsync_start = (uint16_t)(hactive + hoff);
    t->hsync_end = (uint16_t)(hactive + hoff + hwidth);
    t->htotal = (uint16_t)(hactive + hblank);
    t->vactive = (uint16_t)vactive;
    t->vsync_start = (uint16_t)(vactive + voff);
    t->vsync_end = (uint16_t)(vactive + voff + vwidth);
    t->vtotal = (uint16_t)(vactive + vblank);
    t->hsync_positive = separate && (d[17] & 0x02u) != 0;
    t->vsync_positive = separate && (d[17] & 0x04u) != 0;
    t->vic = 0;
}

/* One 18-byte descriptor: a timing when its pixel clock is non-zero (only
 * the first one is the preferred mode), otherwise a display descriptor
 * whose tag says what it holds. */
static void read_descriptor(const uint8_t *d, uint32_t index, struct edid_info *out) {
    bool timing = d[0] != 0 || d[1] != 0;
    if (timing && index == 0) {
        read_detailed_timing(d, &out->preferred);
    } else if (!timing && d[3] == EDID_TAG_MONITOR_NAME && out->name[0] == '\0') {
        read_name(d, out->name);
    }
}

int display_edid_parse(const uint8_t block[EDID_BLOCK_BYTES], struct edid_info *out) {
    if (block == NULL || out == NULL || !header_is_edid(block) || !checksum_is_zero(block)) {
        return KERR_INVAL;
    }
    *out = (struct edid_info){0};
    read_vendor(block, out->vendor);
    out->product = (uint16_t)(block[EDID_PRODUCT_AT] | block[EDID_PRODUCT_AT + 1u] << 8);
    for (uint32_t i = 0; i < EDID_DESCRIPTORS; i++) {
        read_descriptor(block + EDID_DESCRIPTORS_AT + i * EDID_DESCRIPTOR_BYTES, i, out);
    }
    return KERR_OK;
}
