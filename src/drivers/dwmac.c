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
#include "kernel/mem.h"

/* The dwmac4 registers this driver touches. */
#define MAC_CONFIG        0x0000u
#define MAC_PACKET_FILTER 0x0008u
/* Receive-filter bits. `PR` takes everything on the wire, which is a
 * diagnostic rather than a mode to run in: it answers "is this frame not
 * arriving, or arriving and being filtered out", and those two have entirely
 * different fixes. */
#define MAC_FILTER_PR (1u << 0)
#define MAC_VERSION       0x0110u
#define MAC_MDIO_ADDR     0x0200u
#define MAC_MDIO_DATA     0x0204u
#define MAC_ADDR0_HIGH    0x0300u
/* **Address Enable, and the bit this driver was clearing.** The high half of
 * a MAC address register carries two bytes of address and, at bit 31, the
 * flag that says the filter should compare against it. Writing the address
 * as a bare 16-bit value therefore wrote a zero there and switched its own
 * address off: broadcast frames still arrived, because a broadcast is not
 * matched against an address, so DHCP worked perfectly and every **unicast**
 * reply was dropped by the controller. From outside that is a machine that
 * can get a lease and then reach nothing — no ARP reply from the gateway, no
 * DNS answer — with every counter in the stack reading zero drops, because
 * the frames never got as far as the stack. */
#define MAC_ADDR_AE       (1u << 31)
#define MAC_ADDR0_LOW     0x0304u

#define MAC_CONFIG_RE    (1u << 0)  /* receiver enable */
#define MAC_CONFIG_TE    (1u << 1)  /* transmitter enable */
#define MAC_CONFIG_DM    (1u << 13) /* full duplex */
#define MAC_CONFIG_FES   (1u << 14) /* 100 Mbit when PS is set */
#define MAC_CONFIG_PS    (1u << 15) /* port select: MII/RMII rather than GMII */
#define MAC_CONFIG_LM    (1u << 12) /* loopback: the MAC feeds itself */
#define MAC_CONFIG_ACS   (1u << 20) /* strip pad and CRC from length-field frames */
#define MAC_CONFIG_CST   (1u << 21) /* strip CRC from type-field (Ethernet II) frames */

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

/* The DMA and queue registers, which the dwmac4 map puts above 0x1000. */
#define MTL_OPMODE        0x0c00u
#define MTL_TXQ0_OPMODE   0x0d00u
#define MTL_RXQ0_OPMODE   0x0d30u
#define DMA_BUS_MODE      0x1000u
#define DMA_SYSBUS_MODE   0x1004u
#define DMA_CH0_CONTROL   0x1100u
#define DMA_CH0_TX_CTRL   0x1104u
#define DMA_CH0_RX_CTRL   0x1108u
#define DMA_CH0_TXLIST_HI 0x1110u
#define DMA_CH0_TXLIST    0x1114u
#define DMA_CH0_RXLIST_HI 0x1118u
#define DMA_CH0_RXLIST    0x111cu
#define DMA_CH0_TXTAIL    0x1120u
#define DMA_CH0_RXTAIL    0x1128u
#define DMA_CH0_TXRINGLEN 0x112cu
#define DMA_CH0_RXRINGLEN 0x1130u
#define DMA_CH0_STATUS    0x1160u
#define DMA_DEBUG_STATUS0 0x100cu /* bits 11:8: the receive DMA's state */
#define DMA_CH0_CUR_RXDESC 0x114cu
#define MTL_RXQ0_MISSED   0x0d34u /* overflow and missed-packet counts */
#define MTL_RXQ0_DEBUG    0x0d38u /* bits 29:16 packets in the queue, 5:4 fill level */
/* DWMAC4 per-channel interrupt enable, and the bits of it this driver wants.
 * `NIE` is the master enable for the "normal" group — without it the
 * individual bits are armed and nothing is ever raised, which is the same
 * shape as the `sie` bit that left this kernel's whole PLIC path unreachable. */
#define DMA_CH0_INTR_ENA  0x1134u
#define DMA_INTR_RIE      (1u << 6)  /* a frame was received */
#define DMA_INTR_NIE      (1u << 15) /* the normal-interrupt summary */
/* Write-one-to-clear, like the SD controller's `RINTSTS`: a handler that does
 * not acknowledge at the device leaves the PLIC re-raising for ever. */
#define DMA_STATUS_RI     (1u << 6)
#define DMA_STATUS_RBU    (1u << 7)  /* receive buffer unavailable: the DMA suspended */
#define DMA_STATUS_NIS    (1u << 15)

#define DMA_BUS_MODE_SWR  (1u << 0)
/* `DMA_SYSBUS_MODE`, and the one bit that decides whether a descriptor's
 * high address word means anything.
 *
 * **EAME belongs here, and it was not the bug it was added for.** This
 * board has 8 GiB of DRAM from `0x4000_0000`, so the page allocator *can*
 * return a buffer above 4 GiB, and without enhanced addressing the
 * controller would ignore the high word and write the frame to the low 32
 * bits instead. That is worth preventing. But it was not what was wrong:
 * the rings and buffers actually came back at `0x4084_6000` and nearby,
 * inside 32 bits, which the driver now prints — and printing them is what
 * refuted the theory in the same run that tested it.
 *
 * The status bits were misread too. In this generation **bit 12** is the
 * fatal bus error; bit 10 is the early transmit interrupt. `0x405` is
 * `TI | TBU | ETI`: transmit finished, its ring is empty, and nothing is
 * wrong anywhere.
 *
 * `AAL` keeps bursts address-aligned, which the fixed-burst setting below
 * assumes. */
