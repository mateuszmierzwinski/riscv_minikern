#include "drivers/i2c_dw.h"

#include "arch/riscv/mmu.h"
#include "core/console.h"
#include "core/errno.h"
#include "drivers/crg_jh7110.h"

/* The register window. These offsets are the DesignWare block's, and the
 * identity check below is what makes using them safe rather than hopeful. */
#define IC_CON 0x00u
#define IC_TAR 0x04u
#define IC_DATA_CMD 0x10u
#define IC_SS_SCL_HCNT 0x14u
#define IC_SS_SCL_LCNT 0x18u
#define IC_INTR_MASK 0x30u
#define IC_RX_TL 0x38u
#define IC_TX_TL 0x3cu
#define IC_CLR_INTR 0x40u
#define IC_ENABLE 0x6cu
#define IC_STATUS 0x70u
#define IC_TXFLR 0x74u
#define IC_RXFLR 0x78u
#define IC_TX_ABRT_SOURCE 0x80u
#define IC_COMP_VERSION 0xf8u
#define IC_COMP_TYPE 0xfcu

/* What a DesignWare I²C block reads back as its own type. If this does not
 * match, the offsets above are addressing something else. */
#define DW_COMPONENT_TYPE 0x44570140u

/* IC_CON: master enabled, standard speed, 7-bit, restart enabled. */
#define CON_MASTER (1u << 0)
#define CON_SPEED_STANDARD (1u << 1)
#define CON_RESTART_EN (1u << 5)
#define CON_SLAVE_DISABLE (1u << 6)

/* IC_DATA_CMD: the command bits above the data byte. */
#define CMD_READ (1u << 8)
#define CMD_STOP (1u << 9)

/* IC_STATUS */
#define STATUS_TFE (1u << 2)  /* transmit FIFO empty */
#define STATUS_RFNE (1u << 3) /* receive FIFO not empty */
#define STATUS_ACTIVITY (1u << 0)

/* How long to wait, in polls. A 100 kHz transfer of two bytes is about
 * 200 us; this is orders of magnitude more, because the point of the limit
 * is to turn a wedged controller into an error rather than to be tight. */
#define POLL_LIMIT 2000000u

static volatile uint32_t *reg_at(const struct i2c_dw *bus, uint32_t off) {
    return (volatile uint32_t *)((uint8_t *)bus->regs + off);
}

static uint32_t rd(const struct i2c_dw *bus, uint32_t off) {
    return *reg_at(bus, off);
}

static void wr(const struct i2c_dw *bus, uint32_t off, uint32_t value) {
    *reg_at(bus, off) = value;
}

uint32_t i2c_dw_component_type(const struct i2c_dw *bus) {
    return bus->up ? rd(bus, IC_COMP_TYPE) : 0u;
}

uint32_t i2c_dw_component_version(const struct i2c_dw *bus) {
    return bus->up ? rd(bus, IC_COMP_VERSION) : 0u;
}

/* The controller must be disabled to be configured: IC_CON and the timing
 * counters are ignored while it is enabled. */
static void configure(const struct i2c_dw *bus) {
    wr(bus, IC_ENABLE, 0);
    wr(bus, IC_CON, CON_MASTER | CON_SPEED_STANDARD | CON_RESTART_EN | CON_SLAVE_DISABLE);
    /* 100 kHz from a 50 MHz APB clock: half a period is 250 counts each way.
     * The exact clock comes from the tree in a fuller driver; these are the
     * conservative values for this board's APB, and a slow bus is correct
     * where a fast one would be wrong. */
    wr(bus, IC_SS_SCL_HCNT, 250u);
    wr(bus, IC_SS_SCL_LCNT, 250u);
    wr(bus, IC_TX_TL, 0);
    wr(bus, IC_RX_TL, 0);
    wr(bus, IC_INTR_MASK, 0); /* polled: no interrupt reaches the PLIC */
    wr(bus, IC_ENABLE, 1u);
}

int i2c_dw_init(struct i2c_dw *bus, const struct fdt *fdt, const char *path) {
    phys_t base = 0;
    uint64_t size = 0;
    uint32_t type;
    int rc;
    if (bus == NULL) {
        return KERR_INVAL;
    }
    bus->up = false;
    rc = fdt_device_reg(fdt, path, 0, &base, &size);
    if (rc != KERR_OK) {
        return KERR_NOENT;
    }
    /* Clock and reset from the node's own `clocks`/`resets`, which is how
     * every peripheral here is switched on — a complete driver that was
     * never clocked reads zeroes and looks like it works. */
    (void)crg_enable_device(fdt, path);
    bus->regs = mmu_map_device(base, size);
    if (bus->regs == NULL) {
        return KERR_NOMEM;
    }
    bus->base = base;
    bus->up = true;
    type = rd(bus, IC_COMP_TYPE);
    if (type != DW_COMPONENT_TYPE) {
        kprintf("i2c: %s reads component type %08x, not %08x — not addressing a "
                "DesignWare block\n",
                path, type, DW_COMPONENT_TYPE);
        bus->up = false;
        return KERR_IO;
    }
    configure(bus);
    return KERR_OK;
}

