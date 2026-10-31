/* display_mode — video timings, and what the display hardware is told about
 * them.
 *
 * Pure, so it is host-tested: the timing of a CEA-861 mode, the values the
 * DC8200's timing registers and the Innosilicon HDMI transmitter's timing
 * bytes take for it, the AVI infoframe, and a reader for a monitor's EDID.
 * The register encodings are the drivers' business in what they *mean*; how
 * a mode turns into numbers is arithmetic, and arithmetic is checked here
 * against the values working bring-ups on this SoC wrote (docs/DISPLAY.md).
 *
 * The EDID comes from the monitor over the DDC wires, so its reader is an
 * untrusted-input parser and is total: any 128 bytes give an answer or a
 * refusal, never a read outside the block.
 */
#ifndef MINIKERN_CORE_DISPLAY_MODE_H
#define MINIKERN_CORE_DISPLAY_MODE_H

#include <stdbool.h>
#include <stdint.h>

struct display_timing {
    uint32_t pixel_khz;
    uint16_t hactive;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t vactive;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    bool hsync_positive;
    bool vsync_positive;
    uint8_t vic; /* CEA-861 video identification code */
};

/* The CEA mode with this visible size at 60 Hz — 1920x1080 (VIC 16) or
 * 1280x720 (VIC 4) — or NULL. Only modes whose transmitter settings are
 * known for this SoC are listed. */
const struct display_timing *display_mode_cea(uint32_t width, uint32_t height);

/* The DC8200's display timing words for a mode: `DISP_HSIZE`/`DISP_VSIZE`
 * (active | total << 16), `DISP_HSYNC`/`DISP_VSYNC` (start | end << 15,
 * bit 30 enable, bit 31 negative polarity), and a plane's size (width |
 * height << 15). */
struct dc_timing_words {
    uint32_t hsize;
    uint32_t hsync;
    uint32_t vsize;
    uint32_t vsync;
    uint32_t plane_size;
};

void display_dc_timing(const struct display_timing *t, struct dc_timing_words *out);

/* The HDMI transmitter's timing bytes, registers 0x08 to 0x15 in order:
 * polarity/external-timing control, htotal, hblank, hdelay, hsync width
 * (each low byte then high), vtotal (two bytes), vblank, vdelay, vsync
 * width. */
#define HDMI_TIMING_BYTES 14u
void display_hdmi_timing(const struct display_timing *t, uint8_t out[HDMI_TIMING_BYTES]);

/* The AVI infoframe for an RGB picture of `vic`, as the transmitter's packet
 * buffer takes it: three header bytes, the checksum, then the payload.
 * The checksum makes all 17 bytes sum to zero, as CEA-861 requires. */
#define AVI_INFOFRAME_BYTES 17u
void display_avi_infoframe(uint8_t vic, uint8_t out[AVI_INFOFRAME_BYTES]);

/* What a monitor says about itself in the first 128 bytes of its EDID. */
#define EDID_BLOCK_BYTES 128u

struct edid_info {
    char vendor[4];            /* three letters, PNP id */
    uint16_t product;
    char name[14];             /* the monitor-name descriptor, "" if none */
    struct display_timing preferred; /* the first detailed timing; pixel_khz 0 if none */
};

/* Reads an EDID base block. KERR_INVAL for anything that is not one — a
 * wrong header or a checksum that does not sum to zero — which is also what
 * an unplugged or unpowered DDC line reads as (all 0xff or all zero). */
int display_edid_parse(const uint8_t block[EDID_BLOCK_BYTES], struct edid_info *out);

#endif
