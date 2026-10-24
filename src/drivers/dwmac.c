/* A DesignWare GMAC 5.20, as the JH7110 carries it. See the header for what
 * was measured before this was written, and `CLAUDE.md` §3 for how.
 *
 * **The register layout is the dwmac4 one**, which matters more than it
 * sounds: DesignWare moved MDIO from 0x10/0x14 to 0x200/0x204 and the
 * version register from 0x20 to 0x110 between generations, and the board
 * confirmed which by answering 0x4152 at the second and zero at the first.
 * Reading the wrong one returns a plausible number, which is the whole
 * reason the probe exists.
 */
#include "drivers/dwmac.h"

#include <stddef.h>

#include "arch/riscv/mmu.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/fdt.h"
#include "drivers/crg_jh7110.h"

/* The dwmac4 registers this driver touches. */
#define MAC_CONFIG        0x0000u
#define MAC_PACKET_FILTER 0x0008u
#define MAC_VERSION       0x0110u
#define MAC_MDIO_ADDR     0x0200u
#define MAC_MDIO_DATA     0x0204u
#define MAC_ADDR0_HIGH    0x0300u
#define MAC_ADDR0_LOW     0x0304u

#define MAC_CONFIG_RE    (1u << 0)  /* receiver enable */
#define MAC_CONFIG_TE    (1u << 1)  /* transmitter enable */
#define MAC_CONFIG_DM    (1u << 13) /* full duplex */
#define MAC_CONFIG_FES   (1u << 14) /* 100 Mbit when PS is set */
#define MAC_CONFIG_PS    (1u << 15) /* port select: MII/RMII rather than GMII */

#define MDIO_BUSY  (1u << 0)
#define MDIO_READ  (3u << 2)
#define MDIO_WRITE (1u << 2)

/* The PHY's standard registers, which every PHY has whatever the part. */
#define PHY_CONTROL 0u
#define PHY_STATUS  1u
#define PHY_ID_HIGH 2u
#define PHY_ID_LOW  3u
#define PHY_AUTONEG_ADV 4u
#define PHY_1000_CONTROL 9u

#define PHY_STATUS_LINK      (1u << 2)
#define PHY_STATUS_AUTONEG_DONE (1u << 5)
#define PHY_CONTROL_RESET    (1u << 15)
#define PHY_CONTROL_AUTONEG  (1u << 12)
#define PHY_CONTROL_RESTART  (1u << 9)

/* The one controller this driver drives, and what it needs to remember.
 *
 * Static rather than allocated because there is one of it: the board has
 * two controllers and only one has a cable, and binding both would mean
 * two drivers' worth of rings for a machine with one network. */
static struct {
    volatile uint32_t *regs;
    uint8_t mac[6];
    uint32_t phy;
    bool present;
    uint32_t mbit;
    bool full_duplex;
    const struct frame_sink *sink;
} nic;

static uint32_t rd(uint32_t off) {
    return nic.regs[off / 4u];
}

static void wr(uint32_t off, uint32_t value) {
    nic.regs[off / 4u] = value;
}

/* One MDIO transfer.
 *
 * The clock-range field (`CR`) divides the CSR clock down to MDIO's 2.5 MHz
 * ceiling. It is left at the reset default here deliberately: the probe
 * read a PHY identifier successfully with it, so it is known to work on
 * this board, and choosing a divider from a CSR frequency this driver does
 * not measure would be a guess replacing a measurement. */
static int mdio(uint32_t phy, uint32_t reg, uint32_t op, uint16_t *value) {
    uint32_t cmd = (phy << 21) | (reg << 16) | op | MDIO_BUSY;
    if (op == MDIO_WRITE) {
        wr(MAC_MDIO_DATA, *value);
    }
    wr(MAC_MDIO_ADDR, cmd);
    for (uint32_t i = 0; i < 100000u; i++) {
        if ((rd(MAC_MDIO_ADDR) & MDIO_BUSY) == 0) {
            if (op == MDIO_READ) {
                *value = (uint16_t)rd(MAC_MDIO_DATA);
            }
            return KERR_OK;
        }
    }
    return KERR_TIMEDOUT;
}

static int phy_read(uint32_t reg, uint16_t *value) {
    return mdio(nic.phy, reg, MDIO_READ, value);
}

static int phy_write(uint32_t reg, uint16_t value) {
    return mdio(nic.phy, reg, MDIO_WRITE, &value);
}

/* The lowest MDIO address that answers with an identifier.
 *
 * Searched rather than taken from the tree's `phy-handle`, because the
 * probe found the same PHY answering at **two** addresses (0 and 1) and a
 * driver that trusted one number would be right by luck. The identifier is
 * the evidence that something is there: an address with nothing on it reads
 * as all-ones or all-zeroes. */
