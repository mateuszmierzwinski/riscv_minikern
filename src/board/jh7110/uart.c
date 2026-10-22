#include "board/jh7110/uart.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/riscv/mmu.h"
#include "core/console.h"
#include "core/errno.h"

/* 16550 registers, one byte each. The JH7110 and QEMU's ns16550a agree on
 * these; a part that needs 32-bit accesses would need its own reg-shift,
 * which the device tree provides and this driver would have to honour. */
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
    char path[64];
    bool up;
} uart;

static void put(uint32_t reg, uint8_t v) {
    uart.regs[reg] = v;
}

static uint8_t get(uint32_t reg) {
    return uart.regs[reg];
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
    remember_path(path);
    /* The FIFOs on and receive interrupts off: this console is polled, and a
     * device left asserting an interrupt nobody handles is how a board hangs
     * the moment interrupts are enabled. The baud rate is left exactly as the
     * firmware set it (uart.h). */
    put(UART_IER, 0);
    put(UART_FCR, 0x07u); /* enable, and clear both FIFOs */
    uart.up = true;
    *out = (struct console_port){.write = uart_write, .self = NULL};
    return KERR_OK;
}

const char *jh7110_uart_path(void) {
    return uart.up ? uart.path : "";
}

phys_t jh7110_uart_base(void) {
    return uart.base;
}
