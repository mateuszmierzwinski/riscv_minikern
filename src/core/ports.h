/* ports.h — every interface the core consumes, in one place.
 *
 * The core is the hexagon's centre: it knows these ports and nothing else.
 * Board and arch code (src/board, src/arch) implement them, and the
 * composition root (src/board/<board>/main.c) wires concrete adapters in.
 * The core never includes a board or arch header. */
#ifndef MINIKERN_CORE_PORTS_H
#define MINIKERN_CORE_PORTS_H

#include <stddef.h>
#include <stdint.h>

/* A character device the console can write to (a UART, later a framebuffer
 * or a network log). write delivers len bytes as-is: newline translation is
 * the device's business, because a serial terminal and a file differ. */
struct console_port {
    void (*write)(void *self, const char *buf, size_t len);
    /* The bytes exactly as given — no CR before each LF. NULL when the
     * device cannot (the SBI firmware adds the CR itself). */
    void (*write_untranslated)(void *self, const char *buf, size_t len);
    void *self;
};

/* Bytes typed at a console. read blocks until at least one byte is there
 * (or returns a negative errno, e.g. when interrupted) and returns how
 * many it copied, at most n. */
struct console_input_port {
    int32_t (*read)(void *self, void *buf, uint32_t n);
    int (*ready)(void *self);  /* a read would not block */
    void *self;
};

/* What the machine can do when the kernel has nothing left to run. */
struct power_port {
    void (*idle)(void *self);      /* wait for an interrupt; may return */
    void (*poweroff)(void *self);  /* returns only if the firmware refused */
    void *self;
};

/* Whole pages of kernel memory, mapped and addressable by the kernel. alloc
 * returns KERR_OK with *virt set to npages contiguous, page-aligned bytes,
 * or KERR_NOMEM; free hands back exactly what one alloc returned. */
struct page_port {
    int (*alloc)(void *self, uint32_t npages, void **virt);
    void (*free)(void *self, void *virt, uint32_t npages);
    void *self;
};

/* A network device: sends one complete Ethernet frame (no FCS). Returns
 * KERR_OK, or KERR_AGAIN when its transmit ring is full. */
struct netdev_port {
    int (*send)(void *self, const uint8_t *frame, uint32_t len);
    void *self;
};

/* Where a network device delivers each received frame (without FCS). The
 * frame is valid only during the call. */
struct frame_sink {
    void (*frame)(void *self, const uint8_t *frame, uint32_t len);
    void *self;
};

/* An interrupt controller. ack returns the pending interrupt's ID (claiming
 * it), or an ID >= IRQ_MAX_LINES when nothing is pending; eoi completes it. */
struct irq_chip_port {
    uint32_t (*ack)(void *self);
    void (*eoi)(void *self, uint32_t id);
    void (*enable)(void *self, uint32_t id);
    void *self;
};

/* A temperature sensor: milli-degrees Celsius, or a negative errno. One
 * reading, no filtering — a policy that wants an average keeps its own. */
struct thermal_port {
    int (*read_mC)(void *self, int32_t *mC);
    void *self;
};

/* The CPU clock and the regulator that feeds it, as one port: they are
 * changed together and in an order only the board knows (raise the voltage
 * before the frequency, lower it after), and splitting them would let a
 * caller get that order wrong. */
struct cpuclock_port {
    int (*set)(void *self, uint32_t khz, uint32_t mv);
    void *self;
};

#endif