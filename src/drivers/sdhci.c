#include "drivers/sdhci.h"

#include <stdbool.h>
#include <stddef.h>

#include "arch/riscv/mmu.h"
#include "core/console.h"
#include "core/errno.h"
#include "drivers/pci.h"
#include "drivers/sdcard.h"

/* Host controller registers (SD Host Controller Interface 3.0). */
#define SDHCI_ARGUMENT 0x08u
#define SDHCI_TRANSFER_MODE 0x0cu
#define SDHCI_COMMAND 0x0eu
#define SDHCI_RESPONSE 0x10u
#define SDHCI_BUFFER 0x20u
#define SDHCI_PRESENT_STATE 0x24u
#define SDHCI_HOST_CONTROL 0x28u
#define SDHCI_POWER_CONTROL 0x29u
#define SDHCI_CLOCK_CONTROL 0x2cu
#define SDHCI_TIMEOUT_CONTROL 0x2eu
#define SDHCI_SOFTWARE_RESET 0x2fu
#define SDHCI_INT_STATUS 0x30u
#define SDHCI_INT_ENABLE 0x34u
#define SDHCI_SIGNAL_ENABLE 0x38u
#define SDHCI_BLOCK_SIZE 0x04u
#define SDHCI_BLOCK_COUNT 0x06u
#define SDHCI_CAPABILITIES 0x40u

#define PRESENT_CARD_INSERTED (1u << 16)
#define PRESENT_WRITE_ENABLED (1u << 19)
#define PRESENT_BUFFER_READ (1u << 11)
#define PRESENT_BUFFER_WRITE (1u << 10)
#define PRESENT_CMD_INHIBIT 0x1u
#define PRESENT_DAT_INHIBIT 0x2u

#define RESET_ALL 0x1u
#define RESET_CMD 0x2u
#define RESET_DATA 0x4u

#define CLOCK_INTERNAL_ENABLE 0x1u
#define CLOCK_INTERNAL_STABLE 0x2u
#define CLOCK_SD_ENABLE 0x4u

#define INT_COMMAND_COMPLETE 0x1u
#define INT_TRANSFER_COMPLETE 0x2u
#define INT_BUFFER_WRITE_READY 0x10u
#define INT_BUFFER_READ_READY 0x20u
#define INT_ERROR 0x8000u

/* Command flags: which kind of response to expect, and whether data moves. */
#define CMD_RESP_NONE 0x0u
#define CMD_RESP_136 0x1u
#define CMD_RESP_48 0x2u
#define CMD_RESP_48_BUSY 0x3u
#define CMD_CRC_CHECK 0x8u
#define CMD_INDEX_CHECK 0x10u
#define CMD_DATA 0x20u

#define TRANSFER_READ 0x10u
#define TRANSFER_BLOCK_COUNT 0x02u

/* Card commands. */
#define CMD_GO_IDLE 0u
#define CMD_SEND_IF_COND 8u
#define CMD_APP_CMD 55u
#define ACMD_SEND_OP_COND 41u
#define CMD_ALL_SEND_CID 2u
#define CMD_SEND_RELATIVE_ADDR 3u
#define CMD_SEND_CSD 9u
#define CMD_SELECT_CARD 7u
#define CMD_SET_BLOCKLEN 16u
#define CMD_READ_SINGLE 17u
#define CMD_WRITE_SINGLE 24u

/* What CMD8 asks for: 2.7–3.6 V, with a pattern the card echoes back so a
 * driver can tell a real answer from a floating bus. */
#define IF_COND_VOLTAGE 0x100u
#define IF_COND_PATTERN 0xaau

/* ACMD41's argument: 3.2–3.4 V, and the bit that says "I understand
 * high-capacity cards". A host that omits it is told the card's size in a
 * layout that cannot express more than 2 GiB. */
#define OCR_VOLTAGE_WINDOW 0x00300000u
#define OCR_HIGH_CAPACITY (1u << 30)
#define OCR_POWERED_UP (1u << 31)

#define BLOCK_BYTES SD_BLOCK_BYTES
#define WAIT_SPINS 50000000u

/* The controller. What the *card* is — its address, its capacity and whether
 * it addresses blocks or bytes — belongs to the shared protocol and lives in
 * `card` below. */
