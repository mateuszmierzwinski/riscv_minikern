#include "drivers/dwmmc.h"

#include "kernel/irqwait.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/riscv/mmu.h"
#include "arch/riscv/timer.h"
#include "core/console.h"
#include "core/errno.h"
#include "drivers/crg_jh7110.h"
#include "drivers/sdcard.h"

/* The DesignWare mobile-storage register map. Word registers, four bytes
 * apart, which is the part a reader should check first on any controller
 * after this project's UART lesson. */
#define DW_CTRL 0x000u
#define DW_PWREN 0x004u
#define DW_CLKDIV 0x008u
#define DW_CLKENA 0x010u
#define DW_TMOUT 0x014u
#define DW_CTYPE 0x018u
#define DW_BLKSIZ 0x01cu
#define DW_BYTCNT 0x020u
#define DW_INTMASK 0x024u
#define DW_CMDARG 0x028u
#define DW_CMD 0x02cu
#define DW_RESP0 0x030u
#define DW_RINTSTS 0x044u
#define DW_STATUS 0x048u
#define DW_FIFOTH 0x04cu
#define DW_CDETECT 0x050u
#define DW_VERID 0x06cu
#define DW_UHS_REG 0x074u
#define DW_BMOD 0x080u

/* CTRL. */
#define CTRL_RESET 0x1u
#define CTRL_FIFO_RESET 0x2u
#define CTRL_DMA_RESET 0x4u
#define CTRL_INT_ENABLE 0x10u

/* CMD. The flags that matter, and `START` which hands the command over. */
#define CMD_START 0x80000000u
#define CMD_USE_HOLD_REG 0x20000000u
#define CMD_UPDATE_CLOCK_ONLY 0x00200000u
#define CMD_SEND_INIT 0x00008000u
#define CMD_STOP_ABORT 0x00004000u
#define CMD_WAIT_PRVDATA 0x00002000u
#define CMD_WRITE 0x00000400u
#define CMD_DATA_EXPECTED 0x00000200u
#define CMD_CHECK_RESP_CRC 0x00000100u
#define CMD_RESP_LONG 0x00000080u
#define CMD_RESP_EXPECTED 0x00000040u

/* RINTSTS: what finished, and what went wrong. */
#define INT_CMD_DONE 0x00000004u
#define INT_DATA_OVER 0x00000008u
#define INT_TX_READY 0x00000010u
#define INT_RX_READY 0x00000020u
#define INT_RESP_ERR 0x00000002u
#define INT_RCRC 0x00000040u
#define INT_DCRC 0x00000080u
#define INT_RTO 0x00000100u
#define INT_DRTO 0x00000200u
#define INT_HTO 0x00000400u
#define INT_FRUN 0x00000800u
#define INT_HLE 0x00001000u
#define INT_SBE 0x00002000u
#define INT_EBE 0x00008000u
#define INT_ERRORS                                                                           \
    (INT_RESP_ERR | INT_RCRC | INT_DCRC | INT_RTO | INT_DRTO | INT_HTO | INT_FRUN | INT_HLE | \
     INT_SBE | INT_EBE)

/* STATUS. */
#define STATUS_DATA_BUSY 0x00000200u
#define STATUS_FIFO_EMPTY 0x00000004u
#define STATUS_FIFO_FULL 0x00000008u

#define WAIT_SPINS 40000000u

/* The names the JH7110's tree uses, and the generic one so this binds on any
 * board with the same controller. */
static const char *const compatibles[] = {"starfive,jh7110-mmc", "snps,dw-mshc"};

static struct {
    volatile uint32_t *regs;
    phys_t base;
    uint32_t fifo_offset;
    struct sd_card card;
    char path[64];
    bool up;
    bool trace;
    /* **The end-of-transfer interrupt**, armed only once the interrupt
     * controller exists — which is after this card has already been read,
     * because the root filesystem is on it. */
    struct irqwait done;
    bool irq_armed;
    bool arm_only;
    uint64_t interrupts_missed;
    uint64_t read_retries;
    /* Set once the card has been asked why a write failed, so the answer is
     * printed once per boot rather than once per refused block. */
    bool asked_why;
} mmc;

