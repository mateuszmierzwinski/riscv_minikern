#include "drivers/crg_jh7110.h"

#include <stddef.h>

#include "arch/riscv/mmu.h"
#include "core/console.h"
#include "core/errno.h"

#define CLOCK_ENABLE (1u << CRG_CLOCK_ENABLE_BIT)

/* Where each generator keeps its reset banks, relative to its own base.
 * They differ per domain, which is why they live in the table below rather
 * than in a constant. */
struct domain_layout {
    const char *compatible;
    uint32_t assert_offset;
    uint32_t status_offset;
    uint32_t resets;
};

static const struct domain_layout layouts[CRG_DOMAINS] = {
    {"starfive,jh7110-syscrg", 0x2f8u, 0x308u, 126u},
    {"starfive,jh7110-aoncrg", 0x038u, 0x03cu, 8u},
    {"starfive,jh7110-voutcrg", 0x048u, 0x04cu, 13u},
};

static struct {
    volatile uint32_t *regs;
    phys_t base;
    bool up;
} domains[CRG_DOMAINS];

#define CONFIRM_SPINS 100000u

static void write_reg(enum crg_domain d, uint32_t off, uint32_t v) {
    domains[d].regs[off / 4u] = v;
}

static uint32_t read_reg(enum crg_domain d, uint32_t off) {
    return domains[d].regs[off / 4u];
}

uint32_t crg_jh7110_init(const struct fdt *fdt) {
    uint32_t found = 0;
    for (uint32_t i = 0; i < CRG_DOMAINS; i++) {
        char path[64];
        phys_t base = 0;
        uint64_t size = 0;
        void *at;
        if (fdt_find_compatible(fdt, layouts[i].compatible, path, sizeof path) != KERR_OK ||
            fdt_device_reg(fdt, path, 0, &base, &size) != KERR_OK) {
            continue;
        }
        at = mmu_map_device(base, size);
        if (at == NULL) {
            continue;
        }
        domains[i].regs = (volatile uint32_t *)at;
        domains[i].base = base;
        domains[i].up = true;
        found++;
    }
    return found;
}

int crg_enable_clock(enum crg_domain domain, uint32_t index) {
    uint32_t off = crg_clock_offset(index);
    if (domain >= CRG_DOMAINS || !domains[domain].up) {
        return KERR_NOENT;
    }
    uint32_t before = read_reg(domain, off);
    write_reg(domain, off, before | CLOCK_ENABLE);
    /* Read back. An enable bit that does not stay set used to mean one
     * thing here — the parent is off — and "the difference between that and
     * 'I wrote it' is the difference between a diagnosis and a mystery".
     * That is still true, and it was not the whole truth. */
    if ((read_reg(domain, off) & CLOCK_ENABLE) != 0) {
        return KERR_OK;
    }
    /* **An inverter has no gate**, so its bit 31 can never stick: bit 30 is
     * the polarity of a clock that is always on, and the rest of the
     * register is empty. Reporting that as a failure refused both of this
     * board's Ethernet controllers, each of which names an inverted
     * transmit clock — and the refusal was indistinguishable from the real
     * fault this check exists to catch. Saying which it is costs one
     * comparison. */
    if ((before & (1u << CRG_CLOCK_INVERT_BIT)) != 0 &&
        (before & (CRG_CLOCK_MUX_MASK | CRG_CLOCK_DIV_MASK)) == 0) {
        return KERR_NOTSUP;
    }
    return KERR_IO;
}

volatile uint32_t *crg_regs(enum crg_domain domain) {
    return domain >= CRG_DOMAINS || !domains[domain].up ? NULL : domains[domain].regs;
}

int crg_release_reset(enum crg_domain domain, uint32_t id) {
    uint32_t bank, mask, assert_off, status_off;
    if (domain >= CRG_DOMAINS || !domains[domain].up) {
        return KERR_NOENT;
    }
    if (id >= layouts[domain].resets) {
        return KERR_INVAL;
    }
    bank = crg_reset_bank(id);
    mask = 1u << crg_reset_bit(id);
    assert_off = layouts[domain].assert_offset + bank;
    status_off = layouts[domain].status_offset + bank;
    /* Clearing the bit releases; setting it holds the block in reset. */
    write_reg(domain, assert_off, read_reg(domain, assert_off) & ~mask);
    for (uint32_t spin = 0; spin < CONFIRM_SPINS; spin++) {
        /* A released line reads back as 1 in the status bank. Confirmed
         * rather than assumed, because a block still in reset accepts every
         * write and ignores all of them. */
        if ((read_reg(domain, status_off) & mask) != 0) {
            return KERR_OK;
        }
    }
    return KERR_TIMEDOUT;
}

