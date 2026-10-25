/* Reconnaissance for the board's own Ethernet, before any driver exists.
 *
 * **This asks the hardware what it is instead of assuming.** The one
 * expensive lesson in this tree's driver history is the SD controller: a
 * complete SDHCI driver was written and proved against an emulator, and the
 * board turned out to carry a DesignWare MSHC — "not a driver with a bug in
 * it; it was the wrong driver, and only hardware could say so". The GMAC is
 * a bigger block than that one, so the facts come first.
 *
 * Four questions, each of which changes what the driver must be:
 *
 *   - **which controller**, since the board has two and only one has a
 *     cable: the first bring-up measured `ethernet@16030000` timing out on
 *     PHY negotiation while `ethernet@16040000` negotiated, so a driver
 *     that bound the first would find a dead link on a working board;
 *   - **which generation**, because DesignWare's register map moved: a
 *     `dwmac-1000` keeps MDIO at 0x10/0x14 and its version at 0x20, while a
 *     `dwmac-4.x`/`5.x` keeps them at 0x200/0x204 and 0x110. Reading the
 *     wrong offset returns a plausible number, which is the failure this
 *     whole file exists to avoid;
 *   - **which PHY**, by its identifier over MDIO, because its reset and
 *     link setup are its own;
 *   - **whether the clocks come up**, which `crg_enable_device` answers and
 *     which has silently stopped a driver here before.
 *
 * It only reads, apart from enabling the clocks a driver would need anyway.
 */
#include "drivers/dwmac_probe.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/riscv/mmu.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/fdt.h"
#include "drivers/crg_jh7110.h"

/* The two places DesignWare has kept the version register, and the two it
 * has kept MDIO. Which pair is right is what the compatible string says and
 * what reading both is meant to corroborate. */
#define GMAC1000_VERSION 0x0020u
#define GMAC4_VERSION    0x0110u
#define GMAC1000_MII_ADDR 0x0010u
#define GMAC1000_MII_DATA 0x0014u
#define GMAC4_MII_ADDR    0x0200u
#define GMAC4_MII_DATA    0x0204u

/* Every clock or reset a node asks for, resolved to the node that provides
 * it and that node's own compatible string.
 *
 * **This is the question `crg_enable_device` answers only as yes or no.**
 * It returns `KERR_NOENT` when *none* of a node's clocks came from a
 * generator it recognises, which is what it said for both controllers — and
 * from outside that is indistinguishable from "the board has no clocks for
 * this". The JH7110 has more than the two generators that driver knows
 * (sys and aon): there are also stg, isp and vout. Rather than guess which
 * the Ethernet uses, this prints what the tree says and lets the board
 * answer — the same reason the version offsets are both printed below. */
static void say_providers(const struct fdt *fdt, const char *path, const char *prop) {
    struct fdt_prop p;
    if (fdt_get_prop(fdt, path, prop, &p) != KERR_OK) {
        kprintf("gmac:   %s: absent\n", prop);
        return;
    }
    for (uint32_t at = 0; at + 1u < p.len / 4u; at += 2u) {
        uint64_t phandle = 0, index = 0;
        char provider[128];
        struct fdt_prop compat;
        if (fdt_prop_cells(&p, at, 1, &phandle) != KERR_OK ||
            fdt_prop_cells(&p, at + 1u, 1, &index) != KERR_OK) {
            return;
        }
        if (fdt_find_phandle(fdt, (uint32_t)phandle, provider, sizeof provider) != KERR_OK) {
            kprintf("gmac:   %s[%u]: phandle %u resolves to nothing\n", prop, at / 2u,
                    (uint32_t)phandle);
            continue;
        }
        if (fdt_get_prop(fdt, provider, "compatible", &compat) != KERR_OK) {
            kprintf("gmac:   %s[%u]: index %u from %s\n", prop, at / 2u, (uint32_t)index,
                    provider);
            continue;
        }
        kprintf("gmac:   %s[%u]: index %u from %s (%s)\n", prop, at / 2u, (uint32_t)index,
                provider, (const char *)compat.value);
    }
}

