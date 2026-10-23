/* klock — one lock around the kernel.
 *
 * It preserves exactly the property the single-hart design relied on — one
 * hart inside the kernel at a time — on four harts, without auditing every
 * structure first. The parallelism this buys is in **user space**, which is
 * where a program spends its time: four harts running user code, serialised
 * only while they are in a syscall.
 *
 * The protocol, which is the whole of it:
 *
 *   - A hart takes the lock when it **enters** the kernel from user mode,
 *     and releases it when it returns there.
 *   - An idle hart holds it only while it looks for work.
 *   - A thread switch **hands the lock on**: ownership is a hart's, and a
 *     switch does not change harts, so the resuming thread legitimately
 *     holds what the suspending one held.
 *
 * Interrupts are off throughout the kernel (the hardware clears `sstatus.SIE`
 * on a trap), so there is no nesting to worry about — except on the one path
 * that enables them deliberately, an idle hart's `wfi`, which is why
 * `klock_enter` reports whether it actually took the lock rather than
 * assuming.
 *
 * Finer locks come only where measurement asks for them. A kernel lock is
 * the right first answer because it is the one that cannot be subtly wrong.
 */
#ifndef MINIKERN_KERNEL_KLOCK_H
#define MINIKERN_KERNEL_KLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* Takes the lock unless this hart already holds it. Reports whether it was
 * taken, which is what the caller must pass back to `klock_exit`. */
bool klock_enter(void);

/* Releases it, if `took` says this call owns it. */
void klock_exit(bool took);

/* Whether this hart holds it, for an assertion on a path that must be
 * inside the kernel's critical section. */
bool klock_held(void);

/* The lock's internals, for a diagnostic: the next ticket, the one being
 * served, and who holds it. A lock whose two numbers have diverged is a
 * machine that will stop, and the numbers say which side lost count. */
void klock_state(uint32_t *next, uint32_t *serving, uint64_t *owner);

/* Releases the lock, waits for an interrupt with interrupts enabled, and
 * takes it again. The only place in the kernel where interrupts are on, and
 * the reason `klock_enter` has to be re-entrant-aware: the interrupt that
 * wakes the hart arrives while it does not hold the lock. */
void klock_idle_wait(void);

/* Times a hart won the lock while another hart was still recorded as its
 * owner. Anything but zero means the one lock this kernel relies on is not
 * exclusive, and every "the lock prevents that" argument is void. */
uint32_t klock_breaches(void);

#endif
