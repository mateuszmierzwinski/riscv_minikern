/* plic — the Platform-Level Interrupt Controller: where device interrupts
 * arrive.
 *
 * One PLIC serves the whole machine, with a **context** per hart per
 * privilege level. That indirection is the thing to get right: a hart's
 * enable bits and its priority threshold live at an offset derived from its
 * *context number*, not from its hart id — and on a SoC with a monitor core,
 * those are not the same thing. The JH7110 pairs an S7 with four U74s, and
 * the firmware takes M-mode contexts, so the supervisor context for hart *n*
 * is not context *n*.
 *
 * This is the RISC-V counterpart of the ARM kernel's GIC, and the shape is
 * familiar: a distributor-like global part (priorities, pending) and a
 * per-hart part (enables, threshold, claim/complete). What is different is
 * that claiming and completing are reads and writes of **one register**, so
 * the handler cannot forget to end the interrupt without also failing to
 * take the next one — the GIC let you forget the EOI and merely stop
 * receiving.
 */
#ifndef MINIKERN_ARCH_RISCV_PLIC_H
#define MINIKERN_ARCH_RISCV_PLIC_H

#include <stdbool.h>
#include <stdint.h>

#include "core/fdt.h"
#include "core/types.h"

#define PLIC_MAX_SOURCES 1024u

/* Finds the PLIC in the device tree, maps it, and works out which context
 * this hart's supervisor interrupts arrive in — by reading the controller's
 * `interrupts-extended` list, which names a hart and a privilege level per
 * context. Guessing it is hart-id-shaped is the mistake this is written to
 * avoid. */
int plic_init(const struct fdt *fdt, uint64_t hart);

/* Lets source `irq` through to this hart, at the lowest priority that is
 * still above the threshold. */
void plic_enable(uint32_t irq);
void plic_disable(uint32_t irq);

/* The highest-priority pending source, or 0 when there is none. Claiming it
 * also clears its pending bit. */
uint32_t plic_claim(void);

/* Says the source has been handled, which is what lets it fire again. A
 * handler that returns without this takes the interrupt once and never
 * again. */
void plic_complete(uint32_t irq);

/* Where it was found and which context this hart uses, for the boot log —
 * the context is worth printing because it is the part nobody can guess. */
phys_t plic_base(void);
uint32_t plic_context(void);
bool plic_ready(void);

#endif
