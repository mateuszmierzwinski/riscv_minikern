#include "drivers/dc8200.h"

#include <stddef.h>

#include "arch/riscv/mmu.h"
#include "core/console.h"
#include "core/errno.h"

/* Everything up to the last register used (0x2518) in 32-bit words. */
#define DC_WINDOW_BYTES 0x3000u

/* Registers, as byte offsets from the controller's base, display/plane 0
 * (docs/DISPLAY.md §4). */
#define IRQ_ACK 0x0010u
#define IRQ_EN 0x0014u
#define IRQ_DISPLAY0_VSYNC 1u
#define CHIP_REV 0x0024u
#define FB_ADDRESS 0x1400u
#define FB_STRIDE 0x1408u
#define DITHER_CONFIG 0x1410u
#define PANEL_CONFIG 0x1418u
#define DISP_HSIZE 0x1430u
#define DISP_HSYNC 0x1438u
#define DISP_VSIZE 0x1440u
#define DISP_VSYNC 0x1448u
#define GENERAL_CONFIG 0x14b0u
#define DPI_CONFIG 0x14b8u
#define FB_CONFIG 0x1518u
#define FB_SIZE 0x1810u
#define CLK_GATING 0x1a28u
#define FB_CONFIG_EX 0x1cc0u
#define PANEL_START 0x1cccu
#define DP_CONFIG 0x1cd0u
#define FB_WATER_MARK 0x1ce8u
#define FB_TOP_LEFT 0x24d8u
#define FB_BOTTOM_RIGHT 0x24e0u
#define FB_BLEND_CONFIG 0x2510u
#define PANEL_CONFIG_EX 0x2518u

#define FORMAT_XRGB8888 (5u << 26)
#define FB_UNDERFLOW (1u << 5)
#define FB_EX_ENABLE (1u << 13)
#define FB_EX_COMMIT (1u << 12)
#define BLEND_DISABLE 0x2u
#define DPI_RGB888 5u
/* bits 2:0 = RGB888 (2), bit 3 = the DP/TMDS output enabled. StarFive's
 * U-Boot writes 0xd2 instead; this is its Linux driver's value (§4.4). */
#define DP_RGB888_ENABLED 0x0au
/* DE, data and clock enabled, and bit 12: running. */
#define PANEL_RUNNING 0x1111u
#define PANEL_COMMIT_PENDING 1u
#define DISPLAY0_RUNNING 1u

static volatile uint32_t *regs;

static void put(uint32_t offset, uint32_t value) {
    regs[offset / 4u] = value;
}

static uint32_t get(uint32_t offset) {
    return regs[offset / 4u];
}

/* Display 0's cursor block (mainline `vs_cursor_plane_regs.h`; display 1's
 * is 0x1080 above). CONFIG: bits 1:0 format (2 = ARGB8888), bit 2 image
 * update, bit 3 commit, bits 7:5 size (0 = 32). LOCATION: x in 14:0, y in
 * 30:16. */
#define CURSOR_CONFIG 0x1468u
#define CURSOR_ADDRESS 0x146cu
#define CURSOR_LOCATION 0x1470u
#define CURSOR_ARGB8888 0x2u
#define CURSOR_IMAGE_UPDATE (1u << 2)
#define CURSOR_COMMIT (1u << 3)
#define CURSOR_SIZE_32 (0u << 5)
#define CURSOR_POSITION_MAX 0x7fffu

int dc8200_cursor_show(phys_t image, uint32_t x, uint32_t y) {
    if (regs == NULL) {
        return KERR_NODEV;
    }
    if (image >= ((phys_t)1 << 32) || x > CURSOR_POSITION_MAX || y > CURSOR_POSITION_MAX) {
        return KERR_RANGE;
    }
    put(CURSOR_ADDRESS, (uint32_t)image);
    put(CURSOR_LOCATION, x | (y << 16));
    put(CURSOR_CONFIG, CURSOR_ARGB8888 | CURSOR_SIZE_32 | CURSOR_IMAGE_UPDATE | CURSOR_COMMIT);
    return KERR_OK;
}