/* Waits for a condition in IC_STATUS, or gives up. */
static int wait_status(const struct i2c_dw *bus, uint32_t mask, bool set) {
    for (uint32_t i = 0; i < POLL_LIMIT; i++) {
        bool now = (rd(bus, IC_STATUS) & mask) != 0;
        if (now == set) {
            return KERR_OK;
        }
    }
    return KERR_TIMEDOUT;
}

/* An aborted transfer leaves its reason in IC_TX_ABRT_SOURCE and the
 * controller needs the interrupt cleared before it will work again —
 * reading IC_CLR_INTR does that. A device that is not there aborts with
 * "address not acknowledged", which is the common case and not an error
 * worth a message. */
static int abort_reason(const struct i2c_dw *bus) {
    uint32_t why = rd(bus, IC_TX_ABRT_SOURCE);
    (void)rd(bus, IC_CLR_INTR);
    return why == 0 ? KERR_OK : KERR_IO;
}

/* **Starts every transfer from a known state.**
 *
 * The first version did not, and the symptom was a lesson: twelve different
 * PMIC registers read back almost the same byte. That is not what a dozen
 * voltage rails look like, and it was not what the PMIC said — it was one
 * stale byte left in the receive FIFO, read again and again. A read that
 * returns plausible-looking data from the wrong place is the worst kind,
 * because nothing about it looks like a failure.
 *
 * Draining first makes each transfer's answer its own. Clearing the
 * interrupt state matters too: an abort left pending from a previous
 * transfer stops the controller accepting the next one, so a single missing
 * device would wedge every read after it. */
static void drain(const struct i2c_dw *bus) {
    for (uint32_t i = 0; i < 16u && (rd(bus, IC_STATUS) & STATUS_RFNE) != 0; i++) {
        (void)rd(bus, IC_DATA_CMD);
    }
}

/* The target address.
 *
 * **Set with the controller disabled**, which this block version requires:
 * `IC_TAR` is only writable while enabled on versions that implement the
 * dynamic-target feature, and this one does not. Trying it the other way
 * made *every* read fail — a cleaner failure than the one it was meant to
 * fix, and the reason that attempt did not survive.
 *
 * Disabling also resets the FIFOs, so it doubles as the drain. */
static int aim_at(const struct i2c_dw *bus, uint8_t addr) {
    wr(bus, IC_ENABLE, 0);
    wr(bus, IC_TAR, addr);
    wr(bus, IC_ENABLE, 1u);
    (void)rd(bus, IC_CLR_INTR);
    return KERR_OK;
}

int i2c_dw_read_u8(struct i2c_dw *bus, uint8_t addr, uint8_t reg, uint8_t *out) {
    int rc;
    if (bus == NULL || !bus->up || out == NULL) {
        return KERR_INVAL;
    }
    drain(bus);
    rc = aim_at(bus, addr);
    if (rc != KERR_OK) {
        return rc;
    }
    wr(bus, IC_DATA_CMD, reg);                 /* the register number */
    wr(bus, IC_DATA_CMD, CMD_READ | CMD_STOP); /* then read one byte */
    rc = wait_status(bus, STATUS_RFNE, true);
    if (rc != KERR_OK) {
        (void)abort_reason(bus);
        return rc;
    }
    *out = (uint8_t)(rd(bus, IC_DATA_CMD) & 0xffu);
    return abort_reason(bus);
}

int i2c_dw_write_u8(struct i2c_dw *bus, uint8_t addr, uint8_t reg, uint8_t value) {
    int rc;
    if (bus == NULL || !bus->up) {
        return KERR_INVAL;
    }
    drain(bus);
    rc = aim_at(bus, addr);
    if (rc != KERR_OK) {
        return rc;
    }
    wr(bus, IC_DATA_CMD, reg);
    wr(bus, IC_DATA_CMD, CMD_STOP | value);
    rc = wait_status(bus, STATUS_TFE, true);
    if (rc != KERR_OK) {
        (void)abort_reason(bus);
        return rc;
    }
    return abort_reason(bus);
}
