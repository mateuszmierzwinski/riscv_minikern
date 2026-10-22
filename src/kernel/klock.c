#include "kernel/klock.h"

#include "arch/riscv/smp.h"
#include "arch/riscv/spinlock.h"

static struct spinlock the_lock = SPINLOCK_INIT;

/* Which hart holds it, plus one so that 0 can mean "nobody" — hart 0 is a
 * real hart id here (it is the JH7110's monitor core, and on QEMU it is an
 * ordinary one), so a sentinel of 0 would be a hart's own id. */
static volatile uint64_t owner_plus_one;

bool klock_held(void) {
    return __atomic_load_n(&owner_plus_one, __ATOMIC_ACQUIRE) == smp_this_hart() + 1u;
}

bool klock_enter(void) {
    if (klock_held()) {
        return false;
    }
    spin_lock(&the_lock);
    __atomic_store_n(&owner_plus_one, smp_this_hart() + 1u, __ATOMIC_RELEASE);
    return true;
}

void klock_exit(bool took) {
    if (!took) {
        return;
    }
    __atomic_store_n(&owner_plus_one, 0u, __ATOMIC_RELEASE);
    spin_unlock(&the_lock);
}

void klock_state(uint32_t *next, uint32_t *serving, uint64_t *owner) {
    *next = __atomic_load_n(&the_lock.next, __ATOMIC_RELAXED);
    *serving = __atomic_load_n(&the_lock.serving, __ATOMIC_RELAXED);
    *owner = __atomic_load_n(&owner_plus_one, __ATOMIC_RELAXED);
}

void klock_idle_wait(void) {
    bool held = klock_held();
    klock_exit(held);
    /* Interrupts on only here, and only around the wait itself. The tick is
     * what wakes this: a hart with nothing to do must still take its timer
     * interrupt, or it would sleep through the moment work appeared. */
    __asm__ volatile("csrsi sstatus, 2\n\twfi\n\tcsrci sstatus, 2" : : : "memory");
    if (held) {
        (void)klock_enter();
    }
}
