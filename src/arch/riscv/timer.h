/* timer — the tick, through SBI.
 *
 * RISC-V gives a supervisor one 64-bit counter (`time`) and one comparator
 * per hart, and the comparator is written **through the firmware**, not
 * directly: `sbi_set_timer` sets the next deadline. So there is one way to
 * arm a timer, it is the same on an emulator and on silicon, and it needs no
 * knowledge of the board at all.
 *
 * Compare the ARM kernel this migrated from, which programmed the generic
 * timer's comparator itself, per core, and had to read `CNTFRQ` because the
 * frequency differed between the emulator and the chip — 62.5 MHz through
 * `-kernel` and 24 MHz through U-Boot, which is exactly the sort of fact a
 * hard-coded constant gets wrong on one path.
 *
 * The frequency is still a board fact here, and still comes from the device
 * tree (`/cpus/timebase-frequency`), because the counter's rate is the
 * machine's and nothing in the ISA states it.
 */
#ifndef MINIKERN_ARCH_RISCV_TIMER_H
#define MINIKERN_ARCH_RISCV_TIMER_H

#include <stdbool.h>
#include <stdint.h>

#include "core/fdt.h"

/* Reads the counter's frequency from the tree and arms the first tick.
 * `hz` is how many ticks a second are wanted. KERR_NOENT when the tree does
 * not say what the frequency is, because a timer armed with a guessed
 * frequency is worse than no timer: it looks like it works. */
int timer_init(const struct fdt *fdt, uint32_t hz);

/* Arms the next tick, one interval from the deadline that just passed — not
 * from *now*, so a slow handler does not make the clock drift. */
void timer_rearm(void);

/* The counter, which is monotonic and never wraps in any practical lifetime
 * (at 4 MHz, 2^64 ticks is 146 thousand years). */
uint64_t timer_now(void);

/* Nanoseconds since boot, from the counter and the frequency. */
uint64_t timer_ns(void);

/* How many ticks have been taken, and the configured rate. */
uint64_t timer_ticks(void);

/* Every hart's ticks added together, which is a measure of how much of the
 * machine is awake rather than of time. */
uint64_t timer_ticks_all_harts(void);

/* Arms **this hart's** timer and enables its interrupt. Every hart calls it
 * for itself, because the comparator is per-hart: `timer_init` reads the
 * frequency from the tree once, and this is the part that is each hart's
 * own. */
int timer_init_cpu(void);
uint32_t timer_hz(void);
uint64_t timer_frequency(void);
bool timer_ready(void);

/* Called from the trap handler when a timer interrupt arrives. */
void timer_on_tick(void);

/* A callback run on every tick, for whatever wants to know — the scheduler
 * does, to preempt. NULL to remove it. */
void timer_set_tick_hook(void (*hook)(void));

#endif
