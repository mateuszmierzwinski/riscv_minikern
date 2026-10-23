#include "board/jh7110/uart.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/riscv/mmu.h"
#include "core/console.h"
#include "core/errno.h"

/* 16550 register *numbers*. How far apart they actually sit, and how wide
 * each access must be, is **the board's business and the device tree says
 * so**:
 *
 *   - QEMU's `ns16550a` has no `reg-shift`, so the registers are one byte
 *     apart and a byte access reaches them.
 *   - The **JH7110's `snps,dw-apb-uart` has `reg-shift = <2>` and
 *     `reg-io-width = <4>`**: the registers are four bytes apart and must be
 *     read and written as 32-bit words.
 *
 * This driver assumed one byte apart, which was right on the emulator and
 * wrong on the board — and the failure was not silence but *garbling*. The
 * transmit register is at offset 0 either way, so characters went out; the
 * line-status register is number 5, which with a shift of two is at 0x14 and
 * without it is a byte inside the interrupt-enable register. So the
 * "is the transmitter ready?" test read an unrelated register, the driver
 * wrote without waiting, the FIFO overran, and most of the output
 * disappeared. The board printed its boot banner through SBI perfectly and
 * then became unreadable the moment the kernel took the port over.
 *
 * No emulator could have shown this: QEMU's shift is zero. */
#define UART_THR 0u  /* write: transmit */
#define UART_RBR 0u  /* read: receive */
#define UART_IER 1u
#define UART_FCR 2u
#define UART_LCR 3u
#define UART_LSR 5u

#define LSR_DATA_READY 0x01u
#define LSR_THR_EMPTY 0x20u

/* The two strings a 16550 on this architecture answers to. The DesignWare
 * one comes first because it is what the JH7110's own tree says, and the
 * generic one is what an emulator says. */
static const char *const compatibles[] = {"snps,dw-apb-uart", "ns16550a"};

static struct {
    volatile uint8_t *regs;
    phys_t base;
    /* From the tree: how far apart the registers are (as a shift) and how
     * wide each access must be. */
    uint32_t reg_shift;
    uint32_t io_width;
    char path[64];
    bool up;
} uart;

/* One register's address, honouring the board's spacing. */
static volatile void *at(uint32_t reg) {
    return (volatile void *)(uart.regs + ((size_t)reg << uart.reg_shift));
}

static void put(uint32_t reg, uint8_t v) {
    /* The width matters as much as the spacing: a part that wants 32-bit
     * accesses may ignore a byte write entirely, or take it and leave the
     * other three bytes of the word undefined. */
    if (uart.io_width == 4u) {
        *(volatile uint32_t *)at(reg) = v;
    } else {
        *(volatile uint8_t *)at(reg) = v;
    }
}

static uint8_t get(uint32_t reg) {
    if (uart.io_width == 4u) {
        return (uint8_t)*(volatile uint32_t *)at(reg);
    }
    return *(volatile uint8_t *)at(reg);
}

/* Waits for the transmit holding register to drain. Bounded, because a
 * console that spins for ever on a UART that is not there turns a diagnostic
 * into a hang — which is the opposite of what a console is for. */
static void wait_for_room(void) {
    for (uint32_t spin = 0; spin < 1000000u; spin++) {
        if ((get(UART_LSR) & LSR_THR_EMPTY) != 0) {
            return;
        }
    }
}

/* **The receive side.** Polled, one byte at a time, and non-blocking: the
 * caller is a line discipline that has other things to do, and a UART at
 * 115200 delivers a byte every 87 microseconds, which no amount of spinning
 * improves on.
 *
 * `LSR_DATA_READY` and `UART_RBR` have been defined in this file since it
 * was written and never used, because the console was a write-only sink.
 * Reading goes through the same `get` as everything else, so it inherits
 * the spacing and width the device tree gave — which is the whole reason
 * this driver works on both the emulator and the board.
 *
 * Returns -1 when there is nothing, rather than 0: a received NUL is a
 * byte, and a reader that cannot tell it from "no byte" loses it. */
int jh7110_uart_getc(void) {
    if (uart.regs == NULL || (get(UART_LSR) & LSR_DATA_READY) == 0) {
        return -1;
    }
    return (int)get(UART_RBR);
}

static void uart_write(void *self, const char *data, size_t len) {
    (void)self;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            wait_for_room();
            put(UART_THR, '\r');
        }
        wait_for_room();
        put(UART_THR, (uint8_t)data[i]);
    }
}

/* Copies a path into the driver's own storage: the one the finder filled
 * points into a caller's buffer. */
static void remember_path(const char *path) {
    uint32_t i = 0;
    while (path[i] != '\0' && i + 1u < sizeof uart.path) {
        uart.path[i] = path[i];
        i++;
    }
    uart.path[i] = '\0';
}

/* Looks for each compatible string in turn. */
static int find_uart(const struct fdt *fdt, char *path, uint32_t cap, phys_t *base, uint64_t *size) {
    for (uint32_t i = 0; i < sizeof compatibles / sizeof compatibles[0]; i++) {
        if (fdt_find_compatible(fdt, compatibles[i], path, cap) != KERR_OK) {
            continue;
        }
        if (fdt_device_reg(fdt, path, 0, base, size) == KERR_OK) {
            return KERR_OK;
        }
    }
    return KERR_NOENT;
}

int jh7110_uart_init(const struct fdt *fdt, struct console_port *out) {
    char path[64];
    phys_t base = 0;
    uint64_t size = 0;
    int rc = find_uart(fdt, path, sizeof path, &base, &size);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = mmu_map_device(base, size);
    if (rc != KERR_OK) {
        return rc;
    }
    uart.regs = (volatile uint8_t *)(uintptr_t)base;
    uart.base = base;
    /* **How the registers are laid out, from the tree rather than assumed.**
     * `reg-shift` is how far apart they are as a power of two and
     * `reg-io-width` how wide each access must be; both default to the
     * generic 16550's one-byte arrangement, which is what an emulator
     * describes and what this driver used to hard-code. The JH7110 says 2
     * and 4. */
    uart.reg_shift = fdt_get_u32(fdt, path, "reg-shift", 0u);
    uart.io_width = fdt_get_u32(fdt, path, "reg-io-width", 1u);
    remember_path(path);
    /* The FIFOs on and receive interrupts off: this console is polled, and a
     * device left asserting an interrupt nobody handles is how a board hangs
     * the moment interrupts are enabled. The baud rate is left exactly as the
     * firmware set it (uart.h). */
    put(UART_IER, 0);
    put(UART_FCR, 0x07u); /* enable, and clear both FIFOs */
    uart.up = true;
    /* Reported, because getting it wrong is not silence but garbling — and
     * garbled output is the hardest thing to diagnose with nothing but
     * garbled output to read. */
    kprintf("uart: registers %u bytes apart, %u-byte accesses\n",
            1u << uart.reg_shift, uart.io_width);
    *out = (struct console_port){.write = uart_write, .self = NULL};
    return KERR_OK;
}

const char *jh7110_uart_path(void) {
    return uart.up ? uart.path : "";
}

phys_t jh7110_uart_base(void) {
    return uart.base;
}