void dc8200_cursor_read(uint32_t *config, uint32_t *address, uint32_t *location) {
    *config = regs != NULL ? get(CURSOR_CONFIG) : 0u;
    *address = regs != NULL ? get(CURSOR_ADDRESS) : 0u;
    *location = regs != NULL ? get(CURSOR_LOCATION) : 0u;
}

/* Overlay 0's block (StarFive's vendor `vs_dc_hw.h`; overlay n adds 4n).
 * CONFIG: bit 24 enable, bits 20:16 format (6 = ARGB8888), bit 31 commit.
 * CONFIG_EX: bits 2:0 the layer's z position, bit 3 the display. SIZE and
 * the corners: x in 14:0, y in 29:15, the bottom-right exclusive. */
#define OVL_CONFIG 0x1540u
#define OVL_BLEND_CONFIG 0x1580u
#define OVL_ADDRESS 0x15c0u
#define OVL_STRIDE 0x1600u
#define OVL_TOP_LEFT 0x1640u
#define OVL_BOTTOM_RIGHT 0x1680u
#define OVL_SRC_GLOBAL 0x16c0u
#define OVL_DST_GLOBAL 0x1700u
#define OVL_SIZE 0x17c0u
#define OVL_U_ADDRESS 0x1840u
#define OVL_V_ADDRESS 0x1880u
#define OVL_FACTOR_X 0x1a40u
#define OVL_FACTOR_Y 0x1a80u
#define OVL_H_COEF_INDEX 0x1ac0u
#define OVL_H_COEF_DATA 0x1b00u
#define OVL_V_COEF_INDEX 0x1b40u
#define OVL_V_COEF_DATA 0x1b80u
#define OVL_INIT_OFFSET 0x1bc0u
#define OVL_SCALE_CONFIG 0x1c00u
#define OVL_WATER_MARK 0x1dc0u
#define OVL_CONFIG_EX 0x2540u
#define OVL_ENABLE (1u << 24)
#define OVL_COMMIT (1u << 31)
#define OVL_FORMAT_ARGB8888 (6u << 16)
#define OVL_Z_ABOVE_PRIMARY 1u
#define OVL_SCALE_ON (1u << 8)
#define OVL_SCALE_TAPS 0x33u        /* what the vendor driver loads with its filter */
#define OVL_INIT_HALF 0x80008000u   /* the first sample half a pixel in, both ways */
#define OVL_BLEND_OPAQUE 0x3548u    /* the vendor's "pixel alpha none" */
#define OVL_ALPHA_FULL (0xffu << 24)
#define PLANE_XY_MAX 0x7fffu
#define DEBUG_COUNTER_SELECT 0x14d0u
#define DEBUG_COUNTER_VALUE 0x14d8u

/* Both planes' commit bits clear and the panel's update pending, so a set
 * of writes lands in one frame — or the reverse, which releases them
 * (the vendor's shadow-register bracket, as `dc8200_scanout` uses). */
static void hold_updates(bool hold) {
    if (hold) {
        put(PANEL_CONFIG_EX, PANEL_COMMIT_PENDING);
        put(FB_CONFIG_EX, get(FB_CONFIG_EX) & ~FB_EX_COMMIT);
        put(OVL_CONFIG, get(OVL_CONFIG) & ~OVL_COMMIT);
        return;
    }
    put(FB_CONFIG_EX, get(FB_CONFIG_EX) | FB_EX_COMMIT);
    put(OVL_CONFIG, get(OVL_CONFIG) | OVL_COMMIT);
    put(PANEL_CONFIG_EX, 0u);
}

/* The scaler: our linear filter in both directions, the factors, and the
 * enable bit when either side changes size. */
