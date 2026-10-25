/* A ticket lock: the one synchronisation primitive.
 *
 * Ticket rather than test-and-set because it is **fair**. A test-and-set
 * lock lets the hart with the warmest cache win repeatedly, and a hart that
 * loses often enough is starved — which on four cores shows up as one core
 * doing nothing while three fight. A ticket lock serves harts in the order
 * they arrived, and the arrival order is the only thing it needs to be
 * correct about.
 *
 * `amoadd` and `amoswap` are in the base ISA's A extension, which `rv64gc`
 * includes, so there is no load-reserved loop to write. The memory ordering
 * is explicit: **acquire** on the way in (nothing after the lock may be
 * observed before it) and **release** on the way out (everything before the
 * unlock is visible to whoever takes it next). Getting those backwards
 * produces a lock that works on an emulator and fails on silicon, which is
 * the worst possible outcome.
 */
#ifndef MINIKERN_ARCH_RISCV_SPINLOCK_H
#define MINIKERN_ARCH_RISCV_SPINLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "arch/riscv/cache.h"

/* The two counters are on **separate cache lines**, which matters on four
 * cores and costs 128 bytes.
 *
 * `next` is written by every hart that arrives wanting the lock. `serving` is
 * what every waiting hart spins on, reading it over and over. In one line,
 * each arrival invalidates the line every waiter is spinning on — the two
 * fields have opposite access patterns, which is the textbook case for
 * splitting them. (Not measurable under an emulator that models no caches;
 * see `cache.h`.) */
struct spinlock {
    uint32_t next CACHE_ALIGNED;    /* the next ticket to hand out */
    uint32_t serving CACHE_ALIGNED; /* the ticket being served */
};

#define SPINLOCK_INIT {0, 0}

static inline void spin_lock(struct spinlock *l) {
    uint32_t ticket;
    __asm__ volatile("amoadd.w.aq %0, %2, (%1)"
                     : "=r"(ticket)
                     : "r"(&l->next), "r"(1u)
                     : "memory");
    while (__atomic_load_n(&l->serving, __ATOMIC_ACQUIRE) != ticket) {
        /* Nothing but waiting. No `wfi` here: the wake-up would have to come
         * from the unlocking hart, and an IPI per unlock costs more than the
         * spin it saves on a critical section this short. */
    }
}

/* Takes the lock and says how many cycles were spent waiting.
 *
 * **The ticket answers the question.** On a ticket lock there is no way to
 * ask "would I wait?" without taking a ticket, and once taken the answer is
 * free: if the lock is already serving our number we never waited, and the
 * clock is not read at all. That keeps the uncontended path — nearly all of
 * it — at one extra load over `spin_lock`, which matters because this sits
 * on the path it measures. */
static inline void spin_lock_counting(struct spinlock *l, uint64_t *waited_cycles) {
    uint32_t ticket;
    *waited_cycles = 0;
    __asm__ volatile("amoadd.w.aq %0, %2, (%1)"
                     : "=r"(ticket)
                     : "r"(&l->next), "r"(1u)
                     : "memory");
    if (__atomic_load_n(&l->serving, __ATOMIC_ACQUIRE) == ticket) {
        return; /* ours already: no wait, and no clock read */
    }
    {
        uint64_t began;
        __asm__ volatile("rdcycle %0" : "=r"(began));
        while (__atomic_load_n(&l->serving, __ATOMIC_ACQUIRE) != ticket) {
            /* As `spin_lock`: nothing but waiting. */
        }
        {
            uint64_t ended;
            __asm__ volatile("rdcycle %0" : "=r"(ended));
            *waited_cycles = ended - began;
        }
    }
}

static inline void spin_unlock(struct spinlock *l) {
    __atomic_store_n(&l->serving, l->serving + 1u, __ATOMIC_RELEASE);
}

/* Tries once. For a path that has something else to do rather than wait —
 * an idle hart with no thread to run gains nothing by queueing. */
static inline bool spin_try_lock(struct spinlock *l) {
    uint32_t serving = __atomic_load_n(&l->serving, __ATOMIC_RELAXED);
    uint32_t expected = serving;
    return __atomic_compare_exchange_n(&l->next, &expected, serving + 1u, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

#endif
