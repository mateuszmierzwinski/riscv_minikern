/* The Verisilicon DC8200 display controller on the JH7110 (`0x2940_0000`).
 *
 * It reads a framebuffer from memory and turns it into a timed pixel
 * stream; on this board its display 0 feeds the HDMI transmitter. One
 * primary plane is all a framebuffer needs: an address, a stride, a format
 * and a size, plus the display's timing. Beyond it — the chip's real 2D
 * acceleration — the hardware cursor and overlay 0 with its scaler, both
 * off unless asked for (docs/DISPLAY.md §4.7).
 *
 * The register values follow StarFive's U-Boot write sequence, the one
 * validated on this SoC, with the timing words computed by
 * `display_dc_timing` and checked against that sequence's 1080p constants.
 */
#ifndef MINIKERN_DRIVERS_DC8200_H
#define MINIKERN_DRIVERS_DC8200_H

#include <stdbool.h>
#include <stdint.h>

#include "core/display_mode.h"
#include "core/types.h"

/* What a powered, clocked controller reports as its revision. */
#define DC8200_REVISION 0x5720u

/* Maps the controller at `base` (no tree node on this board's firmware). */
int dc8200_init(phys_t base);

/* The revision register: DC8200_REVISION when the VOUT domain is on and the
 * controller's clocks and resets are released, 0 before. */
uint32_t dc8200_revision(void);

/* The model register — 0x8200 for this controller — and the customer id. */
#define DC8200_MODEL 0x8200u
uint32_t dc8200_model(void);
uint32_t dc8200_customer(void);

/* The hardware cursor: a 32×32 ARGB8888 image at physical `image` (below
 * 4 GiB), its hot spot at (`x`, `y`) — the first of the controller's
 * planes this driver uses. */
int dc8200_cursor_show(phys_t image, uint32_t x, uint32_t y);

/* The cursor registers as the controller holds them. */
void dc8200_cursor_read(uint32_t *config, uint32_t *address, uint32_t *location);

/* Overlay 0 — one of the two overlays with a scaler (overlays 1 and 3 have
 * none): an ARGB8888 image of `width`×`height` at physical `image` (below
 * 4 GiB, `stride` bytes a line), shown at (`x`, `y`) as `out_width`×
 * `out_height` on display 0, above the primary plane and opaque. A size
 * that differs from the image's turns the scaler on, with a linear filter
 * (`display_scaler_kernel`). Registers and bit layouts from StarFive's
 * vendor driver (docs/DISPLAY.md §4.6); no monitor has seen one yet. */
struct dc8200_overlay {
    phys_t image;
    uint32_t stride, width, height;
    uint32_t x, y, out_width, out_height;
};
int dc8200_overlay_show(const struct dc8200_overlay *o);

/* Takes overlay 0 off the screen. */
void dc8200_overlay_hide(void);

/* Overlay 0's registers as the controller holds them, for the log: config,
 * address, size, top-left, bottom-right, scale config, x and y factors. */
#define DC8200_OVERLAY_READBACK 8u
void dc8200_overlay_read(uint32_t out[DC8200_OVERLAY_READBACK]);

/* The controller's debug counter `select` reads (`0x14D0`/`0x14D8`), which
 * no driver uses and nothing documents: read here to find, by measurement,
 * one that counts what the planes fetch. */
uint32_t dc8200_debug_counter(uint32_t select);

/* What three of them count, **measured on the board** (2026-11-01,
 * docs/DISPLAY.md §4.6): the pixels display 0 sends (2,073,600 a 1080p
 * frame), and the 128-byte read bursts the primary plane and overlay 0
 * fetch (64,800 a frame for a 1080p XRGB primary; for overlay 0 its
 * *source* lines' bursts, whatever size it is scaled to — 128 for 64×64).
 * Selects 2 and 9 count 16-byte beats, eight to a burst; 7 and 8, and 9
 * and 10, read alike; 20 is non-zero only while the scaler runs. */
#define DC8200_COUNT_PRIMARY_BURSTS 0u
#define DC8200_COUNT_OUTPUT_PIXELS 4u
#define DC8200_COUNT_OVERLAY0_BURSTS 7u
#define DC8200_BURST_BYTES 128u

/* Scans out `stride`-byte lines of XRGB8888 at physical `fb` on display 0
 * with timing `t`. The controller takes a 32-bit address, so `fb` must lie
 * below 4 GiB: KERR_RANGE otherwise. */
int dc8200_scanout(phys_t fb, uint32_t stride, const struct display_timing *t);

/* The registers that decide whether display 0 runs, for the log when it
 * does not: plane config and its extension, the panel's config, start and
 * commit, the output format, and the top block's interrupt status. */
void dc8200_report(void);

/* Arms display 0's vsync interrupt flag, and whether it has been raised
 * since (read and cleared): a frame boundary passing, which proves the
 * controller runs independently of the scan-position register. */
void dc8200_arm_vsync(void);
bool dc8200_vsync_seen(void);

/* Whether the primary plane has run dry — the controller could not fetch
 * pixels in time. */
bool dc8200_underflow(void);

#endif