/* How long a transfer may take before the wait gives up. Generous, because
 * a card that is slow is not a card that is broken — and the number only
 * matters when the interrupt does not come, which is the case it exists to
 * make debuggable rather than silent. */
#define DATA_OVER_TIMEOUT_NS 2000000000ull

uint64_t dwmmc_interrupts_missed(void) {
    return mmc.interrupts_missed;
}

void dwmmc_arm_only(bool on) {
    mmc.arm_only = on;
}

void dwmmc_set_trace(bool on) {
    mmc.trace = on;
}

static void put(uint32_t off, uint32_t v) {
    mmc.regs[off / 4u] = v;
}

static uint32_t get(uint32_t off) {
    return mmc.regs[off / 4u];
}

/* The data FIFO's offset depends on the controller's version, which it
 * reports: 0x200 from version 0x240a, 0x100 before it. Reading the wrong one
 * transfers nothing and reports no error, because the address is a valid
 * register either way. */
static uint32_t fifo_offset_of(uint32_t verid) {
    return (verid & 0xffffu) >= 0x240au ? 0x200u : 0x100u;
}

/* Resets the controller's three parts and waits for it to say so. The bits
 * are self-clearing, and a controller that never clears them is one whose
 * clock is not running — which on this SoC is the first thing to suspect. */
static int reset_controller(void) {
    put(DW_CTRL, CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET);
    for (uint32_t spin = 0; spin < WAIT_SPINS; spin++) {
        if ((get(DW_CTRL) & (CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET)) == 0) {
            return KERR_OK;
        }
    }
    return KERR_TIMEDOUT;
}

/* Empties the FIFO before a data command.
 *
 * **Measured on the board: the first data read after bring-up overran**
 * (`RINTSTS 0x82c`, `FRUN` among the completion bits). Bring-up leaves words
 * in the FIFO — the controller is reset once, before the card is talked to —
 * so the first 512 bytes landed on top of them and the read returned `EIO`
 * with correct data. Nothing noticed while the first read was a whole-disk
 * superblock that the filesystem simply read again; a partition table read
 * once, and believed, turned it into "this disk has no partitions". */
static int reset_fifo(void) {
    put(DW_CTRL, get(DW_CTRL) | CTRL_FIFO_RESET);
    for (uint32_t spin = 0; spin < WAIT_SPINS; spin++) {
        if ((get(DW_CTRL) & CTRL_FIFO_RESET) == 0) {
            return KERR_OK;
        }
    }
    return KERR_TIMEDOUT;
}

/* Changes the card clock. The divider cannot be written while the clock is
 * running, so the sequence is: stop, set, start — each step handed to the
 * controller through a command with no index, which is how this controller
 * takes clock changes. */
static int set_clock_divider(uint32_t divider) {
    int rc;
    put(DW_CLKENA, 0);
    put(DW_CLKDIV, divider);
    rc = KERR_OK;
    put(DW_CMD, CMD_START | CMD_UPDATE_CLOCK_ONLY | CMD_WAIT_PRVDATA);
    for (uint32_t spin = 0; spin < WAIT_SPINS; spin++) {
        if ((get(DW_CMD) & CMD_START) == 0) {
            break;
        }
    }
    put(DW_CLKENA, 1u);
    put(DW_CMD, CMD_START | CMD_UPDATE_CLOCK_ONLY | CMD_WAIT_PRVDATA);
    for (uint32_t spin = 0; spin < WAIT_SPINS; spin++) {
        if ((get(DW_CMD) & CMD_START) == 0) {
            return rc;
        }
    }
    return KERR_TIMEDOUT;
}

/* Sends one command and waits for it to finish.
 *
 * The interrupt-status register is cleared **before** the command rather than
 * after: a stale completion bit from the previous command makes the next one
 * appear to finish instantly, and the response then read is the old one. */
