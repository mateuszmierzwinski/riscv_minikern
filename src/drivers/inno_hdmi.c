#include "drivers/inno_hdmi.h"

#include <stddef.h>

#include "arch/riscv/mmu.h"
#include "core/errno.h"
#include "drivers/mmio_poll.h"

/* The register window: the PHY's registers end at 0x1d3, so 0x800 bytes of
 * 32-bit words cover them all. */
#define HDMI_WINDOW_BYTES 0x800u

/* Registers, by number (docs/DISPLAY.md §3.1). */
#define SYS_CTRL 0x00u
#define VIDEO_CONTRL1 0x01u
#define VIDEO_CONTRL2 0x02u
#define AV_MUTE 0x05u
#define VIDEO_TIMING_CTL 0x08u
#define DDC_CLK_LOW 0x4bu
#define DDC_CLK_HIGH 0x4cu
#define EDID_SEGMENT 0x4du
#define EDID_WORD_ADDR 0x4eu
#define EDID_FIFO_OFFSET 0x4fu
#define EDID_FIFO_DATA 0x50u
#define HDCP_CTRL 0x52u
#define INT_MASK1 0xc0u
#define INT_STATUS1 0xc1u
#define HDMI_STATUS 0xc8u
#define COLORBAR 0xc9u
#define PHY_SYNC 0xceu
#define PHY_PRE_PLL_LOCK 0x1a9u
#define PHY_POST_PLL_LOCK 0x1afu
#define PHY_BANDGAP 0x1b0u
#define PHY_TMDS_DRIVERS 0x1b2u
#define PHY_LDO 0x1b4u
#define PHY_SERIALIZER 0x1beu
#define PHY_PRE_EMPHASIS 0x1bfu
#define PHY_SLEW 0x1c0u
#define PHY_PLL_REFERENCE 0x1ccu

#define SYS_POWER_OFF 0x63u /* analog and digital out of reset, PHY powered down */
#define SYS_POWER_ON 0x61u
#define STATUS_HOTPLUG 0x80u
#define INT_EDID_READY 0x04u
#define COLORBAR_NORMAL 0x10u
#define COLORBAR_BIST 0x00u
#define TMDS_ALL_LANES 0x8fu
#define LDO_ON 0x07u
#define SERIALIZER_ON 0x71u
#define BANDGAP_ON 0x04u
#define PLL_REFERENCE 0x0fu
#define VIDEO_EXTERNAL_DE 0x01u /* RGB 4:4:4, DE from the controller */

/* The DDC clock divider the vendor driver uses on this SoC: (51.2 MHz >> 2)
 * / 100 kHz. Its source clock is undocumented; 128 is what has worked. */
#define DDC_DIVIDER 128u

#define PLL_LOCK_LIMIT_NS 20000000ull
#define EDID_LIMIT_NS 100000000ull
#define TMDS_SETTLE_NS 50000000ull

static volatile uint32_t *regs;

static volatile uint32_t *reg(uint32_t n) {
    return regs + n;
}

static void put(uint32_t n, uint8_t value) {
    *reg(n) = value;
}

int inno_hdmi_init(phys_t base) {
    void *at = mmu_map_device(base, HDMI_WINDOW_BYTES * 4u);
    if (at == NULL) {
        return KERR_NOMEM;
    }
    regs = at;
    return KERR_OK;
}

bool inno_hdmi_hotplug(void) {
    return regs != NULL && (*reg(HDMI_STATUS) & STATUS_HOTPLUG) != 0;
}

/* One register write of a PLL table. */
struct reg_write {
    uint16_t reg;
    uint8_t value;
};

/* The PHY's pre- and post-PLL for each mode's pixel clock, from a 24 MHz
 * reference, ending with the pre-PLL powered up (docs/DISPLAY.md §3.2: the
 * vendor Linux table and U-Boot agree on every byte). */
static const struct reg_write pll_1080p[] = {
    {0x1a0, 0x01}, {0x1aa, 0x0f}, {0x1a1, 0x01}, {0x1a2, 0xf0}, {0x1a3, 0x63},
    {0x1a4, 0x15}, {0x1a5, 0x41}, {0x1a6, 0x42}, {0x1ab, 0x01}, {0x1ac, 0x14},
    {0x1ad, 0x01}, {0x1aa, 0x0e}, {0x1a0, 0x00},
};

static const struct reg_write pll_720p[] = {
    {0x1a0, 0x01}, {0x1aa, 0x0f}, {0x1a1, 0x01}, {0x1a2, 0xf0}, {0x1a3, 0x63},
    {0x1a4, 0x1a}, {0x1a5, 0x41}, {0x1a6, 0x64}, {0x1ab, 0x01}, {0x1ac, 0x14},
    {0x1ad, 0x01}, {0x1aa, 0x0e}, {0x1a0, 0x00},
};

/* The table and the eye-diagram tweak for a VIC, or NULL. */
static const struct reg_write *pll_table(uint8_t vic, uint32_t *n, uint8_t emphasis[2]) {
    if (vic == 16u) {
        *n = sizeof pll_1080p / sizeof pll_1080p[0];
        emphasis[0] = 0x02u;
        emphasis[1] = 0x22u;
        return pll_1080p;
    }
    if (vic == 4u) {
        *n = sizeof pll_720p / sizeof pll_720p[0];
        emphasis[0] = 0x00u;
        emphasis[1] = 0x00u;
        return pll_720p;
    }
    return NULL;
}