/* Enables each of a node's clocks and resets **individually**, reporting
 * every one.
 *
 * `crg_enable_device` stops at the first failure and returns its code,
 * which is right for a driver and useless for reconnaissance: it turned
 * five clocks and two resets into a single `rc -5`. The interesting
 * question is *which* of them will not come up, because
 * `crg_enable_clock`'s failure means something specific — it writes the
 * enable bit and reads it back, and a bit that does not stay set means the
 * **parent** clock is off. One index naming its parent is the whole of what
 * this needs to learn. */
static bool path_is_aon(const char *path);

static bool try_each_clock(const struct fdt *fdt, const char *path) {
    static const char *const WHICH[] = {"clocks", "resets"};
    bool all = true;
    for (uint32_t w = 0; w < 2u; w++) {
        struct fdt_prop p;
        if (fdt_get_prop(fdt, path, WHICH[w], &p) != KERR_OK) {
            continue;
        }
        for (uint32_t at = 0; at + 1u < p.len / 4u; at += 2u) {
            uint64_t phandle = 0, index = 0;
            char provider[128];
            enum crg_domain domain;
            int rc;
            if (fdt_prop_cells(&p, at, 1, &phandle) != KERR_OK ||
                fdt_prop_cells(&p, at + 1u, 1, &index) != KERR_OK ||
                fdt_find_phandle(fdt, (uint32_t)phandle, provider, sizeof provider) !=
                    KERR_OK) {
                all = false;
                continue;
            }
            domain = path_is_aon(provider) ? CRG_AON : CRG_SYS;
            rc = w == 0 ? crg_enable_clock(domain, (uint32_t)index)
                        : crg_release_reset(domain, (uint32_t)index);
            kprintf("gmac:   %s %s[%u] index %u: rc %d%s\n",
                    w == 0 ? "enable" : "deassert", WHICH[w], at / 2u, (uint32_t)index, rc,
                    rc == KERR_NOTSUP  ? " (no gate: an inverter, already running)"
                    : rc == KERR_IO    ? " (the bit would not stay set)"
                                       : "");
            /* `KERR_NOTSUP` is a clock with no gate, which is running and
             * needs nothing — the same judgement `crg_enable_device` makes.
             * Counting it as a failure here left both controllers reported
             * as unclocked after the generator had been taught otherwise,
             * which would have read as the fix not working. */
            all = all && (rc == KERR_OK || rc == KERR_NOTSUP);
        }
    }
    return all;
}

/* Every clock register of a generator, read and decoded.
 *
 * **Reading is free, and this is the question that was about to be answered
 * by poking.** `crg_enable_clock` reported that one clock's enable bit will
 * not stay set, which means its parent is off — and the obvious next move,
 * enabling AON clocks until it holds, writes to a generator whose layout
 * nobody here has looked at. The registers themselves say more: a JH7110
 * clock register keeps its enable in bit 31 and, where the clock has one, a
 * parent selection and a divider in the low bits. So the mux field of the
 * clock that refuses names the parent it wants, and the enable bits of its
 * neighbours say which of them is already running.
 *
 * It writes nothing. A clock tree that has to be guessed at should be read
 * first, and this tree's own history is the argument: the always-on pin
 * controller was switched on with the *wrong generator's* constant and the
 * symptom was exactly this one — an enable bit that would not stay set. */
static void dump_clock_registers(enum crg_domain domain, const char *name, uint32_t count) {
    phys_t base = crg_base(domain);
    volatile uint32_t *regs;
    if (!crg_present(domain) || base == 0) {
        kprintf("gmac: %s generator is not present\n", name);
        return;
    }
    regs = mmu_map_device(base, 0x1000u);
    if (regs == NULL) {
        kprintf("gmac:   %llx could not be mapped\n", (unsigned long long)base);
        return;
    }
    kprintf("gmac: %s generator at %llx\n", name, (unsigned long long)base);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = regs[crg_clock_offset(i) / 4u];
        kprintf("gmac:   [%2u] %08x  %s mux %u div %u\n", i, v,
                (v & (1u << CRG_CLOCK_ENABLE_BIT)) != 0 ? "on " : "off",
                (v >> 24) & 0x3fu, v & 0xffffffu);
    }
}

/* Which generator a provider path names. The two the CRG driver knows are
 * told apart by address, because that is what the tree gives and the
 * compatible string has already been printed above. */
static bool path_is_aon(const char *path) {
    const char *at = path;
    while (*at != '\0') {
        if (at[0] == '1' && at[1] == '7' && at[2] == '0' && at[3] == '0') {
            return true; /* clock-controller@17000000 */
        }
        at++;
    }
    return false;
}