static int dw_command(void *self, uint8_t index, uint32_t argument, uint32_t kind,
                      uint32_t *response) {
    /* **No hold register.** It retimes the command and data lines and is
     * only correct once the controller's delay chain has been configured,
     * which on this SoC means the `starfive,sysreg` setting this driver does
     * not touch yet. Setting it regardless is how a command that the card
     * answers correctly comes back with a CRC error. */
    uint32_t flags = CMD_START | CMD_WAIT_PRVDATA | index;
    uint32_t status;
    (void)self;
    if (kind != SD_RESP_NONE) {
        flags |= CMD_RESP_EXPECTED;
    }
    /* The CRC is checked for every response that has one — which the OCR
     * does not. */
    if (kind == SD_RESP_SHORT || kind == SD_RESP_LONG || kind == SD_RESP_BUSY) {
        flags |= CMD_CHECK_RESP_CRC;
    }
    if (kind == SD_RESP_LONG) {
        flags |= CMD_RESP_LONG;
    }
    if (index == CMD_GO_IDLE) {
        flags |= CMD_SEND_INIT; /* the 80 clocks a card needs before it listens */
    }
    /* **Which commands move data, all of them.** Listing only the
     * single-block pair left `CMD18` without `DATA_EXPECTED`, so the
     * controller sent the command and never started a transfer: the read
     * waited for a FIFO that nothing was filling and timed out. A command
     * that carries data has to say so, and the set is four, not two. */
    if (index == CMD_READ_SINGLE || index == CMD_READ_MULTIPLE || index == CMD_WRITE_SINGLE ||
        index == CMD_WRITE_MULTIPLE) {
        bool writing = index == CMD_WRITE_SINGLE || index == CMD_WRITE_MULTIPLE;
        flags |= CMD_DATA_EXPECTED | (writing ? CMD_WRITE : 0u);
    }
    /* A stop ends a transfer that is still running, which the controller has
     * to be told: without it the command queues behind the data it is meant
     * to interrupt. */
    if (index == CMD_STOP_TRANSMISSION) {
        flags |= CMD_STOP_ABORT;
        flags &= ~CMD_WAIT_PRVDATA;
    }
    put(DW_RINTSTS, 0xffffffffu);
    put(DW_CMDARG, argument);
    put(DW_CMD, flags);
    for (uint32_t spin = 0; spin < WAIT_SPINS; spin++) {
        status = get(DW_RINTSTS);
        if ((status & INT_ERRORS) != 0) {
            /* **Which** error, not just that there was one. The controller
             * distinguishes a response timeout (no card, or a clock that is
             * not reaching it) from a response CRC failure (it answered, and
             * the timing is wrong) from a data timeout — three different
             * faults that an `EIO` makes indistinguishable, and a board is
             * the only place to find out which. */
            if (mmc.trace) {
                kprintf("mmc: CMD%u failed, RINTSTS %08x%s%s%s%s%s\n", index,
                        (unsigned)status, (status & INT_RTO) != 0 ? " response-timeout" : "",
                        (status & INT_RCRC) != 0 ? " response-crc" : "",
                        (status & INT_DRTO) != 0 ? " data-timeout" : "",
                        (status & INT_HLE) != 0 ? " hardware-lock" : "",
                        (status & INT_RESP_ERR) != 0 ? " response-error" : "");
            }
            return KERR_IO;
        }
        if ((status & INT_CMD_DONE) != 0) {
            if (response != NULL) {
                /* A long response is four words; the lowest comes first,
                 * which is the order the shared protocol expects. */
                uint32_t words = kind == SD_RESP_LONG ? 4u : 1u;
                for (uint32_t i = 0; i < words; i++) {
                    response[i] = get(DW_RESP0 + i * 4u);
                }
            }
            return KERR_OK;
        }
    }
    return KERR_TIMEDOUT;
}

/* Moves one block through the FIFO, a word at a time.
 *
 * Used both for a single-block command and for each block of a multi-block
 * stream, which is why it waits on the FIFO level rather than on the
 * transfer-complete interrupt alone.
 *
 * The FIFO's level is in the status register and the "ready" interrupt bits
 * are the controller's invitation to move a burst; this waits on the level
 * directly, which is simpler and correct whatever the watermark is set to. */
