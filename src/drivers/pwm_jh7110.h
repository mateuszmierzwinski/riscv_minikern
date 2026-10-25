/* PWM on the JH7110: `starfive,jh7110-pwm` at 0x120d0000, from the tree.
 *
 * It is an OpenCores PTC — a timer-counter block with one set of registers
 * per channel — and the useful thing about it for a kernel with nothing
 * wired to the header is that the counter is **readable**: a channel that
 * has been started can be seen counting, which proves the clock and the
 * register map without an oscilloscope.
 */
#ifndef DRIVERS_PWM_JH7110_H
#define DRIVERS_PWM_JH7110_H

#include "core/fdt.h"
#include "core/types.h"

#define PWM_CHANNELS 8u

struct pwm_jh7110 {
    volatile uint32_t *regs;
    bool up;
};

int pwm_jh7110_init(struct pwm_jh7110 *pwm, const struct fdt *fdt, const char *path);

/* Starts `channel` with a period of `period_ticks` and a high time of
 * `high_ticks`, both in counter ticks of the block's input clock. A high
 * time at or above the period is full scale, which the hardware expresses by
 * never reloading — so it is clamped here rather than passed through. */
int pwm_jh7110_start(struct pwm_jh7110 *pwm, uint32_t channel, uint32_t period_ticks,
                     uint32_t high_ticks);

/* Stops `channel`, leaving its output low. */
int pwm_jh7110_stop(struct pwm_jh7110 *pwm, uint32_t channel);

/* The channel's live counter. **This is the proof**: read twice with the
 * channel running, it must have moved, and a block that is unclocked or
 * whose registers are elsewhere returns the same value both times. */
uint32_t pwm_jh7110_counter(const struct pwm_jh7110 *pwm, uint32_t channel);

#endif