/* A property printed as text when it is text, because `phy-mode` and
 * `compatible` are the two that decide the most and are both strings. */
static void say_string_prop(const struct fdt *fdt, const char *path, const char *prop) {
    struct fdt_prop p;
    if (fdt_get_prop(fdt, path, prop, &p) != KERR_OK || p.len == 0) {
        kprintf("gmac:   %s: absent\n", prop);
        return;
    }
    kprintf("gmac:   %s: %s\n", prop, (const char *)p.value);
}

/* One MDIO read, in whichever register layout the caller names.
 *
 * The busy bit is polled rather than waited on with a timer: this runs once
 * at probe time, and a controller that never clears it is a fact worth
 * reporting rather than hanging on — so the loop is bounded and says so. */
static int mdio_read(volatile uint32_t *regs, uint32_t addr_off, uint32_t data_off,
                     uint32_t phy, uint32_t reg, uint32_t cr, uint16_t *value) {
    uint32_t cmd = (phy << 21) | (reg << 16) | (cr << 8) | (3u << 2) | 1u;
    regs[addr_off / 4u] = cmd;
    for (uint32_t i = 0; i < 100000u; i++) {
        if ((regs[addr_off / 4u] & 1u) == 0) {
            *value = (uint16_t)regs[data_off / 4u];
            return KERR_OK;
        }
    }
    return KERR_TIMEDOUT;
}

/* The PHY's identifier, which is two registers and is the thing that names
 * the part: a VisionFive 2 is expected to carry a Motorcomm YT8531
 * (0x4f51e91x), and anything else means the board is not the one this was
 * written for. */
static void say_phy(volatile uint32_t *regs, uint32_t addr_off, uint32_t data_off,
                    const char *which) {
    for (uint32_t phy = 0; phy < 32u; phy++) {
        uint16_t hi = 0, lo = 0;
        if (mdio_read(regs, addr_off, data_off, phy, 2u, 0, &hi) != KERR_OK) {
            kprintf("gmac:   MDIO (%s layout) never went idle\n", which);
            return;
        }
        if (mdio_read(regs, addr_off, data_off, phy, 3u, 0, &lo) != KERR_OK) {
            return;
        }
        if (hi == 0xffffu || (hi == 0 && lo == 0)) {
            continue; /* nothing answering at this address */
        }
        kprintf("gmac:   PHY at %u answers id %04x%04x (%s layout)\n", phy, hi, lo, which);
    }
}

/* The compatible strings a JH7110 might name its Ethernet with, and the
 * paths the first board bring-up actually logged.
 *
 * **Both, because matching is exact.** `fdt_find_compatible` walks a node's
 * whole `compatible` list but compares each entry in full, so asking for
 * `snps,dwmac` finds nothing on a board whose strings are
 * `starfive,jh7110-dwmac` and `snps,dwmac-5.20` — which is what the first
 * run of this probe reported, and a reasonable person could read as "the
 * board has no Ethernet". The paths are the belt to that braces: the
 * addresses came off this board's own tree, so if every compatible guess
 * misses, the nodes can still be found and their real strings printed. */
static const char *const CANDIDATE_COMPATIBLES[] = {
    "starfive,jh7110-dwmac", "snps,dwmac-5.20", "snps,dwmac-4.20a", "snps,dwmac",
    "snps,dwmac-3.70a",      "st,stm32-dwmac",
};

static const char *const CANDIDATE_PATHS[] = {
    "/soc/ethernet@16030000",
    "/soc/ethernet@16040000",
    "/soc/ethernet@16030000/mdio",
};

/* Everything worth knowing about one controller, given its path. */
static void say_controller(const struct fdt *fdt, const char *path);