static int dw_move_block(void *self, bool reading, void *buf) {
    uint32_t *words = buf;
    uint32_t moved = 0;
    (void)self;
    while (moved < SD_BLOCK_BYTES / 4u) {
        uint32_t status;
        bool progressed = false;
        for (uint32_t spin = 0; spin < WAIT_SPINS && !progressed; spin++) {
            status = get(DW_RINTSTS);
            if ((status & INT_ERRORS) != 0) {
                return KERR_IO;
            }
            status = get(DW_STATUS);
            if (reading && (status & STATUS_FIFO_EMPTY) == 0) {
                words[moved++] = get(mmc.fifo_offset);
                progressed = true;
            } else if (!reading && (status & STATUS_FIFO_FULL) == 0) {
                put(mmc.fifo_offset, words[moved++]);
                progressed = true;
            }
        }
        if (!progressed) {
            return KERR_TIMEDOUT;
        }
    }
    /* The words are in the FIFO's hands now. `DATA_OVER` marks the end of
     * the **whole** transfer, not of each block, so a multi-block read must
     * not wait for it between blocks — the controller raises it once, after
     * the last one. The block itself is complete when its words have moved,
     * which is what the loop above waited for. */
    return KERR_OK;
}

/* Waits for the controller to finish a transfer.
 *
 * `DATA_OVER` marks the end of the **whole** transfer and is raised once,
 * however many blocks it carried — so it is waited for after the last block
 * and not between them. Waiting between blocks of a multi-block read waits
 * for something that will not happen until the read is over. */
/* The controller's pending-interrupt word — read **and acknowledged**.
 *
 * `RINTSTS` is write-one-to-clear, so a handler that only reads it leaves
 * the controller asserting its line: the PLIC completes the interrupt, the
 * device immediately raises it again, and the hart never leaves the
 * handler. The machine does nothing else, with nothing printed, which is
 * exactly what happened the first time this was armed — the boot log stops
 * at the line announcing it.
 *
 * `irq.h` warns about this in general terms ("an interrupt nobody handles
 * will otherwise fire for ever"); the specific shape is that *handling* it
 * is not enough, the device has to be told. */
static uint32_t data_over_status(void) {
    uint32_t status = get(DW_RINTSTS);
    put(DW_RINTSTS, status);
    return status;
}

static int wait_for_data_over(void) {
    /* **Waited for, not watched, when the interrupt is wired.** The spin
     * below holds the kernel lock for the whole of a transfer, and one hart
     * is inside the kernel at a time — so the cost is not the spinning, it
     * is that nothing else can happen until a card has finished. With the
     * interrupt armed the hart sleeps or releases the lock and `wfi`s, and
     * the controller's completion is what ends the wait.
     *
     * The spin stays as the fallback, and is *reached* rather than kept for
     * tidiness: this driver runs before the interrupt controller is up — the
     * root filesystem is mounted from this card — so the first transfers of
     * every boot have no interrupt to wait for. A driver that assumed
     * otherwise would not be able to read the disk it was being loaded
     * from. */
    if (mmc.irq_armed) {
        int rc = irqwait_wait(&mmc.done, DATA_OVER_TIMEOUT_NS);
        uint32_t status = mmc.done.status;
        irqwait_rearm(&mmc.done);
        if (rc == KERR_OK) {
            return (status & INT_ERRORS) != 0 ? KERR_IO : KERR_OK;
        }
        /* A timeout falls through to one polled look, which distinguishes
         * "the controller never finished" from "the interrupt never
         * arrived" — and those want opposite investigations. */
        /* **The polled look is for the case where the interrupt never
         * came**, so it reads the register the handler would have cleared.
         * If the handler ran, `fired` was set and the wait returned above;
         * reaching here means it did not, so the bits are still there. */
        if ((get(DW_RINTSTS) & INT_DATA_OVER) != 0) {
            put(DW_RINTSTS, INT_DATA_OVER | INT_ERRORS);
            mmc.interrupts_missed++;
            return KERR_OK;
        }
        return KERR_TIMEDOUT;
    }
    for (uint32_t spin = 0; spin < WAIT_SPINS; spin++) {
        uint32_t status = get(DW_RINTSTS);
        if ((status & INT_ERRORS) != 0) {
            return KERR_IO;
        }
        if ((status & INT_DATA_OVER) != 0) {
            return KERR_OK;
        }
    }
    return KERR_TIMEDOUT;
}

