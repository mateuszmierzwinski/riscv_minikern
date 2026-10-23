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

/* Harts that were inside the kernel while another hart owned the lock.
 *
 * **The number every other theory rests on.** "Two harts cannot both be in
 * that function, because the lock prevents it" is the argument that
 * eliminated most of the §0 candidates, and nothing had ever checked the
 * lock itself — the existing counter beside it in `trap.c` counts trap
 * *frames*, which nest on one hart, so its reading of seven on four harts
 * says nothing either way.
 *
 * There are two ways the lock could fail to be exclusive, and this catches
 * both. The spinlock could hand two harts the same ticket, and
 * `klock_held` could answer **yes** on a hart that owns nothing — which it
 * decides by comparing the owner against `smp_this_hart`, so a hart that
 * misidentifies itself skips the lock entirely and returns `false` for
 * "somebody else took it", which the trap path reads as "I already hold
 * it". Checking at the moment of acquisition covers the pair: having just
 * won the spinlock, nobody else may be recorded as the owner. */
static uint32_t lock_breaches;

uint32_t klock_breaches(void) {
    return __atomic_load_n(&lock_breaches, __ATOMIC_RELAXED);
}

bool klock_enter(void) {
    uint64_t was;
    if (klock_held()) {
        return false;
    }
    spin_lock(&the_lock);
    was = __atomic_exchange_n(&owner_plus_one, smp_this_hart() + 1u, __ATOMIC_ACQ_REL);
    if (was != 0u) {
        __atomic_add_fetch(&lock_breaches, 1u, __ATOMIC_RELAXED);
    }
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
