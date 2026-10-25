#include "drivers/temp_jh7110.h"

#include "arch/riscv/mmu.h"
#include "arch/riscv/timer.h"
#include "core/console.h"
#include "core/errno.h"
#include "drivers/crg_jh7110.h"

/* The sensor's one control-and-data register, as the SoC documents it.
 *
 * `RSTN` is **active low**: the block is held in reset while the bit is
 * clear, so starting it means setting that bit rather than clearing it. That
 * inversion is the easiest thing here to get backwards, and backwards it
 * reads a constant zero — which converts to a confident -81 C. */
#define TEMP_CONTROL 0x00u
#define TEMP_RSTN (1u << 0)  /* 1 releases reset */
#define TEMP_PD (1u << 1)    /* 1 powers down */
#define TEMP_RUN (1u << 2)   /* 1 converts continuously */
/* **The reading is in the control register, not in one of its own.**
 *
 * Measured, not assumed: the first bring-up read `+04` and got zero, and the
 * dump beside it showed `+00: 080f0005` — the five being what this driver
 * had just written, and `0x80f` sitting in bits 16..27 above it. That
 * converts to 38.5 C, which is what a die under a radiator and a fan should
 * read; `+04` and everything after it are zero. So the block has one
 * register carrying both the control bits and the conversion.
 *
 * This is the fourth register map in this project that the documentation
 * gets wrong for this board, and the fourth time a dump settled it in one
 * boot. The driver now reads where the value is. */
#define TEMP_DATA 0x00u
#define TEMP_DOUT_SHIFT 16u
#define TEMP_DOUT_BITS 0xfffu

/* How long the block is given between release and its first conversion. The
 * documented settling time is well under a millisecond; a generous wait once
 * at startup costs nothing and removes a class of "the first reading is
 * wrong" from the driver. */
/* How long a first conversion is waited for. The block is documented to
 * settle in well under a millisecond; a hundred of them costs nothing once
 * at startup and is long enough that a slow sensor is not mistaken for a
 * broken one. */
#define FIRST_CONVERSION_NS 100000000ull

/* What silicon can physically be. A reading outside this is refused: the
 * ARM project's governor acted on an impossible -264 C and went to the top
 * of its frequency table with nothing watching the temperature, which is the
 * failure this bound exists to prevent. */
#define TEMP_FLOOR_MDEG (-40000)
#define TEMP_CEILING_MDEG 150000

static uint32_t rd(const struct temp_jh7110 *t, uint32_t off) {
    return t->regs[off / 4u];
}

static void wr(struct temp_jh7110 *t, uint32_t off, uint32_t v) {
    t->regs[off / 4u] = v;
}

int temp_jh7110_init(struct temp_jh7110 *t, const struct fdt *fdt, const char *path) {
    phys_t base = 0;
    uint64_t size = 0;
    if (t == NULL) {
        return KERR_INVAL;
    }
    t->up = false;
    if (fdt_device_reg(fdt, path, 0, &base, &size) != KERR_OK) {
        return KERR_NOENT;
    }
    /* Clocked and reset from the node's own properties, like every other
     * peripheral here. A sensor that was never switched on reads zero, and
     * zero converts to a plausible -81 C rather than to an obvious failure —
     * which is exactly how an unclocked driver gets believed. */
    (void)crg_enable_device(fdt, path);
    t->regs = mmu_map_device(base, size);
    if (t->regs == NULL) {
        return KERR_NOMEM;
    }
    /* Powered up, reset released, converting. `RSTN` set rather than
     * cleared: it is active low. */
    wr(t, TEMP_CONTROL, TEMP_RSTN);
    wr(t, TEMP_CONTROL, TEMP_RSTN | TEMP_RUN);
    t->up = true;
    /* **Waits for a conversion, rather than for a fixed time.**
     *
     * A two-millisecond settle was not enough: the first read returned zero
     * while the dump taken immediately after it showed a live value, and a
     * raw zero converts to a confident -81.1 C. So the wait is for the thing
     * that matters — a non-zero conversion — and it is bounded, because a
     * sensor that never produces one is a sensor to report rather than to
     * wait on for ever.
     *
     * Zero is the right thing to wait out: it is not a temperature this
     * block can measure (it is below the floor by 40 degrees), so it can
     * only mean "no conversion yet". */
    {
        uint64_t until = timer_ns() + FIRST_CONVERSION_NS;
        while (timer_ns() < until && temp_jh7110_raw(t) == 0) {
        }
    }
    if (temp_jh7110_raw(t) == 0) {
        kprintf("temp: the sensor produced no conversion within %u ms\n",
                (uint32_t)(FIRST_CONVERSION_NS / 1000000u));
    }
    return KERR_OK;
}

uint32_t temp_jh7110_raw(const struct temp_jh7110 *t) {
    if (!t->up) {
        return 0;
    }
    return (rd(t, TEMP_DATA) >> TEMP_DOUT_SHIFT) & TEMP_DOUT_BITS;
}

int32_t temp_jh7110_millidegrees(const struct temp_jh7110 *t) {
    uint32_t raw;
    int32_t mdeg;
    if (!t->up) {
        return TEMP_UNREADABLE;
    }
    raw = temp_jh7110_raw(t);
    /* `237500 * 4095` is 972 million, inside an int32 — so the multiply does
     * not need widening, and saying so stops someone "fixing" it later. */
    mdeg = (int32_t)((raw * 237500u) / 4096u) - 81100;
    if (mdeg < TEMP_FLOOR_MDEG || mdeg > TEMP_CEILING_MDEG) {
        return TEMP_UNREADABLE;
    }
    return mdeg;
}

void temp_jh7110_sample(struct temp_jh7110 *t) {
    int32_t mdeg = temp_jh7110_millidegrees(t);
    if (mdeg == TEMP_UNREADABLE) {
        return; /* a refused reading is not a new low */
    }
    if (t->readings == 0) {
        t->lowest_mdeg = mdeg;
        t->highest_mdeg = mdeg;
    } else if (mdeg < t->lowest_mdeg) {
        t->lowest_mdeg = mdeg;
    } else if (mdeg > t->highest_mdeg) {
        t->highest_mdeg = mdeg;
    }
    t->readings++;
}

void temp_jh7110_extremes(const struct temp_jh7110 *t, int32_t *lowest, int32_t *highest,
                          uint64_t *readings) {
    bool any = t->readings != 0;
    if (lowest != NULL) {
        *lowest = any ? t->lowest_mdeg : TEMP_UNREADABLE;
    }
    if (highest != NULL) {
        *highest = any ? t->highest_mdeg : TEMP_UNREADABLE;
    }
    if (readings != NULL) {
        *readings = t->readings;
    }
}

void temp_jh7110_dump(const struct temp_jh7110 *t) {
    if (!t->up) {
        return;
    }
    for (uint32_t i = 0; i < 8u; i += 4u) {
        kprintf("temp: +%02x: %08x %08x %08x %08x\n", i * 4u, rd(t, i * 4u), rd(t, (i + 1u) * 4u),
                rd(t, (i + 2u) * 4u), rd(t, (i + 3u) * 4u));
    }
}