static int find_phy(void) {
    for (uint32_t at = 0; at < 32u; at++) {
        uint16_t hi = 0, lo = 0;
        nic.phy = at;
        if (phy_read(PHY_ID_HIGH, &hi) != KERR_OK || phy_read(PHY_ID_LOW, &lo) != KERR_OK) {
            return KERR_TIMEDOUT;
        }
        if (hi == 0xffffu || (hi == 0 && lo == 0)) {
            continue;
        }
        kprintf("dwmac: PHY at %u, id %04x%04x\n", at, hi, lo);
        return KERR_OK;
    }
    return KERR_NODEV;
}

/* Resets the PHY and lets it negotiate.
 *
 * **Reset, then wait for the reset bit to clear itself**, which is the
 * part a driver skips and pays for: the bit is self-clearing and the PHY
 * ignores everything written while it is set, so configuring a PHY that is
 * still resetting writes into nothing. The bound is generous because a PHY
 * reset is milliseconds and this runs once. */
static int phy_reset(void) {
    uint16_t control = 0;
    int rc = phy_write(PHY_CONTROL, PHY_CONTROL_RESET);
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint32_t i = 0; i < 1000000u; i++) {
        rc = phy_read(PHY_CONTROL, &control);
        if (rc != KERR_OK) {
            return rc;
        }
        if ((control & PHY_CONTROL_RESET) == 0) {
            return phy_write(PHY_CONTROL, PHY_CONTROL_AUTONEG | PHY_CONTROL_RESTART);
        }
    }
    return KERR_TIMEDOUT;
}

/* Waits for the link, and reports what it came up as.
 *
 * **The link status bit is latching-low on every PHY**: it reads zero once
 * after a link has been lost, and reading it again gives the current state.
 * So it is read twice before being believed, which is the difference
 * between "the cable is out" and "the cable was out at some point since I
 * last asked".
 *
 * Speed comes from the PHY rather than being assumed, because the MAC has
 * to be told the same thing — a MAC configured for a gigabit link that
 * negotiated a hundred megabits sends at the wrong rate and nothing
 * arrives. */
static int wait_for_link(void) {
    for (uint32_t i = 0; i < 2000000u; i++) {
        uint16_t status = 0;
        if (phy_read(PHY_STATUS, &status) != KERR_OK) {
            return KERR_TIMEDOUT;
        }
        if (phy_read(PHY_STATUS, &status) != KERR_OK) {
            return KERR_TIMEDOUT;
        }
        if ((status & PHY_STATUS_LINK) != 0 && (status & PHY_STATUS_AUTONEG_DONE) != 0) {
            /* The YT8531's speed and duplex live in its own register 17,
             * which is what the probe identified it as. Standard register 10
             * says whether gigabit was agreed, and registers 5/4 the rest —
             * read in that order so the fastest agreement wins. */
            uint16_t gig = 0, partner = 0;
            nic.mbit = 10u;
            nic.full_duplex = false;
            if (phy_read(10u, &gig) == KERR_OK && (gig & 0x0800u) != 0) {
                nic.mbit = 1000u;
                nic.full_duplex = true;
            } else if (phy_read(5u, &partner) == KERR_OK) {
                nic.mbit = (partner & 0x0180u) != 0 ? 100u : 10u;
                nic.full_duplex = (partner & 0x0140u) != 0;
            }
            return KERR_OK;
        }
    }
    return KERR_TIMEDOUT;
}

/* Tells the MAC what the PHY agreed to.
 *
 * `PS` and `FES` together name the speed: a gigabit link clears `PS`
 * (GMII), while 10 and 100 set it and are told apart by `FES`. Getting this
 * wrong does not fail loudly — the MAC runs at a rate the PHY is not using
 * and every frame is lost. */
static void configure_mac(void) {
    uint32_t cfg = rd(MAC_CONFIG);
    cfg &= ~(MAC_CONFIG_PS | MAC_CONFIG_FES | MAC_CONFIG_DM);
    if (nic.mbit != 1000u) {
        cfg |= MAC_CONFIG_PS;
        cfg |= nic.mbit == 100u ? MAC_CONFIG_FES : 0u;
    }
    cfg |= nic.full_duplex ? MAC_CONFIG_DM : 0u;
    wr(MAC_CONFIG, cfg);
}

/* The MAC address: the one the tree gives, or the one already in the
 * controller's registers.
 *
 * The firmware programmed those registers while it was using the device for
 * its own DHCP, so they hold the board's real address — and taking it from
 * there rather than inventing one means this kernel appears on the network
 * as the same machine U-Boot did. A made-up address would work and would be
 * a different host every boot. */