static struct {
    volatile uint8_t *regs;
    bool up;
} sd;

static void write8(uint32_t off, uint8_t v) {
    sd.regs[off] = v;
}

static void write16(uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(sd.regs + off) = v;
}

static void write32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(sd.regs + off) = v;
}

static uint8_t read8(uint32_t off) {
    return sd.regs[off];
}

static uint32_t read32(uint32_t off) {
    return *(volatile uint32_t *)(sd.regs + off);
}

/* Waits for the controller to be willing to take a command. Both inhibit
 * bits, because a data command must also wait for the data line. */
static int wait_ready_for_command(bool with_data) {
    uint32_t mask = PRESENT_CMD_INHIBIT | (with_data ? PRESENT_DAT_INHIBIT : 0u);
    for (uint32_t spin = 0; spin < WAIT_SPINS; spin++) {
        if ((read32(SDHCI_PRESENT_STATE) & mask) == 0) {
            return KERR_OK;
        }
    }
    return KERR_TIMEDOUT;
}

/* Issues one card command and waits for it to complete.
 *
 * The interrupt-status register is used as a completion flag rather than an
 * interrupt: the enables are set so the bits appear, and the signal enables
 * are left clear so nothing is raised. A card is slow and this kernel has
 * nothing else to do during a block transfer, so polling is not a shortcut —
 * it is the simpler correct answer. */
/* The shared protocol's response kinds in this controller's flags.
 *
 * **The CRC is checked only where there is one.** The OCR (an R3 response)
 * carries reserved bits where a CRC would go, so a controller told to check
 * it reports a CRC failure for a perfectly good answer. QEMU's card model
 * does not compute response CRCs, so this was invisible here and stopped a
 * real card one command into powering up. */
static uint8_t flags_for(uint32_t kind, bool with_data) {
    uint8_t flags = with_data ? CMD_DATA : 0u;
    switch (kind) {
        case SD_RESP_LONG:
            return flags | CMD_RESP_136 | CMD_CRC_CHECK;
        case SD_RESP_BUSY:
            return flags | CMD_RESP_48_BUSY | CMD_CRC_CHECK;
        case SD_RESP_SHORT:
            return flags | CMD_RESP_48 | CMD_CRC_CHECK;
        case SD_RESP_SHORT_NOCRC:
            return flags | CMD_RESP_48;
        default:
            return flags | CMD_RESP_NONE;
    }
}

/* This controller's long response, in the layout the shared protocol expects.
 *
 * **Every field is 8 bits from where the specification says it is**, and the
 * shift belongs here rather than in the protocol. The card sends 136 bits — a
 * start bit, a transmission bit, six reserved, the 128-bit payload and a CRC
 * — and this controller presents only the middle of that, having dropped the
 * trailing CRC byte. So the specification's bit 127 arrives as response bit
 * 119, and reading the documented positions gives plausible nonsense: a
 * 64 MiB card reported itself as 30 GiB, and the write that followed was
 * addressed past the end of it. A DesignWare controller does not do this,
 * which is how the shift came to be a transport's business. */
static void read_long_response(uint32_t out[4]) {
    uint32_t raw[4];
    for (uint32_t i = 0; i < 4u; i++) {
        raw[i] = read32(SDHCI_RESPONSE + i * 4u);
    }
    out[0] = raw[0] << 8;
    out[1] = (raw[0] >> 24) | (raw[1] << 8);
    out[2] = (raw[1] >> 24) | (raw[2] << 8);
    out[3] = (raw[2] >> 24) | (raw[3] << 8);
}

static int command(uint8_t index, uint32_t argument, uint8_t flags, uint32_t *response) {
    int rc = wait_ready_for_command((flags & CMD_DATA) != 0);
    if (rc != KERR_OK) {
        return rc;
    }
    write32(SDHCI_INT_STATUS, 0xffffffffu); /* clear whatever the last one left */
    write32(SDHCI_ARGUMENT, argument);
    write16(SDHCI_COMMAND, (uint16_t)(((uint16_t)index << 8) | flags));
    for (uint32_t spin = 0; spin < WAIT_SPINS; spin++) {
        uint32_t status = read32(SDHCI_INT_STATUS);
        if ((status & INT_ERROR) != 0) {
            return KERR_IO;
        }
        if ((status & INT_COMMAND_COMPLETE) != 0) {
            if (response != NULL) {
                *response = read32(SDHCI_RESPONSE);
            }
            return KERR_OK;
        }
    }
    return KERR_TIMEDOUT;
}