#define DMA_SYS_FB   (1u << 0)
#define DMA_SYS_EAME (1u << 11)
#define DMA_SYS_AAL  (1u << 12)
#define DMA_TX_START      (1u << 0)
#define DMA_RX_START      (1u << 0)
#define MTL_TXQ_ENABLE    (2u << 2)
#define MTL_TXQ_STORE_FWD (1u << 1)
#define MTL_RXQ_STORE_FWD (1u << 5)

/* A descriptor's words, as dwmac4 defines them. The fourth word carries the
 * ownership bit, which is the entire synchronisation between this driver and
 * the controller: set means the hardware owns it. */
#define DESC_OWN   (1u << 31)
#define DESC_TX_FD (1u << 29) /* first descriptor of a frame */
#define DESC_TX_LD (1u << 28) /* and last */
#define DESC_TX_IOC (1u << 31) /* in word 2: interrupt on completion */
#define DESC_RX_BUF1V (1u << 24)
#define DESC_RX_IOC   (1u << 30)
#define DESC_RX_ERROR (1u << 15)

/* Eight descriptors each way and 2 KiB a buffer.
 *
 * Eight because a ring must be a power of two for the tail pointer
 * arithmetic to wrap cleanly, and because this kernel polls: a deeper ring
 * only helps a driver that can be interrupted while the hardware runs
 * ahead. 2 KiB holds any Ethernet frame this link will carry and is what
 * `RBSZ` is set to — a buffer smaller than a frame makes the controller
 * split it across descriptors, which is a feature nobody here needs. */
#define RING 64u /* descriptors each way: 8 overflowed under a 12 MB/s netfs burst and the hardware drops silently; a 4 KiB page holds 256 */
#define BUFFER 2048u

/* The one controller this driver drives, and what it needs to remember.
 *
 * Static rather than allocated because there is one of it: the board has
 * two controllers and only one has a cable, and binding both would mean
 * two drivers' worth of rings for a machine with one network. */
