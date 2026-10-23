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

/* A given hart's ticks, by enumeration index. For a report that has to
 * distinguish "the other harts are idle" from "the other harts are not
 * being interrupted at all" — which look the same from outside and have
 * completely different causes. */
uint64_t timer_ticks_of(uint32_t hart_index);

/* Arms **this hart's** timer and enables its interrupt. Every hart calls it
 * for itself, because the comparator is per-hart: `timer_init` reads the
 * frequency from the tree once, and this is the part that is each hart's
 * own. */
int timer_init_cpu(void);

/* Asks this hart to take a timer interrupt no later than `ns`, in addition
 * to its periodic tick.
 *
 * **Why this exists.** The tick is 100 Hz, so a thread that sleeps waits for
 * the next one: a request for 20 microseconds costs up to 10 milliseconds.
 * That is fine until something asks for a great many short sleeps — a Go
 * runtime's backoff does exactly that, 134 of them in one program, and each
 * rounded up to the tick turned a 50 ms `time.Sleep` into 950 ms. The
 * kernel's own measurements said its wake-ups were at most 9.9 ms late,
 * which is the tick doing precisely what it was told; the fault was in
 * asking the tick to do it at all.
 *
 * Arming earlier does not change the **rate**: the periodic schedule is kept
 * separately, so an early wake-up is not counted as a tick and the next tick
 * still falls where it would have. A clock that drifted every time someone
 * slept would be a worse bargain than the latency it fixed. */
void timer_wake_at(uint64_t ns);
uint32_t timer_hz(void);
uint64_t timer_frequency(void);
bool timer_ready(void);

/* Called from the trap handler when a timer interrupt arrives. */
void timer_on_tick(void);

/* Callbacks run on every tick, for whatever wants to know: the scheduler
 * does, to preempt, and the network stack does, to run its timers. A list
 * rather than one slot because there are two of them — and a single slot
 * silently replaced one with the other, which is a feature that stops
 * working rather than a build error.
 *
 * Hooks run in the order they were added. `timer_clear_tick_hooks` removes
 * them all, for a kernel tearing something down. KERR_NOSPC when the list
 * is full. */
int timer_add_tick_hook(void (*hook)(void));

/* ── deadlines ─────────────────────────────────────────────────────────── */

/* Arms this hart's timer for `ns`, or leaves it alone if that is already
 * what it is armed for.
 *
 * **Not re-arming matters here more than on most machines.** This hardware
 * has no `Sstc`, so there is no timer CSR a supervisor can write: every arm
 * is a call into firmware, measured at **335 ns** on a VisionFive 2 — seven
 * context switches. A scheduler that re-armed on every decision would spend
 * more time arming than deciding.
 *
 * The deadline is rounded up to `TIMER_GRANULARITY_NS`, which is what makes
 * "already armed for that" happen often enough to be worth testing for: two
 * threads whose slices end 3 µs apart ask for the same deadline. */
void timer_arm_at(uint64_t ns);

/* Hands the timer over to whoever will arm it.
 *
 * Until this is called the timer is **periodic**, which is what the boot
 * path needs: it proves the clock runs by counting ticks, and a clock that
 * only ticks when asked cannot be proved that way. Once a scheduler exists
 * there is something that knows when the next event is due, and ticking for
 * its own sake becomes pure overhead — a hundred interrupts a second that
 * find nothing to do.
 *
 * After this, a hart is interrupted only when something asked to be. */
void timer_take_over(void);

/* Takes this hart's timer away entirely: nothing is due, so nothing should
 * interrupt.
 *
 * This is the point of the whole arrangement. One runnable thread on a hart
 * and nothing sleeping means **no timer interrupts at all** — not a tick
 * that finds nothing to do, which is what a periodic clock would deliver a
 * hundred times a second for as long as the machine is busy. */
void timer_disarm(void);

/* What this hart is armed for, in nanoseconds, or 0 when it is disarmed. */
uint64_t timer_armed_at(void);

/* How many timer interrupts this hart has taken. The measure of whether
 * dynticks is working: a hart running one thread with nothing asleep should
 * hold this still. */
uint64_t timer_interrupts_of(uint32_t hart_index);

/* Deadlines are rounded up to this, so that near-identical requests become
 * identical ones and the arm can be skipped. 100 µs is 400 ticks of this
 * board's 4 MHz clock — coarse enough to coalesce, fine enough that a 1.5 ms
 * slice is not visibly lengthened. */
#define TIMER_GRANULARITY_NS 100000ull
void timer_remove_tick_hook(void (*hook)(void));

#endif