/* The shared protocol's command port. A long response is normalised on the
 * way out; a short one is a single register. */
static int sdhci_send(void *self, uint8_t index, uint32_t argument, uint32_t kind,
                      uint32_t *response) {
    bool with_data = index == CMD_READ_SINGLE || index == CMD_WRITE_SINGLE;
    int rc;
    if (with_data) {
        write16(SDHCI_BLOCK_SIZE, BLOCK_BYTES);
        write16(SDHCI_BLOCK_COUNT, 1);
        write16(SDHCI_TRANSFER_MODE, index == CMD_READ_SINGLE ? TRANSFER_READ : 0u);
    }
    (void)self;
    rc = command(index, argument, flags_for(kind, with_data),
                 kind == SD_RESP_LONG ? NULL : response);
    if (rc == KERR_OK && kind == SD_RESP_LONG && response != NULL) {
        read_long_response(response);
    }
    return rc;
}

/* Resets the controller and starts its clock. */
static int start_controller(void) {
    write8(SDHCI_SOFTWARE_RESET, RESET_ALL);
    for (uint32_t spin = 0; spin < WAIT_SPINS; spin++) {
        if ((read8(SDHCI_SOFTWARE_RESET) & RESET_ALL) == 0) {
            break;
        }
    }
    if ((read32(SDHCI_PRESENT_STATE) & PRESENT_CARD_INSERTED) == 0) {
        return KERR_NODEV; /* a controller with an empty slot */
    }
    write8(SDHCI_POWER_CONTROL, 0x0fu); /* 3.3 V, on */
    /* The slowest divisor the register can express, which every card accepts
     * for identification. Speed is a later concern and needs the card's own
     * capability data to do safely. */
    write16(SDHCI_CLOCK_CONTROL, (uint16_t)(0x80u << 8) | CLOCK_INTERNAL_ENABLE);
    for (uint32_t spin = 0; spin < WAIT_SPINS; spin++) {
        if ((read32(SDHCI_CLOCK_CONTROL) & CLOCK_INTERNAL_STABLE) != 0) {
            break;
        }
    }
    write16(SDHCI_CLOCK_CONTROL, (uint16_t)((0x80u << 8) | CLOCK_INTERNAL_ENABLE | CLOCK_SD_ENABLE));
    write8(SDHCI_TIMEOUT_CONTROL, 0x0eu);
    write32(SDHCI_INT_ENABLE, 0xffffffffu);  /* so the status bits appear */
    write32(SDHCI_SIGNAL_ENABLE, 0);         /* but nothing is raised */
    return KERR_OK;
}

/* This controller sets its clock from a register this driver already
 * programmed for identification, and the shared protocol's "go faster" step
 * has nothing to add here: raising it safely needs the card's own capability
 * data, which is a later concern. Declared so the port is complete. */
static const struct sd_transport transport = {
    .self = NULL,
    .command = sdhci_send,
    .move_block = NULL, /* this driver drives its own data path, below */
    .run_faster = NULL,
};

static struct sd_card card;

/* Moves one block through the controller's data port, a word at a time.
 *
 * No DMA: the controller supports it and a descriptor table would be faster,
 * but the data port is correct, is the same on every implementation of the
 * standard, and needs no memory the card can reach. Speed is worth having
 * once something is waiting on it. */
static int move_block(bool reading, void *buf) {
    uint32_t *words = buf;
    uint32_t ready = reading ? INT_BUFFER_READ_READY : INT_BUFFER_WRITE_READY;
    for (uint32_t spin = 0; spin < WAIT_SPINS; spin++) {
        uint32_t status = read32(SDHCI_INT_STATUS);
        if ((status & INT_ERROR) != 0) {
            return KERR_IO;
        }
        if ((status & ready) != 0) {
            for (uint32_t i = 0; i < BLOCK_BYTES / 4u; i++) {
                if (reading) {
                    words[i] = read32(SDHCI_BUFFER);
                } else {
                    write32(SDHCI_BUFFER, words[i]);
                }
            }
            break;
        }
    }
    for (uint32_t spin = 0; spin < WAIT_SPINS; spin++) {
        if ((read32(SDHCI_INT_STATUS) & INT_TRANSFER_COMPLETE) != 0) {
            return KERR_OK;
        }
    }
    return KERR_TIMEDOUT;
}

