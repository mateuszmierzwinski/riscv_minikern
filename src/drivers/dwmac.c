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
#define MAC_CONFIG_LM    (1u << 12) /* loopback: the MAC feeds itself */

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
#define RING 8u
#define BUFFER 2048u

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
    phys_t buf = (phys_t)(uintptr_t)(nic.rxb + i * BUFFER);
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
    nic.txd = (volatile uint32_t *)(uintptr_t)txd;
    nic.rxd = (volatile uint32_t *)(uintptr_t)rxd;
    nic.txb = (uint8_t *)(uintptr_t)txb;
    nic.rxb = (uint8_t *)(uintptr_t)rxb;
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

static int dwmac_send(void *self, const uint8_t *frame, uint32_t len) {
    volatile uint32_t *d;
    uint8_t *buf;
    (void)self;
    if (!nic.present || len == 0 || len > BUFFER) {
        return KERR_INVAL;
    }
    d = nic.txd + nic.tx_next * 4u;
    if ((d[3] & DESC_OWN) != 0) {
        return KERR_AGAIN; /* the controller still has this one */
    }
    buf = nic.txb + nic.tx_next * BUFFER;
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = frame[i];
    }
    d[0] = (uint32_t)(uintptr_t)buf;
    d[1] = (uint32_t)((uint64_t)(uintptr_t)buf >> 32);
    d[2] = DESC_TX_IOC | len;
    __asm__ volatile("fence w, w" : : : "memory");
    d[3] = DESC_OWN | DESC_TX_FD | DESC_TX_LD | len;
    nic.tx_next = (nic.tx_next + 1u) % RING;
    /* The tail tells the controller how far it may walk. Writing it is what
     * starts the transfer — a descriptor published without moving the tail
     * sits there for ever, which is the commonest way a dwmac driver
     * appears to do nothing at all. */
    wr(DMA_CH0_TXTAIL, (uint32_t)(uintptr_t)(nic.txd + nic.tx_next * 4u));
    nic.sent++;
    return KERR_OK;
}

static const struct netdev_port dwmac_port = {.send = dwmac_send, .self = NULL};

uint32_t dwmac_poll(void) {
    uint32_t delivered = 0;
    if (!nic.present) {
        return 0;
    }
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
            nic.sink->frame(nic.sink->self, nic.rxb + nic.rx_next * BUFFER, len);
            nic.received++;
            delivered++;
        }
        rx_publish(nic.rx_next);
        nic.rx_next = (nic.rx_next + 1u) % RING;
        wr(DMA_CH0_RXTAIL, (uint32_t)(uintptr_t)(nic.rxd + nic.rx_next * 4u));
    }
    return delivered;
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
    *dropped = nic.dropped;
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

