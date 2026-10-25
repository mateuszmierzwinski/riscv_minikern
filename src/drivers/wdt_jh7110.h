/* The watchdog: `starfive,jh7110-wdt` at 0x13070000, from the tree.
 *
 * **It is deliberately never armed to reset.** A watchdog whose reset is
 * enabled restarts the board when nothing feeds it, and this kernel has no
 * component whose job is to feed one — so enabling it would turn every
 * debugging pause, every board run that stops at a breakpoint in a log, into
 * a reboot. What it does instead is read the counter, which proves the
 * register map and the clock, and offer the reset explicitly to a caller
 * that wants it.
 *
 * U-Boot says `WDT: Not starting watchdog@13070000`, so the block arrives
 * unstarted and this is the first thing to touch it.
 */
#ifndef DRIVERS_WDT_JH7110_H
#define DRIVERS_WDT_JH7110_H

#include "core/fdt.h"
#include "core/types.h"

struct wdt_jh7110 {
    volatile uint32_t *regs;
    bool up;
};

int wdt_jh7110_init(struct wdt_jh7110 *wdt, const struct fdt *fdt, const char *path);

/* Starts the counter **without** the reset: it counts down and, at zero,
 * does nothing but set its interrupt. That is enough to prove the block is
 * clocked and the registers are where this driver thinks, and it cannot
 * reboot the machine. */
int wdt_jh7110_start_counting(struct wdt_jh7110 *wdt, uint32_t ticks);

/* The live counter, which is the proof: read twice with it running, it must
 * have fallen. */
uint32_t wdt_jh7110_counter(const struct wdt_jh7110 *wdt);

/* Stops it. */
int wdt_jh7110_stop(struct wdt_jh7110 *wdt);

/* The first `words` registers, for finding out where the counter actually
 * is. This block has two documented layouts — the JH7100's registers sit
 * around 0x100 and the JH7110's in the first 0x40 — and a driver written to
 * the wrong one loads a value into a register that is not the load register
 * and then reads a counter that is not counting. Which is exactly what the
 * first board run showed, so the window is read rather than guessed at
 * again. */
void wdt_jh7110_dump(const struct wdt_jh7110 *wdt, uint32_t words);

/* Finds the register that starts the counter, by trying the candidates and
 * watching whether the counter moves. Returns the offset, or 0. */
uint32_t wdt_jh7110_find_enable(struct wdt_jh7110 *wdt);

#endif
