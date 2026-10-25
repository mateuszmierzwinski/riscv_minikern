/* irqwait — waiting for a device instead of watching it.
 *
 * **What polling actually costs here.** Every driver in this kernel waits
 * for its hardware by reading a status register in a loop, and one hart is
 * inside the kernel at a time, so a hart spinning on an SD card's
 * end-of-transfer bit is a hart that no other hart can replace: the lock is
 * held for the whole duration of the transfer. The cost is not the spinning
 * — it is that nothing else happens.
 *
 * This is the primitive that fixes it. A driver arms a wait on its
 * interrupt source, starts the transfer, and waits; the hart either sleeps
 * (if a thread owns it) or releases the kernel lock and `wfi`s until the
 * interrupt arrives. Either way the lock is free while the device works.
 *
 * **Two waiting modes, because the drivers are used twice.** The SD card is
 * read by the boot path, where there is no thread to sleep, and later
 * through the filesystem by a process, where there is. A primitive that
 * only handled one would work on the board and fail in a program, or the
 * reverse — and the failure would look like a disk problem.
 *
 * **A timeout is not optional.** An interrupt that never arrives — a
 * mis-set mask, a source number read from the wrong device-tree node, a
 * controller that needs its status cleared before it will raise again — is
 * the ordinary failure of this kind of code, and a wait without a deadline
 * turns it into a machine that hangs with no message. The timeout is what
 * makes the first attempt debuggable.
 */
#ifndef MINIKERN_KERNEL_IRQWAIT_H
#define MINIKERN_KERNEL_IRQWAIT_H

#include <stdbool.h>
#include <stdint.h>

struct irqwait {
    /* Set by the handler, read by the waiter. `volatile` because the two
     * are not the same control flow and the compiler is entitled to assume
     * a plain variable does not change inside a loop. */
    volatile uint32_t fired;
    /* What the handler saw, for a driver that needs to know *which* event
     * ended the wait — an error bit and a completion bit arrive on the same
     * source. */
    volatile uint32_t status;
    uint32_t source;
    bool armed;
};

/* **How a waiting hart sleeps**, installed rather than called.
 *
 * This module sits with the interrupt controller, below the scheduler, so
 * it cannot call `sched_wait_on` directly without inverting the layering —
 * the same problem `trap_set_lock` solves the same way. The kernel wires
 * these up once it has a scheduler; until then a wait releases the lock and
 * `wfi`s, which is all the boot path needs because it has no threads.
 *
 * `wait` returns `KERR_INVAL` when there is no thread to sleep, which is
 * how a wait knows to fall back. */
void irqwait_set_sleeper(int (*wait)(const void *chan, uint64_t deadline_ns),
                         uint32_t (*wake)(const void *chan, uint32_t max));

/* Arms `w` on a PLIC source. The handler it installs sets `fired`, records
 * what `read_status` returns, and wakes anything sleeping on `w`.
 *
 * `read_status` may be NULL, for a device whose interrupt carries no
 * information beyond having happened. It is called **in the handler**, so
 * it must do nothing but read — and in particular must not take the kernel
 * lock, which the handler already holds. */
int irqwait_arm(struct irqwait *w, uint32_t source, uint32_t (*read_status)(void));

/* Waits for the interrupt, or `KERR_TIMEDOUT`.
 *
 * Returns immediately if it has already fired, which is the race this is
 * built around: the device may finish between the driver starting it and
 * the driver waiting, and a wait that slept anyway would wait the whole
 * timeout for an interrupt that already came. */
int irqwait_wait(struct irqwait *w, uint64_t timeout_ns);

/* Forgets the firing, ready for the next transfer. Separate from `arm`
 * because arming registers a handler and a driver does one transfer after
 * another on the same source. */
void irqwait_rearm(struct irqwait *w);

/* How many waits ended in an interrupt and how many in a timeout.
 *
 * Reported because the difference is the whole question about this code: a
 * driver that has silently fallen back to its timeout on every transfer is
 * slower than the polling it replaced, and looks identical from outside. */
void irqwait_counts(uint64_t *woken, uint64_t *timed_out);

#endif /* MINIKERN_KERNEL_IRQWAIT_H */