/* Arms the end-of-transfer interrupt, once the interrupt controller exists.
 *
 * **Separate from the probe**, because the probe runs before the PLIC is up
 * — this card holds the root filesystem, so it is read first. Called again
 * later, which is why it is idempotent. */
int dwmmc_use_interrupts(const struct fdt *fdt) {
    uint32_t source = 0;
    int rc;
    if (!mmc.up || mmc.irq_armed) {
        return mmc.up ? KERR_OK : KERR_NODEV;
    }
    /* The same way `virtio.c` reads its own: the first cell of
     * `interrupts`, which on this board is the PLIC source number. */
    source = fdt_get_u32(fdt, mmc.path, "interrupts", 0);
    rc = source == 0u ? KERR_NOENT : KERR_OK;
    if (rc != KERR_OK) {
        kprintf("mmc: no interrupt in the tree for %s; transfers stay polled\n", mmc.path);
        return KERR_NOENT;
    }
    rc = irqwait_arm(&mmc.done, source, data_over_status);
    if (rc != KERR_OK) {
        kprintf("mmc: interrupt %u could not be armed (rc %d); transfers stay polled\n", source,
                rc);
        return rc;
    }
    /* **The mask last.** With it open before the handler is installed, the
     * first completion is an interrupt nobody claims — which the PLIC will
     * re-raise for ever, and the machine does nothing else. */
    put(DW_INTMASK, INT_DATA_OVER | INT_ERRORS);
    put(DW_CTRL, get(DW_CTRL) | CTRL_INT_ENABLE);
    /* The bisect: with `minikern.mmcirqonly` the handler is installed and
     * the device is unmasked, but the wait stays polled — which separates
     * "unmasking storms" from "the wait is wrong". */
    mmc.irq_armed = !mmc.arm_only;
    kprintf("mmc: end-of-transfer interrupt %u armed; the lock is free while a transfer runs\n",
            source);
    return KERR_OK;
}

/* Four-bit transfers and a faster clock, once the card has agreed to both. */
static int dw_run_faster(void *self) {
    (void)self;
    put(DW_CTYPE, 1u); /* one card, four-bit bus */
    /* **25 MHz, which is every SD card's default speed.**
     *
     * The controller divides its input by `2 * CLKDIV`, and the tree says
     * the input is 50 MHz, so 1 gives 25 MHz. That is the rate the standard
     * guarantees without negotiating anything: going above it means
     * high-speed mode, which has to be asked for with CMD6 and is not asked
     * for here.
     *
     * It was 2 — 12.5 MHz — which was chosen to be safe at an unknown input
     * clock and turned out to be the thing limiting reads: measured on a
     * VisionFive 2, 5.0 MB/s at 12.5 MHz against a four-bit bus that should
     * carry twice that. */
    return set_clock_divider(1u);
}

static const struct sd_transport transport = {
    .self = NULL,
    .command = dw_command,
    .move_block = dw_move_block,
    .run_faster = dw_run_faster,
};

/* Asks the card what it thinks, after the controller has reported a failure.
 *
 * **The card's own answer, not the controller's.** A response timeout from
 * the controller says "nothing came back"; it does not say why, and the two
 * candidates — the card refusing a write and the host asking wrongly — want
 * opposite fixes. R1 carries both: bit 26 is a write-protect violation, bit
 * 25 says the card is locked, and bits 9..12 are the state it is in (4 is
 * `tran`, which is where a write may be issued, and 5 is `data`, which means
 * a previous transfer never ended).
 *
 * Issued only after a failure, so it costs a command on the path that is
 * already broken and nothing on the one that works. */
static void ask_the_card_why(uint8_t failed_index) {
    uint32_t r1 = 0;
    if (dw_command(NULL, CMD_SEND_STATUS, mmc.card.rca << 16, SD_RESP_SHORT, &r1) != KERR_OK) {
        kprintf("mmc: CMD%u failed and the card will not report its status either\n",
                failed_index);
        return;
    }
    kprintf("mmc: after CMD%u the card says R1 %08x: state %u%s%s\n", failed_index, r1,
            (r1 >> 9) & 0xfu, (r1 & (1u << 26)) != 0 ? ", write-protect violation" : "",
            (r1 & (1u << 25)) != 0 ? ", locked" : "");
}

