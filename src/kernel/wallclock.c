#include "kernel/wallclock.h"

#include "arch/riscv/timer.h"

/* The epoch, and the monotonic reading it was taken at.
 *
 * **Both, because one is useless without the other.** Storing only the
 * epoch would make every later call return the same second; storing only
 * the offset would lose it across a re-set. The pair is what lets the
 * monotonic counter carry a wall clock. */
static uint64_t epoch_seconds;
static uint64_t epoch_at_ns;
static bool have_epoch;

void wallclock_set_epoch(uint64_t unix_seconds) {
    /* **A plausibility floor, because a wrong date is worse than none.**
     * Anything before 2020 is either a parse that fell back to zero or an
     * operator typo, and accepting it would date files to the 1970s while
     * claiming to be real — the one outcome worse than admitting there is no
     * clock. 1577836800 is 2020-01-01. */
    if (unix_seconds < 1577836800ull) {
        return;
    }
    epoch_seconds = unix_seconds;
    epoch_at_ns = timer_ns();
    have_epoch = true;
}

bool wallclock_is_real(void) {
    return have_epoch;
}

uint64_t wallclock_seconds(void) {
    uint64_t now = timer_ns();
    if (!have_epoch) {
        return now / 1000000000ull;
    }
    return epoch_seconds + (now - epoch_at_ns) / 1000000000ull;
}
