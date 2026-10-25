#include "drivers/wdt_jh7110.h"

#include "core/console.h"
#include "core/errno.h"
#include "drivers/crg_jh7110.h"
#include "arch/riscv/mmu.h"

/* The JH7110's watchdog registers. It is a Synopsys-derived block with a
 * lock: the control and load registers ignore writes unless the magic has
 * been written first, which is the detail that makes a driver silently do
 * nothing. */
/* **Measured on the board, not taken from the published offsets.**
 *
 * The documented JH7110 layout says control 0x00 and load 0x04. Under that,
 * writing a load value to 0x00 would set control bits and leave the load at
 * zero — and the board shows `0x40000000` at *both* 0x00 and 0x04 after a
 * write to 0x00 alone, which says 0x00 loads and 0x04 is the counter.
 * `wdt_jh7110_find_enable` then tried each candidate and found the pair that
 * actually makes the counter move:
 *
 *     wdt: unlock +01c, enable +008 starts it (1073741818 then 1073738435)
 *
 * The first version of this driver had 0x008 as the *control* register and
 * wrote zero to it, which is the enable being switched off, and then wrote
 * the run bit to 0x104, which is nothing. So it loaded the counter, disabled
 * it, and reported that the block was not clocked — a wrong conclusion about
 * working hardware, and the third time in this project that a guessed
 * register map has produced exactly that.
 *
 * **Where the reset-enable bit lives is deliberately not established**, and
 * nothing here writes any register but these three: a watchdog that can
 * reboot the board is not wanted (there is nothing in this kernel whose job
 * is to feed one), so the safest thing is to leave the register that arms it
 * untouched and unidentified. */
#define WDT_LOAD 0x000u   /* write: the counter's starting value */
#define WDT_VALUE 0x004u  /* read: the live counter, counting down */
#define WDT_ENABLE 0x008u /* bit 0 runs it */
#define WDT_LOCK 0x01cu   /* the magic, before any of the above will take */

#define UNLOCK_MAGIC 0x378f0765u
#define ENABLE_RUN 0x1u

static uint32_t rd(const struct wdt_jh7110 *wdt, uint32_t off) {
    return wdt->regs[off / 4u];
}

static void wr(struct wdt_jh7110 *wdt, uint32_t off, uint32_t v) {
    wdt->regs[off / 4u] = v;
}

/* Unlocks the register file. **Writing zero to the lock re-locks it**, which
 * is the opposite of what the name suggests, so the unlock is written before
 * each configuration rather than once at startup — a driver that unlocked in
 * `init` and configured later would find its writes discarded if anything in
 * between had touched the lock. */
static void unlock(struct wdt_jh7110 *wdt) {
    wr(wdt, WDT_LOCK, UNLOCK_MAGIC);
}

int wdt_jh7110_init(struct wdt_jh7110 *wdt, const struct fdt *fdt, const char *path) {
    phys_t base = 0;
    uint64_t size = 0;
    if (wdt == NULL) {
        return KERR_INVAL;
    }
    wdt->up = false;
    if (fdt_device_reg(fdt, path, 0, &base, &size) != KERR_OK) {
        return KERR_NOENT;
    }
    (void)crg_enable_device(fdt, path);
    wdt->regs = mmu_map_device(base, size);
    if (wdt->regs == NULL) {
        return KERR_NOMEM;
    }
    wdt->up = true;
    return KERR_OK;
}

int wdt_jh7110_start_counting(struct wdt_jh7110 *wdt, uint32_t ticks) {
    if (!wdt->up) {
        return KERR_NODEV;
    }
    if (ticks == 0) {
        return KERR_INVAL;
    }
    unlock(wdt);
    wr(wdt, WDT_ENABLE, 0);
    wr(wdt, WDT_LOAD, ticks);
    /* Unlocked again: the lock closes behind each write on this block, which
     * is why the first version's single unlock in `init` would not have been
     * enough even with the right offsets. */
    unlock(wdt);
    wr(wdt, WDT_ENABLE, ENABLE_RUN);
    return KERR_OK;
}

uint32_t wdt_jh7110_counter(const struct wdt_jh7110 *wdt) {
    return wdt->up ? rd(wdt, WDT_VALUE) : 0;
}

/* Finds which register starts the counter, by trying each candidate.
 *
 * **Measured because the remembered layout is contradicted by the board.**
 * This block has two documented variants, and the published JH7110 offsets
 * say control 0x00 and load 0x04 — under which writing a load value to 0x00
 * would set control bits and leave 0x04 at zero. The board shows 0x40000000
 * at *both* after a write to 0x00 alone, which says 0x00 loads and 0x04 is
 * the counter. Rather than pick whichever source feels more trustworthy, the
 * enable is found by trying the candidates and watching whether the counter
 * at 0x04 moves. That is one board run against an afternoon of being
 * confidently wrong.
 *
 * Safe to do: the load is set enormous beforehand, so even a candidate that
 * happens to arm the reset cannot reach zero inside a boot.
 *
 * Returns the offset that worked, or 0 when none did. */
uint32_t wdt_jh7110_find_enable(struct wdt_jh7110 *wdt) {
    static const uint32_t unlocks[] = {0x01cu, 0x03cu, 0x13cu};
    static const uint32_t enables[] = {0x008u, 0x00cu, 0x010u, 0x014u, 0x104u, 0x110u};
    if (!wdt->up) {
        return 0;
    }
    for (uint32_t u = 0; u < sizeof unlocks / sizeof unlocks[0]; u++) {
        for (uint32_t e = 0; e < sizeof enables / sizeof enables[0]; e++) {
            uint32_t first, second;
            wr(wdt, unlocks[u], UNLOCK_MAGIC);
            wr(wdt, 0x000u, 0x40000000u); /* the load, per the dump */
            wr(wdt, enables[e], 1u);
            first = rd(wdt, 0x004u);
            for (volatile uint32_t i = 0; i < 20000u; i++) {
            }
            second = rd(wdt, 0x004u);
            if (first != second) {
                kprintf("wdt: unlock +%03x, enable +%03x starts it (%u then %u)\n", unlocks[u],
                        enables[e], first, second);
                wr(wdt, enables[e], 0);
                return enables[e];
            }
            wr(wdt, enables[e], 0);
        }
    }
    return 0;
}

void wdt_jh7110_dump(const struct wdt_jh7110 *wdt, uint32_t words) {
    if (!wdt->up) {
        return;
    }
    for (uint32_t i = 0; i < words; i += 4u) {
        kprintf("wdt: +%03x: %08x %08x %08x %08x\n", i * 4u, rd(wdt, i * 4u), rd(wdt, (i + 1u) * 4u),
                rd(wdt, (i + 2u) * 4u), rd(wdt, (i + 3u) * 4u));
    }
}

int wdt_jh7110_stop(struct wdt_jh7110 *wdt) {
    if (!wdt->up) {
        return KERR_NODEV;
    }
    unlock(wdt);
    wr(wdt, WDT_ENABLE, 0);
    return KERR_OK;
}
