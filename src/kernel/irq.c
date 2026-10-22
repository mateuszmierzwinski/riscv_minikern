#include "kernel/irq.h"

#include <stdbool.h>
#include <stddef.h>

#include "arch/riscv/plic.h"
#include "core/console.h"
#include "core/errno.h"

static struct irq_handler handlers[IRQ_MAX_SOURCES];
static bool complained[IRQ_MAX_SOURCES];
static uint64_t dispatched;
static uint64_t spurious;

int irq_register(uint32_t source, const struct irq_handler *h) {
    if (source == 0 || source >= IRQ_MAX_SOURCES || h->handle == NULL) {
        return KERR_INVAL;
    }
    if (handlers[source].handle != NULL) {
        return KERR_EXIST;
    }
    handlers[source] = *h;
    plic_enable(source);
    return KERR_OK;
}

void irq_dispatch(uint32_t source) {
    if (source >= IRQ_MAX_SOURCES || handlers[source].handle == NULL) {
        spurious++;
        /* Said once per source. An interrupt with no handler re-asserts
         * immediately, so a message per occurrence is a machine that does
         * nothing but print — and the source is disabled for the same
         * reason: left enabled, it would starve everything else. */
        if (source < IRQ_MAX_SOURCES && !complained[source]) {
            complained[source] = true;
            kprintf("irq: source %u has no handler; disabling it\n", source);
            plic_disable(source);
        }
        return;
    }
    dispatched++;
    handlers[source].handle(handlers[source].self);
}

uint64_t irq_count(void) {
    return dispatched;
}

uint64_t irq_spurious(void) {
    return spurious;
}
