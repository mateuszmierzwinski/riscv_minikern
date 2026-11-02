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
/* The hardware cursor's image: 32×32 ARGB8888, the smallest size the
 * DC8200 takes (DISPLAY.md, "Mainline cross-check"). An arrow — white with
 * a black outline, transparent around it — pointing up and left from its
 * hot spot at (0, 0). */
#define CURSOR_SIDE 32u
#define CURSOR_PIXELS (CURSOR_SIDE * CURSOR_SIDE)
void display_cursor_arrow(uint32_t out[CURSOR_PIXELS]);

/* ── the overlay scaler (DC8200, docs/DISPLAY.md §4.6) ─────────────────── */

/* A scale factor as the controller takes it: 16.16 fixed point, the step
 * through the source per output pixel, with both ends landing on the end
 * pixels — (src − 1) / (dst − 1). 1.0 when either side is one pixel. */
uint32_t display_scale_factor(uint32_t src, uint32_t dst);

/* The scaler's filter: 17 phases of 9 taps, each a Q14 weight (a phase sums
 * to 16384), packed two to a word, low half first — the layout of the
 * coefficient table StarFive's driver loads. Ours is **linear
 * interpolation** between the two taps either side of the sample (taps 3
 * and 4): phase 0 halfway between them, phase 16 on tap 4. The table is
 * SCALER_KERNEL_WORDS long, phases repeating from 0 after the 17th. */
#define SCALER_TAPS 9u
#define SCALER_PHASES 17u
#define SCALER_ONE 16384u
#define SCALER_KERNEL_WORDS 128u
void display_scaler_kernel(uint32_t out[SCALER_KERNEL_WORDS]);

/* A test image for an overlay: `side`×`side` ARGB8888, opaque, in four
 * quadrants (red, green, blue, white), so a scaled or misplaced copy is
 * told apart from a right one by eye. */
void display_overlay_pattern(uint32_t *out, uint32_t side);

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

/* The CEA mode to drive a monitor at: its preferred timing's size, when the
 * transmitter has settings for that size and the monitor's pixel clock is
 * the mode's to within 1 % (59.94 and 60 Hz both). NULL otherwise — a
 * 1366x768 panel, or a preferred timing at another rate, gets no guess.
 * Both modes' lines (7,680 and 5,120 bytes) are multiples of the 128 the
 * controller wants a stride to be (DISPLAY.md, "Mainline cross-check"). */
const struct display_timing *display_mode_for_monitor(const struct edid_info *e);

#endif
