#include "drivers/display_jh7110.h"

#include <stdbool.h>
#include <stddef.h>

#include "arch/riscv/timer.h"
#include "core/console.h"
#include "core/display_mode.h"
#include "core/errno.h"
#include "drivers/ccache_jh7110.h"
#include "drivers/crg_jh7110.h"
#include "drivers/dc8200.h"
#include "drivers/gpio_jh7110.h"
#include "drivers/i2c_dw.h"
#include "drivers/inno_hdmi.h"
#include "drivers/mmio_poll.h"
#include "drivers/pmu_jh7110.h"

/* Fixed addresses: the tree this firmware passes has no display nodes
 * (docs/DISPLAY.md §0.2), so these are the TRM's memory map. */
#define DC8200_BASE 0x29400000ull
#define HDMI_BASE 0x29590000ull

/* System clocks feeding VOUT, and their resets (syscrg ids, §2.2). */
#define SYSCLK_VOUT_SRC 58u
#define SYSCLK_NOC_BUS_DISP_AXI 60u
#define SYSCLK_VOUT_TOP_AHB 61u
#define SYSCLK_VOUT_TOP_AXI 62u
#define SYSCLK_VOUT_TOP_HDMI_MCLK 63u
#define SYSRST_NOC_BUS_DISP_AXI 26u
#define SYSRST_VOUT_TOP_SRC 43u

/* The VOUT clock generator's own (voutcrg ids, §2.3). */
#define VOUTCLK_DC8200_PIX 1u
#define VOUTCLK_DC8200_AXI 4u
#define VOUTCLK_DC8200_CORE 5u
#define VOUTCLK_DC8200_AHB 6u
#define VOUTCLK_DC8200_PIX0 7u
#define VOUTCLK_DC8200_PIX1 8u
#define VOUTCLK_TOP_LCD 9u
#define VOUTCLK_HDMI_MCLK 15u
#define VOUTCLK_HDMI_BCLK 16u
#define VOUTCLK_HDMI_SYS 17u
#define VOUTRST_DC8200_AXI 0u
#define VOUTRST_DC8200_AHB 1u
#define VOUTRST_DC8200_CORE 2u
#define VOUTRST_HDMI_TX 9u

/* The pixel clock: PLL2 (1188 MHz, set by the firmware) divided down —
 * 8 gives 148.5 MHz, 16 gives 74.25 (§2.4). Mux 0 = this divider, not the
 * transmitter's PHY clock. */
#define PLL2_KHZ 1188000u
#define PIX0_FROM_DIVIDER 0u

/* The HDMI pins (sys GPIO, §1.7): DDC clock and data are the
 * transmitter's own I²C master, hot-plug an input. */
#define PIN_DDC_SCL 0u
#define PIN_DDC_SDA 1u
#define PIN_HPD 15u
#define SIG_OUT_DDC_SCL 11u
#define SIG_OUT_DDC_SDA 12u
#define SIG_OEN_DDC_SCL 3u
#define SIG_OEN_DDC_SDA 4u
#define SIG_IN_DDC_SCL 6u
#define SIG_IN_DDC_SDA 7u
#define SIG_IN_HPD 8u

/* The PMIC's output-enable register and the HDMI rails' bits (§1.3). */
#define PMIC_BUS "/soc/i2c@12050000"
#define PMIC_ADDR 0x36u
#define PMIC_OUTPUT_ENABLE 0x11u
#define PMIC_ALDO3_1V8 (1u << 2)
#define PMIC_ALDO5_0V9 (1u << 4)

/* The scanout, once it runs. Written by the bring-up before the refresh
 * hook is registered, and by that hook only after. */
struct scanout {
    bool up;
    phys_t fb;
    uint64_t bytes;
    uint64_t refreshes;
    uint64_t last_refresh_us;
};

static struct scanout display;