static void program_the_scaler(const struct dc8200_overlay *o) {
    static uint32_t kernel[SCALER_KERNEL_WORDS];
    bool scaled = o->out_width != o->width || o->out_height != o->height;
    display_scaler_kernel(kernel);
    put(OVL_SCALE_CONFIG, OVL_SCALE_TAPS);
    put(OVL_INIT_OFFSET, OVL_INIT_HALF);
    put(OVL_H_COEF_INDEX, 0u);
    for (uint32_t i = 0; i < SCALER_KERNEL_WORDS; i++) {
        put(OVL_H_COEF_DATA, kernel[i]);
    }
    put(OVL_V_COEF_INDEX, 0u);
    for (uint32_t i = 0; i < SCALER_KERNEL_WORDS; i++) {
        put(OVL_V_COEF_DATA, kernel[i]);
    }
    put(OVL_FACTOR_X, display_scale_factor(o->width, o->out_width));
    put(OVL_FACTOR_Y, display_scale_factor(o->height, o->out_height));
    put(OVL_SCALE_CONFIG, OVL_SCALE_TAPS | (scaled ? OVL_SCALE_ON : 0u));
}

static bool overlay_fits(const struct dc8200_overlay *o) {
    return (o->image >> 32) == 0 && o->width != 0 && o->height != 0 && o->out_width != 0 &&
           o->out_height != 0 && o->x + o->out_width <= PLANE_XY_MAX && o->y + o->out_height <= PLANE_XY_MAX &&
           o->stride >= o->width * 4u;
}

int dc8200_overlay_show(const struct dc8200_overlay *o) {
    if (regs == NULL) {
        return KERR_NODEV;
    }
    if (!overlay_fits(o)) {
        return KERR_RANGE;
    }
    hold_updates(true);
    put(OVL_ADDRESS, (uint32_t)o->image);
    put(OVL_U_ADDRESS, 0u);
    put(OVL_V_ADDRESS, 0u);
    put(OVL_STRIDE, o->stride);
    put(OVL_SIZE, o->width | (o->height << 15));
    put(OVL_WATER_MARK, 0u);
    program_the_scaler(o);
    put(OVL_TOP_LEFT, o->x | (o->y << 15));
    put(OVL_BOTTOM_RIGHT, (o->x + o->out_width) | ((o->y + o->out_height) << 15));
    put(OVL_SRC_GLOBAL, OVL_ALPHA_FULL);
    put(OVL_DST_GLOBAL, OVL_ALPHA_FULL);
    put(OVL_BLEND_CONFIG, OVL_BLEND_OPAQUE);
    put(OVL_CONFIG_EX, OVL_Z_ABOVE_PRIMARY); /* display 0 */
    put(OVL_CONFIG, OVL_ENABLE | OVL_FORMAT_ARGB8888);
    hold_updates(false);
    return KERR_OK;
}

void dc8200_overlay_hide(void) {
    if (regs != NULL) {
        hold_updates(true);
        put(OVL_CONFIG, 0u);
        hold_updates(false);
    }
}

void dc8200_overlay_read(uint32_t out[DC8200_OVERLAY_READBACK]) {
    static const uint32_t at[DC8200_OVERLAY_READBACK] = {OVL_CONFIG,     OVL_ADDRESS,      OVL_SIZE,
                                                         OVL_TOP_LEFT,   OVL_BOTTOM_RIGHT, OVL_SCALE_CONFIG,
                                                         OVL_FACTOR_X,   OVL_FACTOR_Y};
    for (uint32_t i = 0; i < DC8200_OVERLAY_READBACK; i++) {
        out[i] = regs != NULL ? get(at[i]) : 0u;
    }
}

uint32_t dc8200_debug_counter(uint32_t select) {
    if (regs == NULL) {
        return 0u;
    }
    put(DEBUG_COUNTER_SELECT, select);
    return get(DEBUG_COUNTER_VALUE);
}

int dc8200_init(phys_t base) {
    void *at = mmu_map_device(base, DC_WINDOW_BYTES);
    if (at == NULL) {
        return KERR_NOMEM;
    }
    regs = at;
    return KERR_OK;
}

uint32_t dc8200_revision(void) {
    return regs == NULL ? 0u : get(CHIP_REV);
}

/* The model and customer-id registers (mainline `vs_hwdb.c`: model 0x20,
 * customer 0x30): the model says this is a DC8200 at all, the customer id
 * tells StarFive's configuration from others of the same revision. */
