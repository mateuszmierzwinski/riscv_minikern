/* uart — a 16550-compatible UART, found in the device tree.
 *
 * The JH7110's UART0 is a Synopsys DesignWare APB UART, which is 16550
 * compatible; QEMU's `virt` machine offers an `ns16550a`. **The same driver
 * binds to both**, because it asks the device tree where the registers are
 * instead of being told at compile time — which is the whole point of doing
 * it this way, and the pattern every later driver here follows.
 *
 * This replaces the SBI console, which costs a trap per character. The SBI
 * one stays available and is what prints before the device tree has been
 * read: on a board whose UART this kernel has not been told about, firmware's
 * console is the only way to say so.
 *
 * It is **polled**, deliberately. A console that needs interrupts cannot
 * report a problem in the interrupt controller, and the first thing a kernel
 * must be able to do is explain why it cannot do anything else.
 */
#ifndef MINIKERN_BOARD_JH7110_UART_H
#define MINIKERN_BOARD_JH7110_UART_H

#include <stdbool.h>

#include "core/fdt.h"
#include "core/ports.h"
#include "core/types.h"

/* Finds a 16550 in the tree, maps its registers, and takes it over.
 *
 * It does **not** reprogram the baud rate: the firmware that printed its own
 * banner through this UART has already set one that works, and a kernel that
 * recomputes a divisor from a clock frequency it has guessed at goes silent
 * at exactly the moment it would have told you why. The divisor is only
 * written when `clock-frequency` is in the tree and a baud rate is asked for.
 *
 * KERR_OK with `*out` filled, KERR_NOENT when the tree names no 16550, or an
 * error from the mapping. */
int jh7110_uart_init(const struct fdt *fdt, struct console_port *out);

/* Where it was found, for the boot log: the node's path and its address. */
const char *jh7110_uart_path(void);
phys_t jh7110_uart_base(void);

#endif