/* Step 1: what the board says before anything is touched. */
static void report_the_starting_state(const struct fdt *fdt) {
    struct i2c_dw bus;
    uint8_t enables = 0;
    bool hpd = false;
    (void)gpio_jh7110_set_input(0, PIN_HPD);
    (void)gpio_jh7110_read(0, PIN_HPD, &hpd);
    kprintf("display: hot-plug line (GPIO %u) %s\n", PIN_HPD,
            hpd ? "high: a monitor is connected" : "low: no monitor seen");
    if (i2c_dw_init(&bus, fdt, PMIC_BUS) != KERR_OK ||
        i2c_dw_read_u8(&bus, PMIC_ADDR, PMIC_OUTPUT_ENABLE, &enables) != KERR_OK) {
        kprintf("display: PMIC output enables could not be read\n");
        return;
    }
    kprintf("display: PMIC 0x11 = %02x: HDMI 1.8 V rail %s, 0.9 V rail %s\n", enables,
            (enables & PMIC_ALDO3_1V8) != 0 ? "on" : "OFF",
            (enables & PMIC_ALDO5_0V9) != 0 ? "on" : "OFF");
}

/* Step 2: DDC clock and data driven by the transmitter's master and read
 * back by it; hot-plug routed to it as an input. */
static int give_the_pins_to_hdmi(void) {
    int rc = gpio_jh7110_set_function(0, PIN_DDC_SCL, SIG_OUT_DDC_SCL, SIG_OEN_DDC_SCL);
    rc = rc != KERR_OK ? rc : gpio_jh7110_set_function(0, PIN_DDC_SDA, SIG_OUT_DDC_SDA, SIG_OEN_DDC_SDA);
    rc = rc != KERR_OK ? rc : gpio_jh7110_route_input(0, SIG_IN_DDC_SCL, PIN_DDC_SCL);
    rc = rc != KERR_OK ? rc : gpio_jh7110_route_input(0, SIG_IN_DDC_SDA, PIN_DDC_SDA);
    return rc != KERR_OK ? rc : gpio_jh7110_route_input(0, SIG_IN_HPD, PIN_HPD);
}

/* Step 3. */
static int power_the_domain(const struct fdt *fdt) {
    uint32_t why = 0;
    int rc = pmu_jh7110_init(fdt);
    rc = rc != KERR_OK ? rc : pmu_jh7110_power_on(PMU_DOMAIN_VOUT, &why);
    kprintf("display: VOUT power domain %s (domains on %02x, status %08x)\n",
            rc == KERR_OK ? "on" : "did NOT come on", pmu_jh7110_domains_on(), why);
    return rc;
}

/* Enables each clock of a list, stopping at the first that will not. */
static int enable_clocks(enum crg_domain d, const uint32_t *ids, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        int rc = crg_enable_clock(d, ids[i]);
        if (rc != KERR_OK) {
            kprintf("display: clock %u of generator %u would not enable (rc %d)\n", ids[i],
                    (uint32_t)d, rc);
            return rc;
        }
    }
    return KERR_OK;
}

static int release_resets(enum crg_domain d, const uint32_t *ids, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        int rc = crg_release_reset(d, ids[i]);
        if (rc != KERR_OK) {
            kprintf("display: reset %u of generator %u would not release (rc %d)\n", ids[i],
                    (uint32_t)d, rc);
            return rc;
        }
    }
    return KERR_OK;
}

/* Step 4a: the system side. Each clock before its reset — a reset released
 * under a gated clock can hang for ever. */
static int clock_the_system_side(void) {
    static const uint32_t noc[] = {SYSCLK_NOC_BUS_DISP_AXI};
    static const uint32_t noc_reset[] = {SYSRST_NOC_BUS_DISP_AXI};
    static const uint32_t top[] = {SYSCLK_VOUT_SRC, SYSCLK_VOUT_TOP_AXI, SYSCLK_VOUT_TOP_AHB,
                                   SYSCLK_VOUT_TOP_HDMI_MCLK};
    static const uint32_t top_reset[] = {SYSRST_VOUT_TOP_SRC};
    int rc = enable_clocks(CRG_SYS, noc, 1u);
    rc = rc != KERR_OK ? rc : release_resets(CRG_SYS, noc_reset, 1u);
    rc = rc != KERR_OK ? rc : enable_clocks(CRG_SYS, top, sizeof top / sizeof top[0]);
    return rc != KERR_OK ? rc : release_resets(CRG_SYS, top_reset, 1u);
}