static struct {
    volatile uint32_t *regs;
    /* The rings' **bus** addresses, for the registers and the descriptors
     * that name them. `txd`/`rxd` below are the same memory as this kernel
     * reaches it. */
    phys_t txd_phys;
    phys_t rxd_phys;
    uint8_t mac[6];
    uint32_t phy;
    /* The PLIC source from this node's `interrupts`, kept so that the
     * composition root can register a handler without re-reading the tree. */
    uint32_t irq;
    /* Logs each accepted frame's destination address, for the one question
     * "what is the reply addressed to". Diagnostic only. */
    bool trace_dest;
    uint32_t traced;
    /* Set before probing to keep the documented-but-broken strict filter. */
    bool strict_filter;
    bool present;
    bool noncoherent;
    uint32_t mbit;
    bool full_duplex;
    const struct frame_sink *sink;
    /* The rings and their buffers, in memory the controller reads and
     * writes directly. **Physically contiguous and page-aligned**, which is
     * why they come from the page allocator rather than the heap: the
     * controller is given a physical address and walks the ring itself, so
     * a ring split across pages that the heap happened to place apart would
     * have it reading whatever lies between them. */
    volatile uint32_t *txd;
    volatile uint32_t *rxd;
    uint8_t *txb;
    uint8_t *rxb;
    uint32_t tx_next;
    uint32_t rx_next;
    uint64_t sent;
    uint64_t received;
    uint64_t dropped;
    uint64_t rx_resumed; /* receive DMA found suspended (RBU) and restarted */
    uint32_t polling;    /* callers inside dwmac_poll now */
    uint64_t overlaps;   /* entries while another caller was already inside */
    uint64_t tx_full; /* frames given up because the transmit ring stayed full */
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

/* A Motorcomm PHY's extended register: its address through 0x1e, its value
 * through 0x1f. */
static uint16_t phy_read_ext(uint16_t reg);

/* Rewrites the bits `mask` of an extended register to `value`. */
static int phy_modify_ext(uint16_t reg, uint16_t mask, uint16_t value) {
    uint16_t now = phy_read_ext(reg);
    int rc = phy_write(0x1eu, reg);
    return rc != KERR_OK ? rc : phy_write(0x1fu, (uint16_t)((now & ~mask) | (value & mask)));
}

static uint16_t phy_read_ext(uint16_t reg) {
    uint16_t v = 0xffffu;
    if (phy_write(0x1eu, reg) == KERR_OK) {
        (void)phy_read(0x1fu, &v);
    }
    return v;
}

static void dump_phy(void) {
    static const uint16_t ext[] = {0xa000u, 0xa001u, 0xa003u, 0xa010u, 0xa012u};
    uint16_t v[3] = {0};
    (void)phy_read(0u, &v[0]);
    (void)phy_read(1u, &v[1]);
    (void)phy_read(0x11u, &v[2]);
    kprintf("gmacdump: phy bmcr %04x bmsr %04x status %04x\n", v[0], v[1], v[2]);
    for (uint32_t i = 0; i < sizeof ext / sizeof ext[0]; i++) {
        kprintf("gmacdump: phy ext %04x = %04x\n", ext[i], phy_read_ext(ext[i]));
    }
}

/* syscrg's GMAC1 clock registers, by index. */
static void dump_gmac1_clocks(void) {
    volatile uint32_t *regs = crg_regs(CRG_SYS);
    if (regs == NULL) {
        return;
    }
    for (uint32_t i = 95u; i <= 112u; i++) {
        kprintf("gmacdump: syscrg[%u] = %08x\n", i, regs[crg_clock_offset(i) / 4u]);
    }
}

/* sys_syscon around the GMAC1 interface select (0x90). */
static void dump_syscon(void) {
    volatile uint32_t *syscon = mmu_map_device(0x13030000u, 0x1000u);
    if (syscon == NULL) {
        return;
    }
    for (uint32_t off = 0x80u; off <= 0x9cu; off += 4u) {
        kprintf("gmacdump: sys_syscon+%02x = %08x\n", off, syscon[off / 4u]);
    }
}

void dwmac_dump_bringup(void) {
    if (!nic.present) {
        return;
    }
    dump_phy();
    dump_gmac1_clocks();
    dump_syscon();
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

#define YT_CHIP_CONFIG 0xa001u
#define YT_RXC_DLY_EN (1u << 8)    /* a fixed ~2 ns receive-clock delay */
#define YT_RGMII_CONFIG1 0xa003u
#define YT_RX_DELAY_SHIFT 10u      /* bits 13:10, 150 ps a step */
#define YT_FE_TX_DELAY_MASK 0x000fu /* bits 3:0, 10/100 transmit, 150 ps a step */
#define YT_PAD_DRIVE 0xa010u
#define YT_RX_CLK_DRIVE_SHIFT 13u  /* bits 15:13 */
#define YT_DELAY_STEP_PS 150u
#define YT_FIXED_DELAY_PS 1900u

/* A device-tree number from the PHY node, or `fallback`. */
static uint32_t phy_prop(const struct fdt *fdt, const char *path, const char *name,
                         uint32_t fallback) {
    struct fdt_prop p;
    uint64_t v = 0;
    if (path == NULL || fdt_get_prop(fdt, path, name, &p) != KERR_OK ||
        fdt_prop_cells(&p, 0, 1, &v) != KERR_OK) {
        return fallback;
    }
    return (uint32_t)v;
}

/* The node the controller's `phy-handle` names, into `out`; NULL if none. */
static const char *phy_node(const struct fdt *fdt, const char *ctrl, char *out, uint32_t cap) {
    struct fdt_prop p;
    uint64_t handle = 0;
    if (fdt_get_prop(fdt, ctrl, "phy-handle", &p) != KERR_OK ||
        fdt_prop_cells(&p, 0, 1, &handle) != KERR_OK ||
        fdt_find_phandle(fdt, (uint32_t)handle, out, cap) != KERR_OK) {
        return NULL;
    }
    return out;
}

/* The YT8531's drive-strength code for a current, from its 1.8 V table:
 * the largest step not above `microamp`. */
static uint16_t yt_drive_code(uint32_t microamp) {
    static const uint16_t table[8] = {1200, 2100, 2700, 2910, 3110, 3600, 3970, 4350};
    uint16_t code = 0;
    for (uint16_t i = 0; i < 8u; i++) {
        code = table[i] <= microamp ? i : code;
    }
    return code;
}

/* The receive-clock delay the tree asks for (`rx-internal-delay-ps`):
 * under 1.9 ns it is the fine delay alone, otherwise the fixed 2 ns path
 * plus the rest in fine steps. */
static int yt_set_rx_delay(uint32_t ps) {
    bool fixed = ps >= YT_FIXED_DELAY_PS;
    uint16_t steps = (uint16_t)(((fixed ? ps - YT_FIXED_DELAY_PS : ps) + YT_DELAY_STEP_PS / 2u) /
                                YT_DELAY_STEP_PS);
    int rc = phy_modify_ext(YT_CHIP_CONFIG, YT_RXC_DLY_EN, fixed ? YT_RXC_DLY_EN : 0);
    if (steps > 0xfu) {
        steps = 0xfu;
    }
    return rc != KERR_OK ? rc
                         : phy_modify_ext(YT_RGMII_CONFIG1, (uint16_t)(0xfu << YT_RX_DELAY_SHIFT),
                                          (uint16_t)(steps << YT_RX_DELAY_SHIFT));
}

/* **The PHY's RGMII timing, from the tree — what U-Boot used to do for us.**
 *
 * Out of reset the YT8531 adds a fixed ~2 ns to the receive clock; this
 * board's GMAC1 wiring wants 300 ps (`rx-internal-delay-ps`). A netboot
 * never noticed, because U-Boot had programmed the PHY before the kernel
 * ran; booting from the SD card with U-Boot's network untouched, the
 * controller received **nothing** — 3 DHCP requests sent, 0 frames in,
 * the receive DMA idle with every buffer free. Measured warm against cold
 * (`minikern.gmacdump`): 0xa001 8020/8120, 0xa003 08f0/00f1, 0xa010
 * cbff/6bff. The gigabit transmit delay is left at its reset value, as
 * U-Boot leaves it. Applied before the PHY's reset, which keeps them. */
static void phy_apply_tree_timing(const struct fdt *fdt, const char *ctrl) {
    char path[128];
    const char *node = phy_node(fdt, ctrl, path, sizeof path);
    if (node == NULL || phy_prop(fdt, node, "rx-internal-delay-ps", ~0u) == ~0u) {
        return; /* nothing asked for: the PHY keeps whatever it has */
    }
    (void)yt_set_rx_delay(phy_prop(fdt, node, "rx-internal-delay-ps", 0));
    (void)phy_modify_ext(YT_RGMII_CONFIG1, YT_FE_TX_DELAY_MASK,
                         (uint16_t)(phy_prop(fdt, node, "tx-internal-delay-ps", 0) /
                                    YT_DELAY_STEP_PS));
    (void)phy_modify_ext(YT_PAD_DRIVE, (uint16_t)(0x7u << YT_RX_CLK_DRIVE_SHIFT),
                         (uint16_t)(yt_drive_code(phy_prop(fdt, node,
                                                           "motorcomm,rx-clk-drv-microamp", 0))
                                    << YT_RX_CLK_DRIVE_SHIFT));
    kprintf("dwmac: PHY timing from the tree: rx delay %u ps, tx delay %u ps\n",
            phy_prop(fdt, node, "rx-internal-delay-ps", 0),
            phy_prop(fdt, node, "tx-internal-delay-ps", 0));
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
 * and every frame is lost.
 *
 * **The CRC is stripped** (`CST`, `ACS`). Left on, a received length counts
 * the 4-byte FCS, so a full-size frame reads as 1518 bytes and the stack
 * drops it as malformed: small replies worked and every bulk TCP transfer
 * stalled, which is how netfs found it. */
static void configure_mac(void) {
    uint32_t cfg = rd(MAC_CONFIG);
    cfg &= ~(MAC_CONFIG_PS | MAC_CONFIG_FES | MAC_CONFIG_DM);
    if (nic.mbit != 1000u) {
        cfg |= MAC_CONFIG_PS;
        cfg |= nic.mbit == 100u ? MAC_CONFIG_FES : 0u;
    }
    cfg |= nic.full_duplex ? MAC_CONFIG_DM : 0u;
    cfg |= MAC_CONFIG_CST | MAC_CONFIG_ACS;
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
/* The receive filter, set deliberately.
 *
 * It was never written at all, so it held whatever the firmware left — which
 * is not a default, it is the previous program's state. Zero is what this
 * driver wants: match unicast against the address registers, accept
 * broadcast, drop the rest. Written explicitly so that the next person
 * reading this knows it was chosen. */
/* **Every frame on the wire, and this is not where that was meant to land.**
 *
 * The filter is supposed to be zero here: match unicast against
 * `MAC_ADDR0`, accept broadcast, drop the rest. Measured on the board, that
 * configuration accepts **no unicast at all** — and the registers in the
 * failing run are byte-identical to the working one:
 *
 *     dwmac: filter 00000000, addr 800089b6/0039cf6c, ours 6c:cf:39:00:b6:89
 *
 * `AE` (bit 31) is set, the address is exactly this interface's, the mask
 * bytes are clear, and the reply that never arrives is addressed to
 * `6c:cf:39:00:b6:89`. With `PR` set, the same boot receives the ARP reply
 * and the DNS answer and a name resolves. So this controller drops unicast
 * that matches a correctly programmed address, which is not how the filter
 * is documented to behave, and the cause is **not identified** — it wants
 * the DWMAC 5.20 configuration detail for this SoC, which is not to hand.
 *
 * Why accept everything rather than leave it strict and broken: with the
 * filter closed this board gets a DHCP lease and can then reach nothing —
 * no ARP reply, so no gateway, so no traffic at all. A network that cannot
 * receive a reply is not a network. The cost of `PR` on this board is that
 * the controller hands up frames for other hosts and the stack discards
 * them by address, which costs cycles and leaks nothing: the stack's own
 * `not-ours` counter is where they go.
 *
 * `minikern.netfilter` restores the strict setting for whoever picks this
 * up, because the next person needs to reproduce it in one boot rather than
 * rediscover it. */
static void set_the_receive_filter(void) {
    wr(MAC_PACKET_FILTER, nic.strict_filter ? 0u : MAC_FILTER_PR);
}

/* Programs the address this driver decided on, with the enable bit.
 *
 * **Unconditionally, whichever source the address came from.** It used to be
 * written only when the tree carried a `local-mac-address`, on the reasoning
 * that an address read back *from* the registers is already in them — which
 * is true of the address and false of `AE`, the bit beside it that says the
 * filter should use it. So on a board whose tree names no address, the
 * enable bit stayed as the firmware left it and nothing in this driver ever
 * set it. Writing both registers in one place is what makes the two paths
 * impossible to tell apart, which is the only way this stays fixed. */
static void program_the_address(void) {
    wr(MAC_ADDR0_LOW, (uint32_t)nic.mac[0] | ((uint32_t)nic.mac[1] << 8) |
                          ((uint32_t)nic.mac[2] << 16) | ((uint32_t)nic.mac[3] << 24));
    wr(MAC_ADDR0_HIGH, (uint32_t)nic.mac[4] | ((uint32_t)nic.mac[5] << 8) | MAC_ADDR_AE);
}

static void take_address(const struct fdt *fdt, const char *path) {
    struct fdt_prop p;
    uint32_t high = rd(MAC_ADDR0_HIGH);
    uint32_t low = rd(MAC_ADDR0_LOW);
    set_the_receive_filter();
    if (fdt_get_prop(fdt, path, "local-mac-address", &p) == KERR_OK && p.len == 6u) {
        for (uint32_t i = 0; i < 6u; i++) {
            nic.mac[i] = p.value[i];
        }
    } else {
        nic.mac[0] = (uint8_t)low;
        nic.mac[1] = (uint8_t)(low >> 8);
        nic.mac[2] = (uint8_t)(low >> 16);
        nic.mac[3] = (uint8_t)(low >> 24);
        nic.mac[4] = (uint8_t)high;
        nic.mac[5] = (uint8_t)(high >> 8);
    }
    program_the_address();
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

/* ── the descriptor rings ────────────────────────────────────────────── */

/* One receive descriptor, handed back to the controller.
 *
 * The buffer address goes in first and the ownership bit **last**, because
 * the controller may act on the descriptor the instant it sees that bit: a
 * descriptor published before its address was written is one the hardware
 * fills at whatever address happened to be there. This is the same
 * publish-last rule the page tables follow, for the same reason. */
static void rx_publish(uint32_t i) {
    volatile uint32_t *d = nic.rxd + i * 4u;
    /* The bus address of this slot's buffer. `nic.rxb` is where the kernel
     * reaches it; the controller needs where it physically is. */
    phys_t buf = mmu_virt_to_phys(nic.rxb + i * BUFFER);
    d[0] = (uint32_t)buf;
    d[1] = (uint32_t)(buf >> 32);
    d[2] = 0;
    /* **The ownership bit last, and fenced.** The controller may act on a
     * descriptor the instant it sees that bit, so one published before its
     * address was written is one the hardware fills at whatever address
     * happened to be there. `fence w, w` is the idiom `virtio.c` uses for
     * the same ordering problem against the same kind of reader. */
    __asm__ volatile("fence w, w" : : : "memory");
    d[3] = DESC_OWN | DESC_RX_BUF1V | DESC_RX_IOC;
}

/* The rings, and the DMA engine told where they are.
 *
 * `DMA_BUS_MODE_SWR` first: a software reset leaves every DMA register at
 * its default, which is the only state this driver knows how to build on —
 * the firmware has been using this controller for its own network and its
 * settings are not ours to inherit. The reset is self-clearing and is
 * waited for, because writing the ring addresses into a block that is still
 * resetting writes them nowhere. */
static int rings_up(void) {
    phys_t txd = 0, rxd = 0, txb = 0, rxb = 0;
    int rc = mem_alloc_pages(1, 1, &txd);
    rc = rc != KERR_OK ? rc : mem_alloc_pages(1, 1, &rxd);
    rc = rc != KERR_OK ? rc : mem_alloc_pages((RING * BUFFER + 4095u) / 4096u, 1, &txb);
    rc = rc != KERR_OK ? rc : mem_alloc_pages((RING * BUFFER + 4095u) / 4096u, 1, &rxb);
    if (rc != KERR_OK) {
        return rc;
    }
    /* **The allocator returns physical addresses and the device wants
     * them; the kernel cannot use them as pointers.** Both facts were true
     * before and invisible, because an identity-mapped kernel makes the two
     * the same number. The physical values are kept to give to the
     * hardware, and these are how this driver reaches the same memory. */
    nic.txd_phys = txd;
    nic.rxd_phys = rxd;
    nic.txd = (volatile uint32_t *)mmu_phys_to_virt(txd);
    nic.rxd = (volatile uint32_t *)mmu_phys_to_virt(rxd);
    nic.txb = (uint8_t *)mmu_phys_to_virt(txb);
    nic.rxb = (uint8_t *)mmu_phys_to_virt(rxb);
    for (uint32_t i = 0; i < RING * 4u; i++) {
        nic.txd[i] = 0;
        nic.rxd[i] = 0;
    }
    for (uint32_t i = 0; i < RING; i++) {
        rx_publish(i);
    }
    nic.tx_next = 0;
    nic.rx_next = 0;
    /* Store-and-forward both ways: the controller holds a whole frame
     * before starting on it, which costs latency and removes the underrun
     * a threshold mode has to be tuned to avoid. A kernel that polls is in
     * no position to tune it. */
    /* Enhanced addressing before the ring addresses are written, because
     * it is what makes the high word of each of them mean anything. */
    wr(DMA_SYSBUS_MODE, rd(DMA_SYSBUS_MODE) | DMA_SYS_EAME | DMA_SYS_AAL | DMA_SYS_FB);
    kprintf("dwmac: rings at %llx/%llx, buffers at %llx/%llx\n", (unsigned long long)txd,
            (unsigned long long)rxd, (unsigned long long)txb, (unsigned long long)rxb);
    wr(MTL_TXQ0_OPMODE, MTL_TXQ_ENABLE | MTL_TXQ_STORE_FWD);
    wr(MTL_RXQ0_OPMODE, MTL_RXQ_STORE_FWD);
    wr(DMA_CH0_TXLIST_HI, (uint32_t)((uint64_t)txd >> 32));
    wr(DMA_CH0_TXLIST, (uint32_t)txd);
    wr(DMA_CH0_RXLIST_HI, (uint32_t)((uint64_t)rxd >> 32));
    wr(DMA_CH0_RXLIST, (uint32_t)rxd);
    /* **The length registers hold one less than the ring**, which is the
     * kind of off-by-one that costs an afternoon: the controller reads it
     * as "the last valid index". */
    wr(DMA_CH0_TXRINGLEN, RING - 1u);
    wr(DMA_CH0_RXRINGLEN, RING - 1u);
    wr(DMA_CH0_RX_CTRL, (BUFFER << 1) | DMA_RX_START);
    wr(DMA_CH0_TX_CTRL, rd(DMA_CH0_TX_CTRL) | DMA_TX_START);
    /* The receive tail points past the last descriptor the hardware may
     * use; everything up to it is fair game. */
    wr(DMA_CH0_RXTAIL, (uint32_t)(rxd + RING * 16u));
    wr(MAC_CONFIG, rd(MAC_CONFIG) | MAC_CONFIG_TE | MAC_CONFIG_RE);
    return KERR_OK;
}

/* How long a full ring is waited on before a frame is given up: eight
 * full-size frames leave in under 100 us at a gigabit, so this is several
 * rings' worth. A frame dropped here costs the sender a retransmit timeout,
 * which is a second, not microseconds. */
#define TX_WAIT_SPINS 200000u

/* Whether the descriptor at d is the driver's (the controller has sent it),
 * waiting a bounded while for it to become so. */
static bool slot_frees(volatile uint32_t *d) {
    for (uint32_t i = 0; i < TX_WAIT_SPINS; i++) {
        if ((d[3] & DESC_OWN) == 0) {
            return true;
        }
    }
    return false;
}

static int dwmac_send(void *self, const uint8_t *frame, uint32_t len) {
    volatile uint32_t *d;
    uint8_t *buf;
    (void)self;
    if (!nic.present || len == 0 || len > BUFFER) {
        return KERR_INVAL;
    }
    d = nic.txd + nic.tx_next * 4u;
    if (!slot_frees(d)) {
        nic.tx_full++;
        return KERR_AGAIN; /* the controller still has this one */
    }
    buf = nic.txb + nic.tx_next * BUFFER;
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = frame[i];
    }
    {
        /* **The address the device will fetch from.** A kernel pointer here
         * is a DMA to whatever that number happens to be physically, which
         * while the kernel was identity-mapped was the right page by
         * accident. */
        uint64_t bus = (uint64_t)mmu_virt_to_phys(buf);
        d[0] = (uint32_t)bus;
        d[1] = (uint32_t)(bus >> 32);
    }
    d[2] = DESC_TX_IOC | len;
    __asm__ volatile("fence w, w" : : : "memory");
    d[3] = DESC_OWN | DESC_TX_FD | DESC_TX_LD | len;
    nic.tx_next = (nic.tx_next + 1u) % RING;
    /* The tail tells the controller how far it may walk. Writing it is what
     * starts the transfer — a descriptor published without moving the tail
     * sits there for ever, which is the commonest way a dwmac driver
     * appears to do nothing at all. */
    wr(DMA_CH0_TXTAIL, (uint32_t)(nic.txd_phys + nic.tx_next * 16u));
    nic.sent++;
    return KERR_OK;
}

static const struct netdev_port dwmac_port = {.send = dwmac_send, .self = NULL};

/* Arms the receive interrupt, and says whether the tree gave us one.
 *
 * **Why this device needs it and the others did not.** Everything here was
 * polled from the housekeeping beat, which is enough for DHCP — a boot-time
 * exchange with a hart that has work to do anyway. It is not enough for a
 * blocking `recvfrom`: the waiting thread has no deadline, so under dynamic
 * ticks nothing is scheduled to fire, nothing polls the device, and the
 * reply sits in the ring while the only thread that wants it waits for
 * ever. The deadlock detector catches it, which is how it was found, and the
 * fix is not a shorter poll interval — a device that is only looked at when
 * something else happens cannot complete a wait that nothing else will end.
 */
void dwmac_enable_rx_interrupt(void) {
    if (!nic.present) {
        return;
    }
    wr(DMA_CH0_INTR_ENA, DMA_INTR_RIE | DMA_INTR_NIE);
}

/* Takes every frame on the wire, for one question only.
 *
 * The board received the two broadcast DHCP replies and nothing else — no
 * ARP reply from the gateway, no DNS reply — and both of those are
 * **unicast** to this interface's address. So either they are not arriving
 * or the address filter is not matching them, and until that is settled
 * every other theory is guesswork. `MAC_PACKET_FILTER` was defined in this
 * driver and never written once, which is how a filter nobody configured
 * came to be the suspect. */
/* **What the filter is actually matching against**, read back rather than
 * assumed.
 *
 * Printed unconditionally, which is the whole point: it was first written
 * inside the promiscuous-mode diagnostic, so it only ever ran in the case
 * that *worked* — three board runs were spent reasoning about whether the
 * address and its enable bit were right in the failing case without ever
 * once printing them there. A diagnostic that only runs on the good path
 * answers the question nobody is asking. */
void dwmac_use_strict_filter(bool on) {
    nic.strict_filter = on;
}

void dwmac_trace_destinations(bool on) {
    nic.trace_dest = on;
}

void dwmac_report_filter(void) {
    if (!nic.present) {
        return;
    }
    kprintf("dwmac: filter %08x, addr %08x/%08x, ours %02x:%02x:%02x:%02x:%02x:%02x\n",
            rd(MAC_PACKET_FILTER), rd(MAC_ADDR0_HIGH), rd(MAC_ADDR0_LOW), nic.mac[0],
            nic.mac[1], nic.mac[2], nic.mac[3], nic.mac[4], nic.mac[5]);
}

void dwmac_set_promiscuous(bool on) {
    if (!nic.present) {
        return;
    }
    wr(MAC_PACKET_FILTER, on ? MAC_FILTER_PR : 0u);
    dwmac_report_filter();
}

uint32_t dwmac_irq(void) {
    return nic.present ? nic.irq : 0u;
}

/* Acknowledges at the device and drains the ring. Returns how many frames
 * were delivered, so a handler that fires with nothing to show says so. */
uint32_t dwmac_handle_interrupt(void) {
    uint32_t status;
    if (!nic.present) {
        return 0;
    }
    status = rd(DMA_CH0_STATUS);
    /* **Acknowledged before the ring is drained**, and only the bits that
     * were set: writing the register wholesale would clear causes this
     * driver does not handle and has not looked at. */
    wr(DMA_CH0_STATUS, status & (DMA_STATUS_RI | DMA_STATUS_NIS));
    return dwmac_poll();
}

/* Restarts a receive DMA that suspended for want of a descriptor.
 *
 * **It suspends with the ring full of free descriptors.** The DMA uses
 * those before the tail, and the tail is written just past the descriptor
 * the driver last handed back — so when the driver has caught up entirely,
 * the tail lands exactly where the DMA is waiting, current equals tail,
 * and it reads that as none available (measured: status RBU, state 4,
 * current = tail = slot 22, 63 of 64 owned by the controller, 28 frames
 * overflowed in the queue). Nothing more arrives, so nothing makes the
 * driver write the tail again: the board stopped receiving for good, 1 boot
 * in 8. Linux gets out of it because the RBU interrupt runs its refill;
 * here the poll checks for it. The tail goes one slot behind the driver,
 * off the DMA's position, and writing it resumes the DMA. */
static void resume_if_suspended(void) {
    if ((rd(DMA_CH0_STATUS) & DMA_STATUS_RBU) == 0) {
        return;
    }
    wr(DMA_CH0_STATUS, DMA_STATUS_RBU); /* write-one-to-clear */
    wr(DMA_CH0_RXTAIL, (uint32_t)(nic.rxd_phys + ((nic.rx_next + RING - 1u) % RING) * 16u));
    nic.rx_resumed++;
}

static uint32_t poll_ring(void);

/* The ring and the stack behind it are one caller's at a time. Counted, so
 * a second caller — another hart's beat while the boot hart polls without
 * the kernel lock — shows up as a number rather than as a corrupt stream. */
uint32_t dwmac_poll(void) {
    uint32_t delivered;
    if (!nic.present) {
        return 0;
    }
    if (__atomic_fetch_add(&nic.polling, 1u, __ATOMIC_ACQ_REL) != 0) {
        nic.overlaps++;
    }
    delivered = poll_ring();
    __atomic_fetch_sub(&nic.polling, 1u, __ATOMIC_ACQ_REL);
    return delivered;
}

static uint32_t poll_ring(void) {
    uint32_t delivered = 0;
    resume_if_suspended();
    for (uint32_t n = 0; n < RING; n++) {
        volatile uint32_t *d = nic.rxd + nic.rx_next * 4u;
        uint32_t status = d[3];
        uint32_t len;
        if ((status & DESC_OWN) != 0) {
            break; /* the controller has not finished with it */
        }
        len = status & 0x7fffu;
        if ((status & DESC_RX_ERROR) != 0 || len == 0 || len > BUFFER) {
            nic.dropped++;
        } else if (nic.sink != NULL) {
            /* **The frame is valid only during the call**, which is the
             * port's contract and why the descriptor is handed back
             * afterwards rather than before: giving it to the controller
             * first would let the next frame overwrite this one while the
             * stack was still reading it. */
            const uint8_t *f = nic.rxb + nic.rx_next * BUFFER;
            if (nic.trace_dest && nic.traced < 12u && len >= 14u) {
                nic.traced++;
                kprintf("dwmac: frame to %02x:%02x:%02x:%02x:%02x:%02x type %02x%02x len %u\n",
                        f[0], f[1], f[2], f[3], f[4], f[5], f[12], f[13], len);
            }
            nic.sink->frame(nic.sink->self, f, len);
            nic.received++;
            delivered++;
        }
        rx_publish(nic.rx_next);
        nic.rx_next = (nic.rx_next + 1u) % RING;
        wr(DMA_CH0_RXTAIL, (uint32_t)(nic.rxd_phys + nic.rx_next * 16u));
    }
    return delivered;
}

void dwmac_report(void) {
    uint32_t owned = 0;
    if (!nic.present) {
        return;
    }
    for (uint32_t i = 0; i < RING; i++) {
        owned += (nic.rxd[i * 4u + 3u] & DESC_OWN) != 0 ? 1u : 0u;
    }
    kprintf("dwmac: rx dma status %08x (RBU bit 7, RPS bit 8, FBE bit 12), rx state %x, "
            "current %08x tail %08x, driver at %u, %u of %u owned by the controller; "
            "%llu suspensions resumed, %llu overlapping polls\n",
            rd(DMA_CH0_STATUS), (rd(DMA_DEBUG_STATUS0) >> 8) & 0xfu, rd(DMA_CH0_CUR_RXDESC),
            rd(DMA_CH0_RXTAIL), nic.rx_next, owned, RING, (unsigned long long)nic.rx_resumed,
            (unsigned long long)nic.overlaps);
    kprintf("dwmac: rx queue missed/overflow %08x, debug %08x\n", rd(MTL_RXQ0_MISSED),
            rd(MTL_RXQ0_DEBUG));
}

/* Loops the MAC back on itself, so a frame sent is a frame received.
 *
 * **This is how the rings get tested without the network in the way.** The
 * first attempt sent a broadcast ARP and waited to see anything at all,
 * on the theory that a real segment is never quiet — and when nothing
 * came back there was no way to tell a broken receive ring from a switch
 * port with nobody else on it. The DMA status said transmit had finished
 * cleanly and receive was running with buffers free, which is consistent
 * with both.
 *
 * In loopback the question has one answer: the frame goes out, comes back
 * through the receive ring, and the bytes either match or they do not. A
 * test that depends on what else is plugged in is not a test of this
 * driver. */
void dwmac_set_loopback(bool on) {
    if (!nic.present && nic.regs == NULL) {
        return;
    }
    wr(MAC_CONFIG, on ? (rd(MAC_CONFIG) | MAC_CONFIG_LM)
                      : (rd(MAC_CONFIG) & ~(uint32_t)MAC_CONFIG_LM));
}

uint32_t dwmac_dma_status(void) {
    return nic.present ? rd(DMA_CH0_STATUS) : 0;
}

/* Whether the controller has finished with the last descriptor posted, and
 * what it said about it.
 *
 * `sent` counting posts rather than completions is honest but not enough: a
 * descriptor the hardware never looks at leaves the count rising and the
 * wire silent, which is exactly the failure a tail pointer left unwritten
 * produces. The ownership bit clearing is the hardware saying it is done. */
uint32_t dwmac_last_tx_status(void) {
    uint32_t i = (nic.tx_next + RING - 1u) % RING;
    return nic.present ? nic.txd[i * 4u + 3u] : 0;
}

void dwmac_counters(uint64_t *sent, uint64_t *received, uint64_t *dropped) {
    *sent = nic.sent;
    *received = nic.received;
    *dropped = nic.dropped + nic.tx_full;
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
/* This controller's PLIC source.
 *
 * **Two spellings, and this board uses the second.** `interrupts` is a list
 * of cells interpreted by the parent controller; `interrupts-extended` is a
 * list of (phandle, source) pairs, which a node uses when its interrupts go
 * to more than one controller — and on the JH7110 they do, so there is no
 * plain `interrupts` property at all. Reading only the first spelling gave
 * source 0, and a source of 0 is indistinguishable from "no interrupt", so
 * the handler was never registered and the failure was a silent fall back to
 * polling.
 *
 * The first pair's second cell is the source for the first controller named,
 * which is the PLIC. Printed when it is found so that the number in the log
 * is the number the PLIC was given. */
static uint32_t interrupt_from_tree(const struct fdt *fdt, const char *path) {
    struct fdt_prop p;
    uint64_t source = 0;
    if (fdt_get_prop(fdt, path, "interrupts", &p) == KERR_OK &&
        fdt_prop_cells(&p, 0, 1, &source) == KERR_OK && source != 0) {
        return (uint32_t)source;
    }
    if (fdt_get_prop(fdt, path, "interrupts-extended", &p) == KERR_OK &&
        fdt_prop_cells(&p, 1, 1, &source) == KERR_OK) {
        return (uint32_t)source;
    }
    kprintf("dwmac: the tree names no interrupt for %s\n", path);
    return 0;
}

static int open_controller(const struct fdt *fdt, const char *path) {
    phys_t base = 0;
    uint64_t size = 0;
    int rc = fdt_device_reg(fdt, path, 0, &base, &size);
    if (rc != KERR_OK) {
        return rc;
    }
    {
        void *at = mmu_map_device(base, size);
        if (at == NULL) {
            return KERR_NOMEM;
        }
        nic.regs = (volatile uint32_t *)at;
        /* Taken here, where the node is in hand. `dwmmc.c` reads its own the
         * same way; the number is the node's, never the driver's to know. */
        nic.irq = interrupt_from_tree(fdt, path);
    }
    rc = crg_enable_device(fdt, path);
    if (rc != KERR_OK) {
        return rc;
    }
    /* The version register is read back as a cross-check that all of the
     * above worked: a block that is mapped but unclocked reads zero here,
     * and this driver would rather say so than configure a silent device. */
    if ((rd(MAC_VERSION) & 0xffu) == 0) {
        kprintf("dwmac: %s reads version 0 — mapped but not running\n", path);
        return KERR_IO;
    }
    /* **The reset comes first, before anything is configured.**
     *
     * A DMA software reset on this controller resets the whole MAC, not
     * just the DMA block — so doing it where it naturally belongs, with the
     * rings, wipes every setting made before it. The first version
     * configured the speed and duplex the PHY had negotiated and *then*
     * reset the MAC, which left a gigabit link driven by a MAC back at its
     * defaults. Nothing complains about that: the link stays up, frames go
     * out at the wrong rate, and none arrive.
     *
     * The firmware has been using this controller for its own network, so
     * its settings are not ours to inherit either. A reset is the only
     * state this driver knows how to build on. */
    wr(DMA_BUS_MODE, rd(DMA_BUS_MODE) | DMA_BUS_MODE_SWR);
    for (uint32_t i = 0; i < 1000000u; i++) {
        if ((rd(DMA_BUS_MODE) & DMA_BUS_MODE_SWR) == 0) {
            return KERR_OK;
        }
    }
    kprintf("dwmac: the DMA block would not come out of reset\n");
    return KERR_TIMEDOUT;
}

int dwmac_init(const struct fdt *fdt, const struct netdev_port **port) {
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
        phy_apply_tree_timing(fdt, CONTROLLERS[i]);
        if (phy_reset() != KERR_OK) {
            kprintf("dwmac: the PHY would not come out of reset\n");
            continue;
        }
        if (wait_for_link() != KERR_OK) {
            kprintf("dwmac: no link on %s\n", CONTROLLERS[i]);
            continue; /* the other controller may be the one with the cable */
        }
        configure_mac();
        {
            struct fdt_prop nc;
            nic.noncoherent = fdt_get_prop(fdt, CONTROLLERS[i], "dma-noncoherent", &nc) ==
                              KERR_OK;
        }
        /* **Said out loud, because nothing in this tree maintains caches.**
         * NVMe and virtio do DMA here without any flush or invalidate,
         * which works because their masters are coherent. If this one is
         * not, frames will arrive as stale bytes rather than not at all —
         * so the tree's own answer is printed, and a corrupt frame has a
         * first suspect instead of a mystery. */
        kprintf("dwmac: the tree says this controller is %s\n",
                nic.noncoherent ? "dma-noncoherent — caches are NOT maintained here"
                                : "cache-coherent");
        if (rings_up() != KERR_OK) {
            kprintf("dwmac: the rings would not come up\n");
            continue;
        }
        nic.present = true;
        *port = &dwmac_port;
        kprintf("dwmac: link up at %u Mbit, %s duplex\n", nic.mbit,
                nic.full_duplex ? "full" : "half");
        return KERR_OK;
    }
    return KERR_NODEV;
}

