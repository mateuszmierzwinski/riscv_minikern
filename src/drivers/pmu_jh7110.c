#include "drivers/pmu_jh7110.h"

#include <stddef.h>

#include "arch/riscv/mmu.h"
#include "core/errno.h"
#include "drivers/mmio_poll.h"

#define PMU_COMPATIBLE "starfive,jh7110-pmu"

/* Registers, as byte offsets (docs/DISPLAY.md §2.1). */
#define SW_TURN_ON_POWER 0x0cu
#define SW_ENCOURAGE 0x44u
#define CURR_POWER_MODE 0x80u
#define INT_STATUS 0x8cu

/* The encourage codes: a reset of the command parser, then the two-word
 * "power on" sequence. */
#define ENCOURAGE_RESET 0xffu
#define ENCOURAGE_ON_FIRST 0x05u
#define ENCOURAGE_ON_SECOND 0x50u

/* The domain reports on within 100 µs in every implementation read; the
 * vendor then waits 50 ms before touching anything inside it. */
#define POWER_ON_LIMIT_NS 1000000ull
#define SETTLE_NS 50000000ull

static volatile uint32_t *regs;

static volatile uint32_t *reg(uint32_t offset) {
    return regs + offset / 4u;
}

int pmu_jh7110_init(const struct fdt *fdt) {
    char path[64];
    phys_t base = 0;
    uint64_t size = 0;
    void *at;
    if (fdt_find_compatible(fdt, PMU_COMPATIBLE, path, sizeof path) != KERR_OK ||
        fdt_device_reg(fdt, path, 0, &base, &size) != KERR_OK) {
        return KERR_NOENT;
    }
    at = mmu_map_device(base, size);
    if (at == NULL) {
        return KERR_NOMEM;
    }
    regs = at;
    return KERR_OK;
}

uint32_t pmu_jh7110_domains_on(void) {
    return regs == NULL ? 0u : *reg(CURR_POWER_MODE);
}

int pmu_jh7110_power_on(uint32_t domain, uint32_t *why) {
    uint32_t last = 0;
    *why = 0;
    if (regs == NULL) {
        return KERR_NODEV;
    }
    if ((*reg(CURR_POWER_MODE) & domain) != 0) {
        return KERR_OK;
    }
    *reg(SW_TURN_ON_POWER) = domain; /* a 1 only for the domain wanted */
    *reg(SW_ENCOURAGE) = ENCOURAGE_RESET;
    *reg(SW_ENCOURAGE) = ENCOURAGE_ON_FIRST;
    *reg(SW_ENCOURAGE) = ENCOURAGE_ON_SECOND;
    if (!mmio_poll(reg(CURR_POWER_MODE), domain, domain, POWER_ON_LIMIT_NS, &last)) {
        *why = *reg(INT_STATUS);
        return KERR_TIMEDOUT;
    }
    mmio_spin_ns(SETTLE_NS);
    return KERR_OK;
}