/* One block, read or written. The address is a block number on a
 * high-capacity card and a byte offset on an older one — the other half of
 * the capacity question, and getting it wrong on a modern card addresses the
 * first 1/512th of it. */
static int one_block(bool reading, uint64_t lba, void *buf) {
    uint32_t argument = card.high_capacity ? (uint32_t)lba : (uint32_t)(lba * BLOCK_BYTES);
    int rc = sdhci_send(NULL, reading ? CMD_READ_SINGLE : CMD_WRITE_SINGLE, argument,
                        SD_RESP_SHORT, NULL);
    return rc != KERR_OK ? rc : move_block(reading, buf);
}

static int sd_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    (void)self;
    if (!sd.up || count == 0 || lba + count > card.blocks) {
        return KERR_INVAL;
    }
    for (uint32_t i = 0; i < count; i++) {
        int rc = one_block(true, lba + i, (uint8_t *)buf + (uint64_t)i * BLOCK_BYTES);
        if (rc != KERR_OK) {
            return rc;
        }
    }
    return KERR_OK;
}

static int sd_write(void *self, uint64_t lba, uint32_t count, const void *buf) {
    (void)self;
    if (!sd.up || count == 0 || lba + count > card.blocks) {
        return KERR_INVAL;
    }
    if ((read32(SDHCI_PRESENT_STATE) & PRESENT_WRITE_ENABLED) == 0) {
        return KERR_ROFS; /* the card's own write-protect notch */
    }
    for (uint32_t i = 0; i < count; i++) {
        int rc = one_block(false, lba + i, (uint8_t *)(uintptr_t)buf + (uint64_t)i * BLOCK_BYTES);
        if (rc != KERR_OK) {
            return rc;
        }
    }
    return KERR_OK;
}

/* Writes go straight to the card: the controller has no cache of its own,
 * and the card's internal one is flushed by its own completion. */
static int sd_flush(void *self) {
    (void)self;
    return sd.up ? KERR_OK : KERR_NODEV;
}

/* The controller, from the tree if the board describes one and from the PCI
 * bus otherwise. The board's is the real target; the PCI one is what an
 * emulator offers, and the registers are the same standard either way. */
static int find_controller(const struct fdt *fdt) {
    static const char *const compatibles[] = {"cdns,sd4hc", "snps,dwcmshc-sdhci", "mmc-spi-slot"};
    struct pci_device pcidev;
    for (uint32_t i = 0; i < sizeof compatibles / sizeof compatibles[0]; i++) {
        char path[64];
        phys_t base = 0;
        uint64_t size = 0;
        if (fdt_find_compatible(fdt, compatibles[i], path, sizeof path) != KERR_OK ||
            fdt_device_reg(fdt, path, 0, &base, &size) != KERR_OK ||
            mmu_map_device(base, size) != KERR_OK) {
            continue;
        }
        sd.regs = (volatile uint8_t *)(uintptr_t)base;
        return KERR_OK;
    }
    if (pci_find_class(SDHCI_PCI_CLASS, SDHCI_PCI_CLASS_MASK, &pcidev) == KERR_OK) {
        sd.regs = (volatile uint8_t *)(uintptr_t)pcidev.bar0;
        return KERR_OK;
    }
    return KERR_NOENT;
}

int sdhci_init(const struct fdt *fdt, struct blockdev *out) {
    int rc = find_controller(fdt);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = start_controller();
    rc = rc != KERR_OK ? rc : sdcard_bring_up(&transport, &card);
    if (rc != KERR_OK) {
        return rc;
    }
    sd.up = true;
    *out = (struct blockdev){.read = sd_read, .write = sd_write, .flush = sd_flush, .self = NULL,
                             .block_size = BLOCK_BYTES, .blocks = card.blocks, .read_only = false};
    return KERR_OK;
}

const char *sdhci_kind(void) {
    return sd.up ? (card.high_capacity ? "SDHC/SDXC" : "SD") : "";
}

uint64_t sdhci_blocks(void) {
    return card.blocks;
}