/* Waits for the card to leave the programming state.
 *
 * **This is what made writes fail, and the card said so.** After a write the
 * card goes to `prg` and programs the block; it will not accept another
 * command until it returns to `tran`. The driver issued the next one
 * regardless, and the card answered with a response timeout — from which the
 * host learns nothing. Asking it instead, with `CMD13`, gives:
 *
 *     mmc: after CMD24 the card says R1 00400e00: state 7
 *
 * State 7 is `prg`, and bit 22 is `ILLEGAL_COMMAND` — the card refusing a
 * write it is not in a state to take. So the first write of a boot succeeded,
 * left the card programming, and every write after it failed. That is
 * exactly the shape of "writes to the SD card work sometimes".
 *
 * Polled with `CMD13` rather than by watching the controller's busy line,
 * because the state is what the next command actually depends on and the
 * card is the authority on it. The limit is generous: a card may take
 * hundreds of milliseconds to program, and a wait that gave up early would
 * turn a slow card into a broken one.
 */
#define PROGRAMMING_TIMEOUT_NS 1000000000ull
#define CARD_STATE_TRAN 4u

static int wait_until_ready_for_a_command(void) {
    uint64_t until = timer_ns() + PROGRAMMING_TIMEOUT_NS;
    while (timer_ns() < until) {
        uint32_t r1 = 0;
        if (dw_command(NULL, CMD_SEND_STATUS, mmc.card.rca << 16, SD_RESP_SHORT, &r1) != KERR_OK) {
            continue; /* the card is too busy to answer: ask again */
        }
        if (((r1 >> 9) & 0xfu) == CARD_STATE_TRAN) {
            return KERR_OK;
        }
    }
    kprintf("mmc: the card did not return to the transfer state\n");
    return KERR_TIMEDOUT;
}

static int one_block(bool reading, uint64_t lba, void *buf) {
    uint32_t argument = mmc.card.high_capacity ? (uint32_t)lba : (uint32_t)(lba * SD_BLOCK_BYTES);
    int rc = reset_fifo();
    if (rc != KERR_OK) {
        return rc;
    }
    put(DW_BLKSIZ, SD_BLOCK_BYTES);
    put(DW_BYTCNT, SD_BLOCK_BYTES);
    rc = dw_command(NULL, reading ? CMD_READ_SINGLE : CMD_WRITE_SINGLE, argument, SD_RESP_SHORT,
                    NULL);
    if (rc != KERR_OK && !reading && !mmc.asked_why) {
        /* Once per boot: the answer does not change between blocks, and a
         * line per failed block buries it. */
        mmc.asked_why = true;
        ask_the_card_why(CMD_WRITE_SINGLE);
    }
    rc = rc != KERR_OK ? rc : dw_move_block(NULL, reading, buf);
    rc = rc != KERR_OK ? rc : wait_for_data_over();
    if (rc != KERR_OK || reading) {
        return rc;
    }
    /* **Only after a write.** A read leaves the card in `tran` already, so
     * asking would cost a command per block for nothing. */
    return wait_until_ready_for_a_command();
}

/* Reads several blocks with **one command**.
 *
 * A card streams blocks until it is told to stop, so `count` blocks cost one
 * command, one stop and `count` FIFO drains — against `count` commands, each
 * with its own round trip to the card and its own completion wait. The
 * difference is most of the time for anything larger than a single block,
 * which is every filesystem read of a file.
 *
 * The stop command is sent whatever happened, including after an error: a
 * card left streaming answers the *next* command with data rather than a
 * response, and the driver that sent it then reads a response out of a data
 * block. One failed read would corrupt every read after it. */
