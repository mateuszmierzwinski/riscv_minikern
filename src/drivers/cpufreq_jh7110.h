/* cpufreq_jh7110 — changing the CPU clock on a JH7110.
 *
 * The hardware half of frequency scaling. `core/cpufreq.c` is the other
 * half — it decides *which* operating point, from load and temperature, and
 * knows nothing about this board. This file knows one register.
 *
 * ## What the board actually does, measured
 *
 * The VisionFive 2's device tree offers four operating points:
 *
 *     375 MHz at 800 mV      750 MHz at 800 mV
 *     500 MHz at 800 mV     1500 MHz at 1040 mV
 *
 * and three of the four share a voltage. The system clock generator's word
 * at index 1 is the CPU core divider: on a board running at 1500 MHz it
 * reads 1, and writing 2 was measured to halve the processor's speed exactly
 * — a loop timed against the fixed 4 MHz reference clock went from
 * 165,562,913 iterations a second to 82,841,462, a ratio of 1.99.
 *
 * So the four points are **1500 MHz divided by 1, 2, 3 and 4**, and the
 * whole of frequency scaling on this board is that one divider.
 *
 * ## Why this needs no PMIC, and where it stops
 *
 * **Lowering frequency at a voltage already set is always safe.** The unsafe
 * order is raising frequency before voltage; this only ever divides a clock
 * whose voltage was set for the fastest point. Running at 375 MHz on 1.04 V
 * is wasteful and harmless.
 *
 * What that means is that this delivers the *latency and thermal* half of
 * DVFS and not the power half: most of the saving at a low operating point
 * comes from dropping to 800 mV, and that needs the AXP15060 over I²C —
 * neither driver exists. The gap is deliberate and stated rather than
 * quietly left: `docs/ROADMAP.md` carries it.
 *
 * It also means the transitions here are **unconditionally safe in any
 * order**, which is why there is no sequencing protocol in this file. Adding
 * voltage control will add one, and that is the point at which it stops
 * being a one-register driver.
 */
#ifndef MINIKERN_DRIVERS_CPUFREQ_JH7110_H
#define MINIKERN_DRIVERS_CPUFREQ_JH7110_H

#include <stdbool.h>
#include <stdint.h>

#include "core/cpufreq.h"
#include "core/fdt.h"

/* Reads the operating points from the device tree and takes the current
 * divider as the starting state. Returns how many points were found; 0
 * means this machine does not describe any, and nothing else here will do
 * anything.
 *
 * The policy it fills is `core/cpufreq.c`'s, so the governor and this driver
 * agree about what the points are rather than each having a table. */
uint32_t cpufreq_jh7110_init(const struct fdt *fdt, struct cpufreq_policy *policy);

/* The frequency the processor is set to, in kHz, or 0 before `init`. Derived
 * from the divider rather than remembered, so it cannot drift from the
 * hardware. */
uint32_t cpufreq_jh7110_current_khz(void);

/* Sets the processor to the operating point at or below `khz`.
 *
 * Returns the frequency actually selected, or 0 if nothing could be. Rounds
 * **down** to a point the table offers, because rounding up could ask for a
 * frequency whose voltage has not been set — the one direction that is not
 * safe.
 *
 * `KERR_NOTSUP` territory in spirit but not in return type: the top point is
 * reachable only because the firmware set its voltage at boot, and this
 * never lowers a voltage, so returning to it is always allowed. */
uint32_t cpufreq_jh7110_set_khz(uint32_t khz);

/* The divider the hardware holds, which is the fact everything else here is
 * derived from. Exposed for the measurement that proves a change happened. */
uint32_t cpufreq_jh7110_divider(void);

#endif