/* Step 4b: inside VOUT — the pixel clock divided for the mode, the
 * controller's and transmitter's clocks, then their resets. */
static int clock_the_display_side(uint32_t pixel_khz) {
    static const uint32_t clocks[] = {VOUTCLK_DC8200_AXI, VOUTCLK_DC8200_CORE, VOUTCLK_DC8200_AHB,
                                      VOUTCLK_DC8200_PIX1, VOUTCLK_TOP_LCD, VOUTCLK_HDMI_MCLK,
                                      VOUTCLK_HDMI_BCLK, VOUTCLK_HDMI_SYS};
    static const uint32_t resets[] = {VOUTRST_DC8200_AXI, VOUTRST_DC8200_AHB, VOUTRST_DC8200_CORE,
                                      VOUTRST_HDMI_TX};
    int rc = crg_set_divider(CRG_VOUT, VOUTCLK_DC8200_PIX, PLL2_KHZ / pixel_khz);
    rc = rc != KERR_OK ? rc : crg_select_parent(CRG_VOUT, VOUTCLK_DC8200_PIX0, PIX0_FROM_DIVIDER);
    rc = rc != KERR_OK ? rc : crg_enable_clock(CRG_VOUT, VOUTCLK_DC8200_PIX0);
    rc = rc != KERR_OK ? rc : enable_clocks(CRG_VOUT, clocks, sizeof clocks / sizeof clocks[0]);
    return rc != KERR_OK ? rc : release_resets(CRG_VOUT, resets, sizeof resets / sizeof resets[0]);
}

/* Step 4's evidence: a controller that is powered, clocked and out of
 * reset answers with its revision. */
static int check_the_controller_answers(void) {
    uint32_t rev;
    int rc = dc8200_init(DC8200_BASE);
    if (rc != KERR_OK) {
        return rc;
    }
    rev = dc8200_revision();
    kprintf("display: DC8200 revision %04x (%s)\n", rev,
            rev == DC8200_REVISION ? "powered and clocked" : "NOT the expected 5720");
    return rev == DC8200_REVISION ? KERR_OK : KERR_NODEV;
}

/* The VOUT generator's pixel-clock registers, as the controller is
 * started: the divider, pix0's gate and mux, and pix1 and the LCD clock. */
static void clock_report(void) {
    volatile uint32_t *v = crg_regs(CRG_VOUT);
    if (v != NULL) {
        kprintf("display: voutcrg pix %08x pix0 %08x pix1 %08x lcd %08x resets %08x/%08x\n",
                v[VOUTCLK_DC8200_PIX], v[VOUTCLK_DC8200_PIX0], v[VOUTCLK_DC8200_PIX1],
                v[VOUTCLK_TOP_LCD], v[0x48u / 4u], v[0x4cu / 4u]);
    }
}

/* Step 5's EDID: who the monitor says it is, and what it prefers. A
 * failure is reported, not fatal — a monitor without DDC still shows. */
static void report_the_monitor(void) {
    uint8_t block[EDID_BLOCK_BYTES];
    struct edid_info e;
    int rc = inno_hdmi_read_edid(0, block);
    if (rc != KERR_OK || display_edid_parse(block, &e) != KERR_OK) {
        kprintf("display: EDID %s (rc %d; first bytes %02x %02x %02x)\n",
                rc == KERR_OK ? "is not valid" : "could not be read", rc, block[0], block[1],
                block[2]);
        return;
    }
    kprintf("display: monitor %s %04x \"%s\", prefers %ux%u at %u kHz\n", e.vendor, e.product,
            e.name, e.preferred.hactive, e.preferred.vactive, e.preferred.pixel_khz);
}

/* Step 5. */
static int start_the_transmitter(const struct display_timing *t) {
    const char *why = "";
    int rc = inno_hdmi_init(HDMI_BASE);
    kprintf("display: transmitter sees hot-plug %s\n", inno_hdmi_hotplug() ? "high" : "low");
    report_the_monitor();
    rc = rc != KERR_OK ? rc : inno_hdmi_set_mode(t, &why);
    kprintf("display: HDMI %ux%u, PLLs %s%s\n", t->hactive, t->vactive,
            rc == KERR_OK ? "locked, TMDS on" : "NOT locked: ", why);
    return rc;
}

