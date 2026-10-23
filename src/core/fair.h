/* fair — the arithmetic of proportional-share scheduling.
 *
 * Pure functions: no clock, no threads, no locks, nothing from `arch/` or
 * `board/`. The scheduler keeps the state and decides *when*; this file
 * decides *how much*, which makes the part that is easy to get subtly wrong
 * the part that can be tested on the host.
 *
 * The model is weighted virtual time. Each thread has a **weight** from its
 * nice value, and accumulates **virtual runtime** at a rate inversely
 * proportional to that weight: a heavy thread's clock runs slow, so it is
 * chosen more often. Picking the smallest virtual runtime therefore shares
 * the processor in proportion to weight, without any thread being starved —
 * a thread that is passed over keeps its virtual time while others advance,
 * so it becomes the smallest eventually and unconditionally.
 *
 * The nice-to-weight table is Linux's, as an **interface fact** rather than
 * borrowed code: a program that asks for nice -5 expects a particular share
 * of the processor, in the same way that it expects `ENOENT` to be 2. Each
 * step is about 1.25x, which is what makes one nice level worth roughly ten
 * percent of the processor between two threads.
 */
#ifndef MINIKERN_CORE_FAIR_H
#define MINIKERN_CORE_FAIR_H

#include <stdint.h>

#define FAIR_NICE_MIN (-20)
#define FAIR_NICE_MAX 19
#define FAIR_NICE0_WEIGHT 1024u

/* `nice` brought into range. Out-of-range values are clamped rather than
 * refused, which is what POSIX requires of `setpriority`. */
int32_t fair_clamp_nice(int32_t nice);

/* The weight a nice value is worth. Clamps, so it is safe on any input. */
uint32_t fair_weight_of_nice(int32_t nice);

/* How much virtual time `ran_ns` of real time is worth to a thread of this
 * weight: `ran_ns * NICE0_WEIGHT / weight`. A nice-0 thread's virtual clock
 * runs at real speed, a heavier thread's slower, a lighter thread's faster.
 *
 * Computed so that a long run cannot overflow: the multiplication is split
 * across the division rather than done first. */
uint64_t fair_vruntime_delta(uint64_t ran_ns, uint32_t weight);

/* The slice a thread of `weight` deserves when the runnable threads weigh
 * `total_weight` between them, given a target latency and a floor.
 *
 * `target_ns` is the period in which every runnable thread should get a
 * turn; a thread's share of it is its share of the weight. The floor exists
 * because a slice shorter than the cost of switching to it spends the
 * processor on switching: with many runnable threads the period stretches
 * instead of the slices shrinking without limit.
 */
uint64_t fair_slice_ns(uint32_t weight, uint32_t total_weight, uint64_t target_ns,
                       uint64_t min_ns);

/* Where a thread's virtual clock should be set when it becomes runnable
 * after not being so — a new thread, or one that has been asleep.
 *
 * It must not keep the virtual time it had: a thread that slept for a second
 * would be a second of virtual time behind everything else and would then
 * monopolise the processor until it caught up. Nor may it simply take the
 * minimum, which would hand a thread that sleeps repeatedly a fresh claim
 * every time. It takes the minimum **less a bounded credit**, so waking is
 * worth a little — enough that an interactive thread is served promptly —
 * and no more.
 */
uint64_t fair_wake_vruntime(uint64_t own, uint64_t minimum, uint64_t credit_ns);

#endif
