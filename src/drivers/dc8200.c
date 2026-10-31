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
