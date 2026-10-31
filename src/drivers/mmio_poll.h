/* Waiting on a register, with a deadline — the one loop every bring-up
 * sequence in the display path needs (a power domain reporting on, a PLL
 * locking, a reset releasing).
 *
 * A vendor loop that spins until a bit sets has no answer for hardware that
 * never sets it, and the answer here is a timeout the caller reports with
 * the register's last value: a bring-up that hangs says nothing, one that
 * times out names the bit.
 */
#ifndef MINIKERN_DRIVERS_MMIO_POLL_H
#define MINIKERN_DRIVERS_MMIO_POLL_H

#include <stdbool.h>
#include <stdint.h>

#include "arch/riscv/timer.h"

/* Spins for at least `ns` nanoseconds. For the settle times a datasheet
 * gives in microseconds or milliseconds during bring-up, never on a path a
 * program waits on. */
static inline void mmio_spin_ns(uint64_t ns) {
    uint64_t until = timer_ns() + ns;
    while (timer_ns() < until) {
    }
}

/* Waits until `(*reg & mask) == want` or `limit_ns` passes. The last value
 * read goes to `*last` either way. */
static inline bool mmio_poll(volatile uint32_t *reg, uint32_t mask, uint32_t want,
                             uint64_t limit_ns, uint32_t *last) {
    uint64_t until = timer_ns() + limit_ns;
    do {
        *last = *reg;
        if ((*last & mask) == want) {
            return true;
        }
    } while (timer_ns() < until);
    return false;
}

#endif