int dwmac_probe(const struct fdt *fdt) {
    uint32_t found = 0;
    for (uint32_t c = 0; c < sizeof CANDIDATE_COMPATIBLES / sizeof CANDIDATE_COMPATIBLES[0];
         c++) {
        for (uint32_t n = 0; n < 4u; n++) {
            char path[128];
            if (fdt_find_compatible_nth(fdt, CANDIDATE_COMPATIBLES[c], n, path, sizeof path) !=
                KERR_OK) {
                break;
            }
            kprintf("gmac: found by compatible \"%s\"\n", CANDIDATE_COMPATIBLES[c]);
            say_controller(fdt, path);
            found++;
        }
    }
    /* And by path, for whatever the guesses missed. */
    for (uint32_t i = 0; found == 0 && i < sizeof CANDIDATE_PATHS / sizeof CANDIDATE_PATHS[0];
         i++) {
        struct fdt_prop p;
        if (fdt_get_prop(fdt, CANDIDATE_PATHS[i], "compatible", &p) != KERR_OK) {
            continue;
        }
        kprintf("gmac: found by path %s\n", CANDIDATE_PATHS[i]);
        say_controller(fdt, CANDIDATE_PATHS[i]);
        found++;
    }
    /* The AON generator's registers, because that is where the one clock
     * that refuses to come up lives (index 6). Sixteen is more than the
     * JH7110's AON domain has and costs nothing to over-read: the window is
     * the generator's own. */
    dump_clock_registers(CRG_AON, "aon", 16u);
    if (found == 0) {
        kprintf("gmac: nothing matched; tried %u compatible string(s) and %u path(s)\n",
                (uint32_t)(sizeof CANDIDATE_COMPATIBLES / sizeof CANDIDATE_COMPATIBLES[0]),
                (uint32_t)(sizeof CANDIDATE_PATHS / sizeof CANDIDATE_PATHS[0]));
        return KERR_NODEV;
    }
    return KERR_OK;
}

static void say_controller(const struct fdt *fdt, const char *path) {
    {
        phys_t base = 0;
        uint64_t size = 0;
        volatile uint32_t *regs;
        bool clocked;
        kprintf("gmac: %s\n", path);
        say_string_prop(fdt, path, "compatible");
        say_string_prop(fdt, path, "phy-mode");
        if (fdt_device_reg(fdt, path, 0, &base, &size) != KERR_OK) {
            kprintf("gmac:   no reg\n");
            return;
        }
        kprintf("gmac:   at %llx, %llu bytes\n", (unsigned long long)base,
                (unsigned long long)size);
        say_string_prop(fdt, path, "clock-names");
        say_providers(fdt, path, "clocks");
        say_providers(fdt, path, "resets");
        clocked = try_each_clock(fdt, path);
        if (!clocked) {
            kprintf("gmac:   not fully clocked; its registers may read as zeroes\n");
        }
        /* **The window has to be mapped before it can be read**, and this
         * is where a wrong explanation was written down before it was
         * checked.
         *
         * The first attempt to read these registers ended with the board
         * printing nothing further, and the conclusion recorded here was
         * that an unclocked peripheral hangs the hart — a bus transaction
         * with no completer, citing this tree's PCIe lesson as support.
         * That was invented. The log had a `PANIC: load page fault` with
         * `stval` pointing at this very window, and the filter used to read
         * the log did not match panic lines, so it was never seen.
         *
         * The actual cause is that nothing has mapped `0x1603_0000`: a
         * driver claims its window, and no driver has claimed this one. The
         * clocks were a red herring, and so was the hardware story. */
        /* **`NULL` on failure, not a status.** This read `!= KERR_OK`, and
         * when `mmu_map_device` began returning a pointer that compiled
         * perfectly while meaning the opposite — `KERR_OK` is 0 and so is
         * `NULL`, so a successful mapping became the failure branch. The
         * one call site in thirteen that did not have to change is the one
         * that silently broke. */
        regs = mmu_map_device(base, size);
        if (regs == NULL) {
            kprintf("gmac:   could not map its register window at %llx\n",
                    (unsigned long long)base);
            return;
        }
        /* **Both candidate offsets, and the board decides.** A DesignWare
         * version register holds a small revision in its low byte and its
         * complement-ish `snpsver` above; the wrong offset gives something
         * that does not look like that at all. */
        kprintf("gmac:   version@%x = %08x, version@%x = %08x\n", GMAC1000_VERSION,
                regs[GMAC1000_VERSION / 4u], GMAC4_VERSION, regs[GMAC4_VERSION / 4u]);
        say_phy(regs, GMAC4_MII_ADDR, GMAC4_MII_DATA, "dwmac4");
        say_phy(regs, GMAC1000_MII_ADDR, GMAC1000_MII_DATA, "dwmac1000");
    }
}