static int read_many(uint64_t lba, uint32_t count, uint8_t *to) {
    uint32_t argument = mmc.card.high_capacity ? (uint32_t)lba : (uint32_t)(lba * SD_BLOCK_BYTES);
    int rc = reset_fifo();
    if (rc != KERR_OK) {
        return rc;
    }
    put(DW_BLKSIZ, SD_BLOCK_BYTES);
    put(DW_BYTCNT, count * SD_BLOCK_BYTES);
    rc = dw_command(NULL, CMD_READ_MULTIPLE, argument, SD_RESP_SHORT, NULL);
    for (uint32_t i = 0; rc == KERR_OK && i < count; i++) {
        rc = dw_move_block(NULL, true, to + (size_t)i * SD_BLOCK_BYTES);
    }
    if (rc == KERR_OK) {
        rc = wait_for_data_over();
    }
    {
        int stop = dw_command(NULL, CMD_STOP_TRANSMISSION, 0, SD_RESP_BUSY, NULL);
        rc = rc != KERR_OK ? rc : stop;
    }
    return rc;
}

static int read_once(uint64_t lba, uint32_t count, void *buf) {
    return count == 1u ? one_block(true, lba, buf) : read_many(lba, count, buf);
}

/* A read, **retried** when it fails (two more attempts at most).
 *
 * Measured on the board: the first data read after bring-up overruns the
 * FIFO (`RINTSTS 0x82c`, `FRUN`) and returns `EIO` with correct data, and
 * emptying the FIFO first does not prevent it — the cause is in the
 * controller's first transfer at the new clock and width, not identified.
 * A read changes nothing on the card, so another attempt is safe; it
 * succeeds, and `dwmmc_read_retries` counts how often that was needed so
 * the workaround stays visible. A partition table read once and believed
 * had turned the overrun into "this card has no partitions". */
static int dw_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    int rc;
    (void)self;
    if (count == 0) {
        return KERR_INVAL;
    }
    rc = read_once(lba, count, buf);
    /* Up to two more tries: one was not always enough — a boot on
     * 2026-10-28 failed a read twice running, and the partition table that
     * read was for went unseen. */
    for (uint32_t again = 0; rc == KERR_IO && again < 2u; again++) {
        mmc.read_retries++;
        rc = read_once(lba, count, buf);
    }
    return rc;
}

uint64_t dwmmc_read_retries(void) {
    return mmc.read_retries;
}

static int dw_write(void *self, uint64_t lba, uint32_t count, const void *buf) {
    const uint8_t *from = buf;
    (void)self;
    for (uint32_t i = 0; i < count; i++) {
        /* The block is copied because the transport writes words from a
         * mutable pointer; the caller's buffer is const. */
        uint8_t block[SD_BLOCK_BYTES];
        int rc;
        for (uint32_t b = 0; b < SD_BLOCK_BYTES; b++) {
            block[b] = from[(size_t)i * SD_BLOCK_BYTES + b];
        }
        rc = one_block(false, lba + i, block);
        if (rc != KERR_OK) {
            return rc;
        }
    }
    return KERR_OK;
}

static int dw_flush(void *self) {
    (void)self;
    /* Nothing is held back: every write has been acknowledged by the card
     * before its command returned. */
    return KERR_OK;
}

static void remember_path(const char *path) {
    for (uint32_t i = 0; i < sizeof mmc.path - 1u; i++) {
        mmc.path[i] = path[i];
        if (path[i] == '\0') {
            return;
        }
    }
}

/* Brings up one controller and looks for a card in it.
 *
 * Separate from the search because **a board has more than one**: a
 * VisionFive 2 has an eMMC socket at 0x16010000 and the SD slot at
 * 0x16020000, and the first one is usually empty. A probe that stopped at
 * the first controller it found reported "no card" on a board with a card
 * in it. */
