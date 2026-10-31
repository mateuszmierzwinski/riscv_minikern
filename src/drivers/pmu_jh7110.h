/* The JH7110's power-management unit: switching a power domain on.
 *
 * Everything in the display path sits in the VOUT domain, which the boot
 * firmware leaves off (U-Boot 2025.04 lights no display, and StarFive's own
 * U-Boot switches the domain off again before the OS). Until it is on, the
 * display controller, the HDMI transmitter and their clock generator read 0
 * and ignore writes — so this is the first step of bringing up a screen.
 *
 * The sequence is a request mask and an "encourage" code pair, then the
 * current-mode register reports the domain on (docs/DISPLAY.md §2.1; the
 * offsets come from driver code, cross-checked across implementations, not
 * from a fetched TRM page).
 */
#ifndef MINIKERN_DRIVERS_PMU_JH7110_H
#define MINIKERN_DRIVERS_PMU_JH7110_H

#include <stdbool.h>
#include <stdint.h>

#include "core/fdt.h"

/* Domain bits in the current-mode register (`starfive,jh7110-pmu`). */
#define PMU_DOMAIN_GPUA (1u << 2)
#define PMU_DOMAIN_VOUT (1u << 4)

/* Finds and maps the PMU (`power-controller@17030000`). KERR_NOENT when the
 * tree does not describe one. */
int pmu_jh7110_init(const struct fdt *fdt);

/* The domains currently on, as bits. 0 before `pmu_jh7110_init`. */
uint32_t pmu_jh7110_domains_on(void);

/* Switches `domain` (one bit) on and waits until it reports on. KERR_OK when
 * it already was; KERR_TIMEDOUT when it never came, with the PMU's
 * interrupt status in `*why` for the log. */
int pmu_jh7110_power_on(uint32_t domain, uint32_t *why);

#endif