/* Frames counted over FRAME_COUNT_NS by the vsync flag: ~30 at 60 Hz. */
#define FRAME_COUNT_NS 500000000ull

static uint32_t count_frames(void) {
    uint32_t frames = 0;
    uint64_t until = timer_ns() + FRAME_COUNT_NS;
    dc8200_arm_vsync();
    while (timer_ns() < until) {
        frames += dc8200_vsync_seen() ? 1u : 0u;
    }
    return frames;
}

/* Step 6's evidence: **frames pass at the mode's rate**, counted by the
 * controller's own vsync flag, and the plane keeps up. (The scan-position
 * register reads 0 on this revision whether it runs or not, so it proves
 * nothing — measured 2026-10-31.) A controller that stands still logs its
 * registers. */
static int check_the_scanout_runs(const struct display_timing *t) {
    uint32_t frames = count_frames();
    uint32_t want = (uint32_t)((uint64_t)t->pixel_khz * 1000u / ((uint32_t)t->htotal * t->vtotal) / 2u);
    bool running = frames + 2u >= want && frames <= want + 2u;
    kprintf("display: %u frames in 0.5 s (%u expected), underflow %s — %s\n", frames, want,
            dc8200_underflow() ? "YES" : "no", running ? "scanning out" : "NOT at the mode's rate");
    if (!running) {
        clock_report();
        dc8200_report();
    }
    return running ? KERR_OK : KERR_IO;
}

/* Step 6. */
static int start_the_scanout(phys_t fb, uint64_t bytes, uint32_t stride,
                             const struct display_timing *t) {
    int rc = ccache_flush_range(fb, bytes);
    if (rc != KERR_OK) {
        kprintf("display: the L2 cannot be flushed (rc %d); the picture may be stale\n", rc);
    }
    rc = dc8200_scanout(fb, stride, t);
    if (rc != KERR_OK) {
        kprintf("display: scanout refused (rc %d)%s\n", rc,
                rc == KERR_RANGE ? ": the framebuffer is above 4 GiB" : "");
        return rc;
    }
    return check_the_scanout_runs(t);
}

/* Steps 2 to 4, which only power and clock: no mode in them. */
static int power_and_clock(const struct fdt *fdt, const struct display_timing *t) {
    int rc = give_the_pins_to_hdmi();
    if (rc != KERR_OK) {
        kprintf("display: the HDMI pins could not be assigned (rc %d)\n", rc);
        return rc;
    }
    rc = power_the_domain(fdt);
    rc = rc != KERR_OK ? rc : clock_the_system_side();
    rc = rc != KERR_OK ? rc : clock_the_display_side(t->pixel_khz);
    return rc != KERR_OK ? rc : check_the_controller_answers();
}

int display_jh7110_start(const struct fdt *fdt, phys_t fb, uint32_t width, uint32_t height,
                         uint32_t stride) {
    const struct display_timing *t = display_mode_cea(width, height);
    uint64_t bytes = (uint64_t)stride * height;
    int rc;
    if (t == NULL) {
        kprintf("display: %ux%u is not a mode the transmitter has settings for\n", width, height);
        return KERR_NOTSUP;
    }
    report_the_starting_state(fdt);
    (void)ccache_init(fdt);
    rc = power_and_clock(fdt, t);
    rc = rc != KERR_OK ? rc : start_the_transmitter(t);
    rc = rc != KERR_OK ? rc : start_the_scanout(fb, bytes, stride, t);
    if (rc == KERR_OK) {
        display = (struct scanout){.up = true, .fb = fb, .bytes = bytes};
        kprintf("display: HDMI is up — whether a picture shows needs someone at the monitor\n");
    }
    return rc;
}

void display_jh7110_refresh(void) {
    uint64_t from;
    if (!display.up) {
        return;
    }
    from = timer_ns();
    (void)ccache_flush_range(display.fb, display.bytes);
    display.last_refresh_us = (timer_ns() - from) / 1000u;
    display.refreshes++;
}

void display_jh7110_refresh_stats(uint64_t *count, uint64_t *last_us) {
    *count = display.refreshes;
    *last_us = display.last_refresh_us;
}