static int try_controller(const struct fdt *fdt, const char *path, phys_t base, uint64_t size,
                          struct blockdev *out) {
    void *at = mmu_map_device(base, size);
    int rc;
    if (at == NULL) {
        return KERR_NOMEM;
    }
    mmc.regs = (volatile uint32_t *)at;
    mmc.base = base;
    remember_path(path);
    mmc.up = false;
    /* **Its clocks and its reset first.** A controller that is not clocked
     * accepts every write and answers none of them, which looks exactly like
     * a controller with no card in it — the lesson this project paid for on
     * a thermal sensor that had never been switched on. */
    rc = crg_enable_device(fdt, path);
    if (rc != KERR_OK) {
        kprintf("mmc: %s could not be clocked (rc %d)\n", path, rc);
        return rc;
    }
    rc = reset_controller();
    if (rc != KERR_OK) {
        kprintf("mmc: %s did not come out of reset\n", path);
        return rc;
    }
    mmc.fifo_offset = fifo_offset_of(get(DW_VERID));
    put(DW_PWREN, 1u);
    /* **The delay the tree asks for.** `post-power-on-delay-ms` is 200 on
     * this board, and a card that has not finished powering up answers
     * commands with errors rather than with nothing — which reads as a
     * broken controller. The tree is asked rather than a constant assumed,
     * because the number belongs to the board's regulator and not to the
     * controller. */
    {
        uint32_t ms = fdt_get_u32(fdt, path, "post-power-on-delay-ms", 10u);
        for (uint32_t i = 0; i < ms; i++) {
            /* No timer yet on some paths, so this is a calibrated spin: the
             * loop count is generous because being slow here costs
             * milliseconds once, and being quick costs a card that never
             * answers. */
            for (uint32_t spin = 0; spin < 200000u; spin++) {
                __asm__ volatile("" ::: "memory");
            }
        }
    }
    put(DW_INTMASK, 0); /* polled until `dwmmc_use_interrupts`, below */
    put(DW_TMOUT, 0xffffffffu);
    put(DW_BMOD, 0);          /* no internal DMA: the FIFO is the data path */
    put(DW_FIFOTH, (0x2u << 28) | ((8u - 1u) << 16) | 8u);
    put(DW_CTYPE, 0);         /* one bit until the card agrees to four */
    put(DW_UHS_REG, 0);
    /* 400 kHz for identification, which is the rate the protocol requires
     * before a card has been addressed. The divider halves twice: the
     * controller divides by `2 * CLKDIV`, and the tree says its input is
     * 50 MHz (`assigned-clock-rates`), so 62 gives 403 kHz. The first
     * attempt used 125 and ran at 200 kHz — legal, and a reminder that this
     * number is arithmetic rather than a magic constant. */
    rc = set_clock_divider(62u);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = sdcard_bring_up(&transport, &mmc.card);
    if (rc != KERR_OK) {
        /* An empty socket, which is the ordinary state of the eMMC one.
         * Reported at a glance rather than as a failure, because the next
         * controller is the interesting one. */
        kprintf("mmc: %s: no card (rc %d, ocr %08x after %u polls)\n", path, rc,
                (unsigned)mmc.card.last_ocr, mmc.card.polls);
        return KERR_NODEV;
    }
    mmc.up = true;
    *out = (struct blockdev){.read = dw_read,
                             .write = dw_write,
                             .flush = dw_flush,
                             .self = NULL,
                             .block_size = SD_BLOCK_BYTES,
                             .blocks = mmc.card.blocks,
                             .read_only = false};
    kprintf("mmc: %s, %llu blocks of %u bytes (%llu MiB)%s\n", path,
            (unsigned long long)mmc.card.blocks, SD_BLOCK_BYTES,
            (unsigned long long)(mmc.card.blocks / 2048u),
            mmc.card.high_capacity ? ", high capacity" : "");
    return KERR_OK;
}

int dwmmc_init(const struct fdt *fdt, struct blockdev *out) {
    /* **Every** controller the tree names, in order, until one has a card.
     * The names are tried in turn because a board may describe its
     * controllers under the vendor's compatible, the generic one, or both. */
    for (uint32_t which = 0; which < 2u; which++) {
        for (uint32_t skip = 0; skip < 4u; skip++) {
            char path[64];
            phys_t base = 0;
            uint64_t size = 0;
            if (fdt_find_compatible_nth(fdt, compatibles[which], skip, path, sizeof path) !=
                KERR_OK) {
                break;
            }
            if (fdt_device_reg(fdt, path, 0, &base, &size) != KERR_OK) {
                continue;
            }
            if (try_controller(fdt, path, base, size, out) == KERR_OK) {
                return KERR_OK;
            }
        }
    }
    return KERR_NOENT;
}

const char *dwmmc_path(void) {
    return mmc.up ? mmc.path : "";
}

uint64_t dwmmc_blocks(void) {
    return mmc.card.blocks;
}