#define CHIP_MODEL 0x0020u
#define CHIP_CUSTOMER 0x0030u

uint32_t dc8200_model(void) {
    return regs == NULL ? 0u : get(CHIP_MODEL);
}

uint32_t dc8200_customer(void) {
    return regs == NULL ? 0u : get(CHIP_CUSTOMER);
}

/* The primary plane: where, how wide a line, what format, how big — and
 * the commit bit that makes the controller take it (StarFive's order:
 * enabled with the commit clear, written, then committed). */
static void program_the_plane(phys_t fb, uint32_t stride, uint32_t size) {
    put(FB_CONFIG_EX, FB_EX_ENABLE);
    put(FB_TOP_LEFT, 0u);
    put(FB_BOTTOM_RIGHT, size);
    put(FB_SIZE, size);
    put(FB_ADDRESS, (uint32_t)fb);
    put(FB_STRIDE, stride);
    put(FB_WATER_MARK, 0u);
    put(FB_BLEND_CONFIG, BLEND_DISABLE);
    put(FB_CONFIG, FORMAT_XRGB8888);
    put(FB_CONFIG_EX, FB_EX_ENABLE | FB_EX_COMMIT);
}

/* Display 0's timing and output format, then the panel switched on. */
static void program_the_display(const struct dc_timing_words *w) {
    put(CLK_GATING, 0u);
    put(DISP_HSIZE, w->hsize);
    put(DISP_HSYNC, w->hsync);
    put(DISP_VSIZE, w->vsize);
    put(DISP_VSYNC, w->vsync);
    put(GENERAL_CONFIG, 0u);
    put(DP_CONFIG, DP_RGB888_ENABLED);
    put(DPI_CONFIG, DPI_RGB888);
    put(PANEL_CONFIG, PANEL_RUNNING);
    put(DITHER_CONFIG, 0u);
}

int dc8200_scanout(phys_t fb, uint32_t stride, const struct display_timing *t) {
    struct dc_timing_words w;
    if (regs == NULL) {
        return KERR_NODEV;
    }
    if ((fb >> 32) != 0) {
        return KERR_RANGE;
    }
    display_dc_timing(t, &w);
    put(PANEL_CONFIG_EX, PANEL_COMMIT_PENDING);
    program_the_plane(fb, stride, w.plane_size);
    program_the_display(&w);
    put(PANEL_CONFIG_EX, 0u); /* the panel's settings take effect */
    put(PANEL_START, DISPLAY0_RUNNING);
    return KERR_OK;
}

bool dc8200_underflow(void) {
    return regs != NULL && (get(FB_CONFIG) & FB_UNDERFLOW) != 0;
}

void dc8200_report(void) {
    if (regs == NULL) {
        return;
    }
    kprintf("dc8200: fb %08x stride %u config %08x ex %08x size %08x\n", get(FB_ADDRESS),
            get(FB_STRIDE), get(FB_CONFIG), get(FB_CONFIG_EX), get(FB_SIZE));
    kprintf("dc8200: hsize %08x hsync %08x vsize %08x vsync %08x\n", get(DISP_HSIZE),
            get(DISP_HSYNC), get(DISP_VSIZE), get(DISP_VSYNC));
    kprintf("dc8200: panel %08x panel_ex %08x start %08x dp %08x dpi %08x irq %08x\n",
            get(PANEL_CONFIG), get(PANEL_CONFIG_EX), get(PANEL_START), get(DP_CONFIG),
            get(DPI_CONFIG), get(IRQ_ACK));
}

void dc8200_arm_vsync(void) {
    if (regs != NULL) {
        put(IRQ_EN, get(IRQ_EN) | IRQ_DISPLAY0_VSYNC);
        (void)get(IRQ_ACK); /* reading acknowledges: start from clear */
    }
}

bool dc8200_vsync_seen(void) {
    return regs != NULL && (get(IRQ_ACK) & IRQ_DISPLAY0_VSYNC) != 0;
}
