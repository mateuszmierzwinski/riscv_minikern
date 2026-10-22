/* irq — one handler per interrupt source.
 *
 * Thinner than its counterpart on the architecture this migrated from,
 * because the controller does more: `plic.h` owns enabling, claiming and
 * completing, so what is left here is a table and a dispatch. There is no
 * "interrupt chip port" to abstract, because there is one controller
 * standard across the architecture rather than one per vendor.
 *
 * Handlers run **with interrupts masked**, on whatever kernel stack the
 * interrupt landed on, and are expected to be short: a handler that wants to
 * do real work wakes something that can.
 */
#ifndef MINIKERN_KERNEL_IRQ_H
#define MINIKERN_KERNEL_IRQ_H

#include <stdint.h>

/* The PLIC's architectural maximum. The JH7110 uses 136 sources and QEMU's
 * virt machine fewer; sizing for the limit costs 16 KiB of .bss and means no
 * board can present a source this table cannot hold. */
#define IRQ_MAX_SOURCES 1024u

struct irq_handler {
    void (*handle)(void *self);
    void *self;
};

/* Installs a handler and enables the source at the controller. KERR_INVAL
 * for an out-of-range source or a source that already has a handler —
 * silently replacing one is how two drivers come to share an interrupt and
 * each believe it is theirs. */
int irq_register(uint32_t source, const struct irq_handler *h);

/* Runs the handler for a claimed source. Called from the trap path, which
 * completes the interrupt afterwards. An unclaimed source is counted and
 * reported once, because an interrupt nobody handles will otherwise fire for
 * ever and the machine will do nothing else. */
void irq_dispatch(uint32_t source);

/* How many interrupts have been dispatched, and how many had no handler. */
uint64_t irq_count(void);
uint64_t irq_spurious(void);

#endif
