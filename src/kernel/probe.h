/* probe — what this machine actually is, measured rather than assumed.
 *
 * The scheduler's numbers are not a matter of taste. How long a slice should
 * be depends on what a context switch costs; whether a timer can be armed
 * per hart depends on whether the hardware has `Sstc`; whether switching
 * address spaces is cheap depends on whether `satp` has an ASID field. Each
 * of those is a property of the silicon, and every one of them has a default
 * that is wrong on some board.
 *
 * So this measures them and prints a table, and the design follows the
 * table. It runs on demand (`minikern.probe=1`) because it is slow and
 * deliberately noisy — it is a report, not a boot step.
 *
 * What it cannot measure, it says so rather than guessing: the PMIC's
 * voltage ramp needs an I²C driver that does not exist yet, and a frequency
 * switch cannot be timed before there is a clock driver to switch it.
 */
#ifndef MINIKERN_KERNEL_PROBE_H
#define MINIKERN_KERNEL_PROBE_H

#include "core/fdt.h"

/* Measures the machine and prints what it found. */
void probe_report(const struct fdt *fdt);

#endif
