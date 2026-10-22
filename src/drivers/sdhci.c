#include "drivers/sdhci.h"

#include <stdbool.h>
#include <stddef.h>

#include "arch/riscv/mmu.h"
#include "core/console.h"
#include "core/errno.h"
#include "drivers/pci.h"

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

#define BLOCK_BYTES 512u
#define WAIT_SPINS 50000000u

static struct {
    volatile uint8_t *regs;
    uint32_t rca;        /* the address the card was given */
    bool high_capacity;  /* addresses blocks rather than bytes */
    uint64_t blocks;
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

/* An application-specific command is an ordinary one prefixed by CMD55. */
static int app_command(uint8_t index, uint32_t argument, uint32_t *response) {
    int rc = command(CMD_APP_CMD, sd.rca << 16, CMD_RESP_48 | CMD_CRC_CHECK, NULL);
    return rc != KERR_OK ? rc : command(index, argument, CMD_RESP_48, response);
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

/* Powers the card up and finds out whether it is high-capacity.
 *
 * ACMD41 is asked repeatedly because a card takes time to power up and says
 * so by leaving the ready bit clear — the loop is the protocol, not a
 * workaround. */
static int power_up_card(void) {
    uint32_t response = 0;
    int rc = command(CMD_GO_IDLE, 0, CMD_RESP_NONE, NULL);
    if (rc != KERR_OK) {
        return rc;
    }
    /* CMD8's echoed pattern distinguishes a card that understands the modern
     * protocol from a floating bus that reads as all ones. */
    rc = command(CMD_SEND_IF_COND, IF_COND_VOLTAGE | IF_COND_PATTERN,
                 CMD_RESP_48 | CMD_CRC_CHECK, &response);
    if (rc != KERR_OK || (response & 0xffu) != IF_COND_PATTERN) {
        return KERR_NOTSUP;
    }
    for (uint32_t tries = 0; tries < 1000u; tries++) {
        rc = app_command(ACMD_SEND_OP_COND, OCR_VOLTAGE_WINDOW | OCR_HIGH_CAPACITY, &response);
        if (rc != KERR_OK) {
            return rc;
        }
        if ((response & OCR_POWERED_UP) != 0) {
            sd.high_capacity = (response & OCR_HIGH_CAPACITY) != 0;
            return KERR_OK;
        }
    }
    return KERR_TIMEDOUT;
}

/* Reads the card's capacity out of its CSD register.
 *
 * **Every field here is 8 bits away from where the specification says it
 * is.** The card sends 136 bits — a start bit, a transmission bit, six
 * reserved, the 128-bit CSD, and a CRC — and the host controller presents
 * only the middle of that in its response registers, having dropped the
 * trailing CRC byte. So the specification's bit 127 arrives as response bit
 * 119, and reading the documented positions directly gives plausible
 * nonsense: a 64 MiB card reported itself as 30 GiB, and the write that
 * followed was addressed past the end of it.
 *
 * Then two layouts of those bits, which is the one piece of real variation
 * in the protocol: a card up to 2 GiB reports a geometry to multiply out,
 * and a high-capacity card reports a block count directly. Reading the
 * wrong one of a modern card gives a working driver that sees a fraction of
 * the storage.
 *
 * `r[0]` holds response bits 31:0, `r[3]` bits 127:96; a specification bit
 * B is response bit B-8. */
static void take_capacity(const uint32_t r[4]) {
    uint32_t version = (r[3] >> 22) & 0x3u;  /* CSD_STRUCTURE, spec 127:126 */
    if (version == 1u) {
        /* SDHC/SDXC: C_SIZE is spec 69:48, so response 61:40. Capacity is
         * (C_SIZE + 1) * 512 KiB, which is (C_SIZE + 1) * 1024 blocks. */
        uint32_t c_size = (r[1] >> 8) & 0x3fffffu;
        sd.blocks = ((uint64_t)c_size + 1u) * 1024u;
    } else {
        /* Standard capacity: C_SIZE is spec 73:62 (response 65:54),
         * C_SIZE_MULT spec 49:47 (response 41:39), READ_BL_LEN spec 83:80
         * (response 75:72). */
        uint32_t c_size = ((r[2] & 0x3u) << 10) | (r[1] >> 22);
        uint32_t mult = (r[1] >> 7) & 0x7u;
        uint32_t read_len = (r[2] >> 8) & 0xfu;
        sd.blocks = ((uint64_t)c_size + 1u) * (1ull << (mult + 2u)) * (1ull << read_len) /
                    BLOCK_BYTES;
    }
}

/* Gives the card an address and asks for its capacity. */
static int identify_card(void) {
    uint32_t csd[4] = {0};
    uint32_t response = 0;
    int rc = command(CMD_ALL_SEND_CID, 0, CMD_RESP_136 | CMD_CRC_CHECK, NULL);
    rc = rc != KERR_OK ? rc
                       : command(CMD_SEND_RELATIVE_ADDR, 0, CMD_RESP_48 | CMD_CRC_CHECK, &response);
    if (rc != KERR_OK) {
        return rc;
    }
    sd.rca = response >> 16;
    rc = command(CMD_SEND_CSD, sd.rca << 16, CMD_RESP_136 | CMD_CRC_CHECK, NULL);
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint32_t i = 0; i < 4u; i++) {
        csd[i] = read32(SDHCI_RESPONSE + i * 4u);
    }
    take_capacity(csd);
    rc = command(CMD_SELECT_CARD, sd.rca << 16, CMD_RESP_48_BUSY | CMD_CRC_CHECK, NULL);
    return rc != KERR_OK ? rc : command(CMD_SET_BLOCKLEN, BLOCK_BYTES, CMD_RESP_48, NULL);
}

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
    uint32_t argument = sd.high_capacity ? (uint32_t)lba : (uint32_t)(lba * BLOCK_BYTES);
    int rc;
    write16(SDHCI_BLOCK_SIZE, BLOCK_BYTES);
    write16(SDHCI_BLOCK_COUNT, 1);
    write16(SDHCI_TRANSFER_MODE, reading ? TRANSFER_READ : 0u);
    rc = command(reading ? CMD_READ_SINGLE : CMD_WRITE_SINGLE, argument,
                 CMD_RESP_48 | CMD_CRC_CHECK | CMD_DATA, NULL);
    return rc != KERR_OK ? rc : move_block(reading, buf);
}

static int sd_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    (void)self;
    if (!sd.up || count == 0 || lba + count > sd.blocks) {
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
    if (!sd.up || count == 0 || lba + count > sd.blocks) {
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
    rc = rc != KERR_OK ? rc : power_up_card();
    rc = rc != KERR_OK ? rc : identify_card();
    if (rc != KERR_OK) {
        return rc;
    }
    sd.up = true;
    *out = (struct blockdev){.read = sd_read, .write = sd_write, .flush = sd_flush, .self = NULL,
                             .block_size = BLOCK_BYTES, .blocks = sd.blocks, .read_only = false};
    return KERR_OK;
}

const char *sdhci_kind(void) {
    return sd.up ? (sd.high_capacity ? "SDHC/SDXC" : "SD") : "";
}

uint64_t sdhci_blocks(void) {
    return sd.blocks;
}