static void take_address(const struct fdt *fdt, const char *path) {
    struct fdt_prop p;
    uint32_t high = rd(MAC_ADDR0_HIGH);
    uint32_t low = rd(MAC_ADDR0_LOW);
    if (fdt_get_prop(fdt, path, "local-mac-address", &p) == KERR_OK && p.len == 6u) {
        for (uint32_t i = 0; i < 6u; i++) {
            nic.mac[i] = p.value[i];
        }
        wr(MAC_ADDR0_LOW, (uint32_t)nic.mac[0] | ((uint32_t)nic.mac[1] << 8) |
                              ((uint32_t)nic.mac[2] << 16) | ((uint32_t)nic.mac[3] << 24));
        wr(MAC_ADDR0_HIGH, (uint32_t)nic.mac[4] | ((uint32_t)nic.mac[5] << 8));
        return;
    }
    nic.mac[0] = (uint8_t)low;
    nic.mac[1] = (uint8_t)(low >> 8);
    nic.mac[2] = (uint8_t)(low >> 16);
    nic.mac[3] = (uint8_t)(low >> 24);
    nic.mac[4] = (uint8_t)high;
    nic.mac[5] = (uint8_t)(high >> 8);
}

void dwmac_address(uint8_t out[6]) {
    for (uint32_t i = 0; i < 6u; i++) {
        out[i] = nic.mac[i];
    }
}

bool dwmac_link_up(uint32_t *mbit, bool *full_duplex) {
    if (!nic.present) {
        return false;
    }
    *mbit = nic.mbit;
    *full_duplex = nic.full_duplex;
    return nic.mbit != 0;
}

void dwmac_set_sink(const struct frame_sink *sink) {
    nic.sink = sink;
}

/* ── bringing one up ─────────────────────────────────────────────────── */

/* The controller to use, which is **not** simply the first one found.
 *
 * The board has two and only one has a cable: the first bring-up measured
 * `ethernet@16030000` timing out on PHY negotiation while `@16040000`
 * negotiated a DHCP lease. So the second is tried first, and the first is
 * the fallback — a driver that bound them in tree order would report a dead
 * link on a working board, and that is a day of debugging the wrong layer.
 */
static const char *const CONTROLLERS[] = {
    "/soc/ethernet@16040000",
    "/soc/ethernet@16030000",
};

/* Everything that has to happen before a register can be read, in the order
 * the hardware requires it.
 *
 * The order is not a preference. The window must be **mapped** or the first
 * read is a page fault — nothing maps a peripheral a driver has not claimed,
 * which cost a board run and a wrong theory. The clocks must be **on** or
 * the registers read as zeroes. And `crg_enable_device` must be allowed to
 * answer "this clock has no gate" without that counting as failure, which is
 * what refused both of these controllers until the generator learned the
 * difference. */
static int open_controller(const struct fdt *fdt, const char *path) {
    phys_t base = 0;
    uint64_t size = 0;
    int rc = fdt_device_reg(fdt, path, 0, &base, &size);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = mmu_map_device(base, size);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = crg_enable_device(fdt, path);
    if (rc != KERR_OK) {
        return rc;
    }
    nic.regs = (volatile uint32_t *)(uintptr_t)base;
    /* The version register is read back as a cross-check that all of the
     * above worked: a block that is mapped but unclocked reads zero here,
     * and this driver would rather say so than configure a silent device. */
    if ((rd(MAC_VERSION) & 0xffu) == 0) {
        kprintf("dwmac: %s reads version 0 — mapped but not running\n", path);
        return KERR_IO;
    }
    return KERR_OK;
}

int dwmac_init(const struct fdt *fdt, const struct netdev_port **port) {
    (void)port;
    for (uint32_t i = 0; i < sizeof CONTROLLERS / sizeof CONTROLLERS[0]; i++) {
        struct fdt_prop exists;
        int rc;
        if (fdt_get_prop(fdt, CONTROLLERS[i], "compatible", &exists) != KERR_OK) {
            continue;
        }
        rc = open_controller(fdt, CONTROLLERS[i]);
        if (rc != KERR_OK) {
            kprintf("dwmac: %s would not start (rc %d)\n", CONTROLLERS[i], rc);
            continue;
        }
        kprintf("dwmac: %s, version %08x\n", CONTROLLERS[i], rd(MAC_VERSION));
        take_address(fdt, CONTROLLERS[i]);
        kprintf("dwmac: address %02x:%02x:%02x:%02x:%02x:%02x\n", nic.mac[0], nic.mac[1],
                nic.mac[2], nic.mac[3], nic.mac[4], nic.mac[5]);
        if (find_phy() != KERR_OK) {
            kprintf("dwmac: no PHY answered\n");
            continue;
        }
        if (phy_reset() != KERR_OK) {
            kprintf("dwmac: the PHY would not come out of reset\n");
            continue;
        }
        if (wait_for_link() != KERR_OK) {
            kprintf("dwmac: no link on %s\n", CONTROLLERS[i]);
            continue; /* the other controller may be the one with the cable */
        }
        configure_mac();
        nic.present = true;
        kprintf("dwmac: link up at %u Mbit, %s duplex\n", nic.mbit,
                nic.full_duplex ? "full" : "half");
        return KERR_OK;
    }
    return KERR_NODEV;
}

uint32_t dwmac_poll(void) {
    return 0; /* the descriptor rings are the next step */
}