int crg_jh7110_attach(enum crg_domain domain, phys_t base, uint64_t size) {
    void *at;
    if (domain >= CRG_DOMAINS) {
        return KERR_INVAL;
    }
    if (domains[domain].up) {
        return KERR_OK;
    }
    at = mmu_map_device(base, size);
    if (at == NULL) {
        return KERR_NOMEM;
    }
    domains[domain].regs = (volatile uint32_t *)at;
    domains[domain].base = base;
    domains[domain].up = true;
    return KERR_OK;
}

/* Replaces the `mask` field of a clock's register with `value`, and reads
 * it back: a generator that is not powered accepts the write and keeps 0. */
static int set_field(enum crg_domain domain, uint32_t index, uint32_t mask, uint32_t value) {
    uint32_t off = crg_clock_offset(index);
    if (domain >= CRG_DOMAINS || !domains[domain].up) {
        return KERR_NOENT;
    }
    write_reg(domain, off, (read_reg(domain, off) & ~mask) | (value & mask));
    return (read_reg(domain, off) & mask) == (value & mask) ? KERR_OK : KERR_IO;
}

int crg_set_divider(enum crg_domain domain, uint32_t index, uint32_t divider) {
    if (divider == 0 || divider > CRG_CLOCK_DIV_MASK) {
        return KERR_INVAL;
    }
    return set_field(domain, index, CRG_CLOCK_DIV_MASK, divider);
}

int crg_select_parent(enum crg_domain domain, uint32_t index, uint32_t mux) {
    if (mux > (CRG_CLOCK_MUX_MASK >> 24)) {
        return KERR_INVAL;
    }
    return set_field(domain, index, CRG_CLOCK_MUX_MASK, mux << 24);
}

phys_t crg_base(enum crg_domain domain) {
    return domain < CRG_DOMAINS ? domains[domain].base : 0;
}

bool crg_present(enum crg_domain domain) {
    return domain < CRG_DOMAINS && domains[domain].up;
}

/* Which generator a phandle refers to. The tree's own numbering, resolved
 * rather than guessed: a device on the always-on generator whose clock was
 * switched on in the system one would have its neighbour started instead. */
static int domain_of_phandle(const struct fdt *fdt, uint32_t phandle, enum crg_domain *out) {
    char path[64];
    if (fdt_find_phandle(fdt, phandle, path, sizeof path) != KERR_OK) {
        return KERR_NOENT;
    }
    /* The system generator is the one at 0x13020000 and the always-on one at
     * 0x17000000; which is which comes from the address rather than the
     * node's name, because a name is a label and an address is a fact. */
    {
        phys_t base = 0;
        uint64_t size = 0;
        if (fdt_device_reg(fdt, path, 0, &base, &size) != KERR_OK) {
            return KERR_NOENT;
        }
        if (base == crg_base(CRG_SYS)) {
            *out = CRG_SYS;
            return KERR_OK;
        }
        if (base == crg_base(CRG_AON)) {
            *out = CRG_AON;
            return KERR_OK;
        }
    }
    return KERR_NOENT;
}

int crg_enable_device(const struct fdt *fdt, const char *path) {
    struct fdt_prop clocks, resets;
    bool any = false;
    int rc;
    if (fdt_get_prop(fdt, path, "clocks", &clocks) == KERR_OK) {
        /* Pairs of (phandle, index), so two cells each. */
        for (uint32_t at = 0; at + 1u < clocks.len / 4u; at += 2u) {
            uint64_t phandle = 0, index = 0;
            enum crg_domain domain;
            if (fdt_prop_cells(&clocks, at, 1, &phandle) != KERR_OK ||
                fdt_prop_cells(&clocks, at + 1u, 1, &index) != KERR_OK) {
                return KERR_INVAL;
            }
            if (domain_of_phandle(fdt, (uint32_t)phandle, &domain) != KERR_OK) {
                continue; /* a clock from some other provider: not ours to switch */
            }
            rc = crg_enable_clock(domain, (uint32_t)index);
            /* A clock with no gate is already running, which is as good as
             * having switched it on. */
            if (rc != KERR_OK && rc != KERR_NOTSUP) {
                return rc;
            }
            any = true;
        }
    }
    if (fdt_get_prop(fdt, path, "resets", &resets) == KERR_OK) {
        for (uint32_t at = 0; at + 1u < resets.len / 4u; at += 2u) {
            uint64_t phandle = 0, id = 0;
            enum crg_domain domain;
            if (fdt_prop_cells(&resets, at, 1, &phandle) != KERR_OK ||
                fdt_prop_cells(&resets, at + 1u, 1, &id) != KERR_OK) {
                return KERR_INVAL;
            }
            if (domain_of_phandle(fdt, (uint32_t)phandle, &domain) != KERR_OK) {
                continue;
            }
            rc = crg_release_reset(domain, (uint32_t)id);
            if (rc != KERR_OK) {
                return rc;
            }
            any = true;
        }
    }
    return any ? KERR_OK : KERR_NOENT;
}
