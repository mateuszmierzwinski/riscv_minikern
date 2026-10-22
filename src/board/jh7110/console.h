/* console — the board's console, as a console_port.
 *
 * Two implementations, and the kernel starts on the first:
 *
 *   **SBI**, which costs a trap per character and needs no knowledge of the
 *   machine at all. It is what prints the banner, including on a board whose
 *   UART this kernel has never seen.
 *
 *   **The 8250 itself** (P1), once the device tree has said where it is. The
 *   JH7110's UART0 is a standard 16550-compatible part, which is the one
 *   piece of the ARM board code that ports almost unchanged.
 *
 * Starting on SBI is a deliberate inversion of how the ARM kernel came up:
 * there, owning the UART was a prerequisite for seeing anything, so the
 * first bring-up had to be debugged by writing characters to a physical
 * address and counting how far it got. Here the firmware's console works
 * before the kernel knows what machine it is on.
 */
#ifndef MINIKERN_BOARD_JH7110_CONSOLE_H
#define MINIKERN_BOARD_JH7110_CONSOLE_H

#include "core/ports.h"

/* The console the firmware already owns. Always available. */
struct console_port jh7110_sbi_console(void);

#endif
