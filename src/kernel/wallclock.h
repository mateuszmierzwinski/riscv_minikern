/* Wall-clock time on a board that has no clock.
 *
 * **The VisionFive 2 has no real-time clock fitted** — its device tree names
 * none, which `minikern.buses` confirmed — so the kernel has no epoch to
 * count from and every timestamp it wrote dated to 1970. That was recorded
 * as a limitation of the hardware, and it is: the board cannot *keep* time
 * across a power cut.
 *
 * But it does not have to. Something else on the network knows what time it
 * is, and the boot loader can be told: `minikern.time=<unix seconds>` sets
 * the epoch once, and the monotonic counter carries it from there. The
 * harness fills it in from this workstation's clock, so a board run has the
 * real date without a battery or a time protocol.
 *
 * What that buys, and it is more than tidier file dates: a **grace period**
 * is a wall-clock deadline. XFS stores a soft quota's expiry as one, and
 * without an epoch a deadline written from a boot-relative clock is nonsense
 * on the next boot — so soft limits were read, reported and never enforced.
 * With a real epoch they can be.
 *
 * **What it does not claim.** This is the time the *operator* said it was,
 * trusted as given: there is no stratum, no drift correction and no
 * authority behind it. A boot with no `minikern.time` reports that it is
 * running on boot-relative time rather than silently pretending to 1970,
 * because a wrong date that looks right is how a grace period expires fifty
 * years early.
 */
#ifndef KERNEL_WALLCLOCK_H
#define KERNEL_WALLCLOCK_H

#include <stdbool.h>

#include "core/types.h"

/* Sets the epoch from the command line's `minikern.time`, if it is there.
 * Called from the composition root, before anything timestamps anything. */
void wallclock_set_epoch(uint64_t unix_seconds);

/* Whether a real epoch was given. A caller that must not act on a guessed
 * date — a quota grace period, say — asks this first. */
bool wallclock_is_real(void);

/* Seconds since the Unix epoch, or seconds since boot when none was given.
 * The second case is honest rather than useful, and `wallclock_is_real`
 * is how a caller tells which it has. */
uint64_t wallclock_seconds(void);

/* The same, in nanoseconds: `CLOCK_REALTIME`. */
uint64_t wallclock_ns(void);

#endif