static bool locked(uint32_t n) {
    uint32_t last = 0;
    return mmio_poll(reg(n), 1u, 1u, PLL_LOCK_LIMIT_NS, &last);
}

/* Steps 2–6 of the vendor sequence: references on, the PLL table, both
 * locks, then the LDO, serializer and the per-mode eye tweak. */
static int lock_the_plls(const struct display_timing *t, const char **why) {
    uint8_t emphasis[2];
    uint32_t n = 0;
    const struct reg_write *table = pll_table(t->vic, &n, emphasis);
    if (table == NULL) {
        *why = "no PLL settings for this mode";
        return KERR_NOTSUP;
    }
    put(PHY_BANDGAP, (uint8_t)(*reg(PHY_BANDGAP) | BANDGAP_ON));
    put(PHY_PLL_REFERENCE, PLL_REFERENCE);
    for (uint32_t i = 0; i < n; i++) {
        put(table[i].reg, table[i].value);
    }
    if (!locked(PHY_PRE_PLL_LOCK) || !locked(PHY_POST_PLL_LOCK)) {
        *why = locked(PHY_PRE_PLL_LOCK) ? "the post-PLL did not lock" : "the pre-PLL did not lock";
        return KERR_TIMEDOUT;
    }
    put(PHY_LDO, LDO_ON);
    put(PHY_SERIALIZER, SERIALIZER_ON);
    put(PHY_PRE_EMPHASIS, emphasis[0]);
    put(PHY_SLEW, emphasis[1]);
    return KERR_OK;
}

/* The timing registers, 0x08 to 0x15, with the transmitter powered down. */
static void frame_the_stream(const struct display_timing *t) {
    uint8_t bytes[HDMI_TIMING_BYTES];
    display_hdmi_timing(t, bytes);
    put(SYS_CTRL, SYS_POWER_OFF);
    put(VIDEO_CONTRL1, VIDEO_EXTERNAL_DE);
    put(VIDEO_CONTRL2, 0x00u); /* 8-bit RGB in and out */
    put(HDCP_CTRL, 0x00u);     /* DVI: no infoframes needed */
    for (uint32_t i = 0; i < HDMI_TIMING_BYTES; i++) {
        put(VIDEO_TIMING_CTL + i, bytes[i]);
    }
}

int inno_hdmi_set_mode(const struct display_timing *t, const char **why) {
    int rc;
    *why = "";
    if (regs == NULL) {
        *why = "not mapped";
        return KERR_NODEV;
    }
    rc = lock_the_plls(t, why);
    if (rc != KERR_OK) {
        return rc;
    }
    frame_the_stream(t);
    put(SYS_CTRL, SYS_POWER_ON);
    put(PHY_TMDS_DRIVERS, TMDS_ALL_LANES);
    mmio_spin_ns(TMDS_SETTLE_NS);
    put(AV_MUTE, 0x00u); /* neither muted nor blacked out */
    put(PHY_SYNC, 0x00u);
    put(PHY_SYNC, 0x01u);
    return KERR_OK;
}

/* Starts a read of 128 bytes at `word` of `segment` into the FIFO and waits
 * for the transmitter to say it is there. */
static int fetch_block(uint8_t segment, uint8_t word) {
    uint32_t last = 0;
    put(DDC_CLK_LOW, (uint8_t)(DDC_DIVIDER & 0xffu));
    put(DDC_CLK_HIGH, (uint8_t)(DDC_DIVIDER >> 8));
    put(INT_MASK1, INT_EDID_READY);
    put(INT_STATUS1, INT_EDID_READY); /* clear a stale one */
    put(EDID_FIFO_OFFSET, 0x00u);
    put(EDID_WORD_ADDR, word);
    put(EDID_SEGMENT, segment); /* this write starts the transfer */
    if (!mmio_poll(reg(INT_STATUS1), INT_EDID_READY, INT_EDID_READY, EDID_LIMIT_NS, &last)) {
        put(INT_MASK1, 0x00u);
        return KERR_TIMEDOUT;
    }
    put(INT_STATUS1, INT_EDID_READY);
    return KERR_OK;
}

int inno_hdmi_read_edid(uint8_t block, uint8_t out[EDID_BLOCK_BYTES]) {
    int rc;
    if (regs == NULL) {
        return KERR_NODEV;
    }
    rc = fetch_block((uint8_t)(block / 2u), (uint8_t)((block % 2u) * EDID_BLOCK_BYTES));
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint32_t i = 0; i < EDID_BLOCK_BYTES; i++) {
        out[i] = (uint8_t)*reg(EDID_FIFO_DATA);
    }
    put(INT_MASK1, 0x00u);
    return KERR_OK;
}

void inno_hdmi_colour_bars(bool on) {
    if (regs != NULL) {
        put(COLORBAR, on ? COLORBAR_BIST : COLORBAR_NORMAL);
    }
}
