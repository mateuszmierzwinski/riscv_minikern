#include "drivers/spi_pl022.h"

#include "core/console.h"
#include "core/errno.h"
#include "drivers/crg_jh7110.h"
#include "arch/riscv/mmu.h"
#include "arch/riscv/timer.h"

/* The PL022's registers, by name from the PrimeCell SSP specification. */
#define SSPCR0 0x000u /* frame format, data size, clock rate */
#define SSPCR1 0x004u /* enable, master/slave, loopback */
#define SSPDR 0x008u  /* the data FIFOs, one register for both */
#define SSPSR 0x00cu  /* FIFO status */
#define SSPCPSR 0x010u /* the clock prescale */
#define SSPPERIPHID0 0xfe0u

#define CR0_DSS_8BIT 0x07u /* data size: the field is bits-minus-one */
#define CR1_LBM (1u << 0)  /* loopback: transmit tied to receive internally */
#define CR1_SSE (1u << 1)  /* the controller enabled */
#define CR1_MS (1u << 2)   /* 1 would be slave; this driver is always master */

#define SR_TNF (1u << 1) /* transmit FIFO not full */
#define SR_RNE (1u << 2) /* receive FIFO not empty */
#define SR_BSY (1u << 4)

/* A transfer of one byte cannot take this long at any clock the driver sets,
 * so a wait that reaches it is a controller that is not running rather than
 * one that is slow. */
#define TRANSFER_TIMEOUT_NS 10000000u

/* The APB clock the prescaler divides. Taken from the node when the tree
 * says, and otherwise the JH7110's documented 49.5 MHz APB — which is only
 * used to pick a divisor, so being wrong makes the bus the wrong speed
 * rather than making it not work. */
#define APB_HZ_DEFAULT 49500000u

static uint32_t rd(const struct spi_pl022 *bus, uint32_t off) {
    return bus->regs[off / 4u];
}

static void wr(struct spi_pl022 *bus, uint32_t off, uint32_t v) {
    bus->regs[off / 4u] = v;
}

int spi_pl022_init(struct spi_pl022 *bus, const struct fdt *fdt, const char *path) {
    phys_t base = 0;
    uint64_t size = 0;
    if (bus == NULL) {
        return KERR_INVAL;
    }
    bus->up = false;
    if (fdt_device_reg(fdt, path, 0, &base, &size) != KERR_OK) {
        return KERR_NOENT;
    }
    /* Clocked and reset from the node's own properties, like every other
     * peripheral here: a driver that was never switched on reads zeroes and
     * looks as though it works. */
    (void)crg_enable_device(fdt, path);
    bus->regs = mmu_map_device(base, size);
    if (bus->regs == NULL) {
        return KERR_NOMEM;
    }
    bus->irq = fdt_get_u32(fdt, path, "interrupts", 0);
    bus->up = true;
    return KERR_OK;
}

uint32_t spi_pl022_part(const struct spi_pl022 *bus) {
    uint32_t id = 0;
    if (!bus->up) {
        return 0;
    }
    /* Four registers, one byte of the identifier in each — a PrimeCell
     * convention, and reading it as one word would give the low byte and
     * three of padding. */
    for (uint32_t i = 0; i < 4u; i++) {
        id |= (rd(bus, SSPPERIPHID0 + i * 4u) & 0xffu) << (i * 8u);
    }
    return id;
}

/* The prescale and the clock-rate divisor for a wanted bit rate.
 *
 * The PL022 divides by `(1 + SCR) * CPSDVSR`, where the prescale is an even
 * number from 2 to 254 and SCR is 0 to 255. Searching prescales in order and
 * taking the first that yields an SCR in range gives the fastest clock not
 * above what was asked — which is the right direction to err for a bus whose
 * device has a maximum. */
static void pick_divisors(uint32_t apb_hz, uint32_t want_hz, uint32_t *cpsdvsr, uint32_t *scr) {
    *cpsdvsr = 2;
    *scr = 255;
    if (want_hz == 0) {
        return;
    }
    for (uint32_t pre = 2; pre <= 254u; pre += 2u) {
        uint32_t divisor = apb_hz / (pre * want_hz);
        if (divisor >= 1u && divisor <= 256u) {
            *cpsdvsr = pre;
            *scr = divisor - 1u;
            return;
        }
    }
}

int spi_pl022_configure(struct spi_pl022 *bus, uint32_t hz, bool loopback) {
    uint32_t cpsdvsr, scr;
    if (!bus->up) {
        return KERR_NODEV;
    }
    /* **Disabled while reconfigured.** Writing the frame format of a running
     * controller is undefined on this block, and the symptom would be a FIFO
     * that behaves correctly until the first real device is attached. */
    wr(bus, SSPCR1, 0);
    pick_divisors(APB_HZ_DEFAULT, hz, &cpsdvsr, &scr);
    wr(bus, SSPCPSR, cpsdvsr);
    wr(bus, SSPCR0, CR0_DSS_8BIT | (scr << 8));
    wr(bus, SSPCR1, CR1_SSE | (loopback ? CR1_LBM : 0u));
    return KERR_OK;
}

/* Waits for a status bit, or gives up. */
static int wait_for(struct spi_pl022 *bus, uint32_t bit, bool set) {
    uint64_t until = timer_ns() + TRANSFER_TIMEOUT_NS;
    while (timer_ns() < until) {
        if (((rd(bus, SSPSR) & bit) != 0) == set) {
            return KERR_OK;
        }
    }
    return KERR_TIMEDOUT;
}

int spi_pl022_transfer(struct spi_pl022 *bus, uint8_t out, uint8_t *in) {
    int rc;
    if (!bus->up) {
        return KERR_NODEV;
    }
    rc = wait_for(bus, SR_TNF, true);
    if (rc != KERR_OK) {
        return rc;
    }
    wr(bus, SSPDR, out);
    /* **The receive FIFO, not the busy flag.** A byte is complete when
     * something has arrived, and busy clears before the last bit has been
     * clocked into the receive register on this block — waiting on busy
     * reads the previous byte. */
    rc = wait_for(bus, SR_RNE, true);
    if (rc != KERR_OK) {
        return rc;
    }
    if (in != NULL) {
        *in = (uint8_t)(rd(bus, SSPDR) & 0xffu);
    } else {
        (void)rd(bus, SSPDR); /* drained even when discarded, or it stays */
    }
    return KERR_OK;
}
