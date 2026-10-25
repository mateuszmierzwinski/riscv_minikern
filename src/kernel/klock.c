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

/* **What the lock actually costs**, which its roadmap entry asks for before
 * anybody splits it: "should wait until something is actually measured to
 * be blocked on it".
 *
 * Three numbers, and the third is the one that decides. Acquisitions say
 * how syscall-heavy the workload was. *Contended* acquisitions — ones that
 * had to wait at all — say how often four harts wanted the kernel at once.
 * And the cycles spent waiting say whether that mattered: a million
 * contended acquisitions that each waited twenty cycles is a lock nobody is
 * blocked on, and ten that each waited a millisecond is a lock that is the
 * whole problem. Counting only the first two would make every busy kernel
 * look contended.
 *
 * Relaxed atomics, and the cycle read is `rdcycle` on either side of the
 * wait: this is on the path it measures, so it has to cost almost nothing.
 * The numbers are per-machine rather than per-hart, which is the right
 * grain for "is the lock the ceiling" and the wrong one for "which hart is
 * starved" — that would need per-hart arrays and is not what is being
 * asked. */
static uint64_t held_cycles;
static uint64_t worst_held;
static uint64_t held_since[SMP_MAX_HARTS];
/* **Who held it that long.** The return address of whoever took the lock,
 * kept only for the worst hold so far — because "some critical section runs
 * for milliseconds" is a hypothesis and an address is a fact. Resolved with
 * `nm` against the image, which is how the long one gets a name instead of a
 * plausible story. */
static uint64_t worst_held_caller;
static void *taken_by[SMP_MAX_HARTS];
/* **What the holder was doing.** The caller's return address turned out to be
 * `trap_handle` for nearly every acquisition — the lock is taken at trap
 * entry, so the address says "a trap" and nothing more. The tag is set by
 * whoever knows: the trap cause, and for a syscall its number. That is the
 * difference between "some critical section is long" and knowing which. */
static uint64_t tag_now[SMP_MAX_HARTS];
static uint64_t worst_held_tag;

static uint64_t acquisitions;
static uint64_t contended;
static uint64_t wait_cycles;
static uint64_t worst_wait;

void klock_contention(uint64_t *acquires, uint64_t *waited, uint64_t *cycles, uint64_t *worst) {
    *acquires = __atomic_load_n(&acquisitions, __ATOMIC_RELAXED);
    *waited = __atomic_load_n(&contended, __ATOMIC_RELAXED);
    *cycles = __atomic_load_n(&wait_cycles, __ATOMIC_RELAXED);
    *worst = __atomic_load_n(&worst_wait, __ATOMIC_RELAXED);
}

bool klock_enter(void) {
    uint64_t was;
    uint64_t began = 0; /* cycles waited, which `spin_lock_counting` fills */
    if (klock_held()) {
        return false;
    }
    __atomic_add_fetch(&acquisitions, 1u, __ATOMIC_RELAXED);
    /* **Asked before waiting**, so that an uncontended acquisition pays one
     * load and no clock read at all — which is the case that has to stay
     * cheap, because it is nearly all of them. */
    spin_lock_counting(&the_lock, &began);
    if (began != 0u) {
        __atomic_add_fetch(&contended, 1u, __ATOMIC_RELAXED);
        __atomic_add_fetch(&wait_cycles, began, __ATOMIC_RELAXED);
        if (began > __atomic_load_n(&worst_wait, __ATOMIC_RELAXED)) {
            __atomic_store_n(&worst_wait, began, __ATOMIC_RELAXED);
        }
    }
    {
        uint32_t me = smp_hart_index();
        if (me < SMP_MAX_HARTS) {
            __asm__ volatile("rdcycle %0" : "=r"(held_since[me]));
            taken_by[me] = __builtin_return_address(0);
        }
    }
    was = __atomic_exchange_n(&owner_plus_one, smp_this_hart() + 1u, __ATOMIC_ACQ_REL);
    if (was != 0u) {
        __atomic_add_fetch(&lock_breaches, 1u, __ATOMIC_RELAXED);
    }
    return true;
}

/* **How long the lock is held**, which is the other half of the question.
 *
 * Wait time says the lock is contended; hold time says *why*. A hart cannot
 * wait longer than somebody else held, so a worst wait of several
 * milliseconds is not evidence about syscall throughput at all — it is
 * evidence that some critical section runs for milliseconds, and on this
 * kernel the candidates are the polled ones: a disk transfer, a console
 * write to a 115200-baud UART. Splitting the lock would not shorten them.
 *
 * Kept per machine and sampled on release, so an uncontended acquisition
 * pays one clock read on each side. */
void klock_held_times(uint64_t *total, uint64_t *worst) {
    *total = __atomic_load_n(&held_cycles, __ATOMIC_RELAXED);
    *worst = __atomic_load_n(&worst_held, __ATOMIC_RELAXED);
}

uint64_t klock_worst_caller(void) {
    return __atomic_load_n(&worst_held_caller, __ATOMIC_RELAXED);
}

void klock_tag(uint64_t what) {
    uint32_t me = smp_hart_index();
    if (me < SMP_MAX_HARTS) {
        __atomic_store_n(&tag_now[me], what, __ATOMIC_RELAXED);
    }
}

uint64_t klock_worst_tag(void) {
    return __atomic_load_n(&worst_held_tag, __ATOMIC_RELAXED);
}

void klock_exit(bool took) {
    if (!took) {
        return;
    }
    {
        uint32_t me = smp_hart_index();
        uint64_t since = me < SMP_MAX_HARTS ? held_since[me] : 0u;
        if (since != 0u) {
            uint64_t now;
            __asm__ volatile("rdcycle %0" : "=r"(now));
            {
                uint64_t took_cycles = now - since;
                __atomic_add_fetch(&held_cycles, took_cycles, __ATOMIC_RELAXED);
                if (took_cycles > __atomic_load_n(&worst_held, __ATOMIC_RELAXED)) {
                    __atomic_store_n(&worst_held, took_cycles, __ATOMIC_RELAXED);
                    __atomic_store_n(&worst_held_caller,
                                     (uint64_t)(uintptr_t)(me < SMP_MAX_HARTS ? taken_by[me] : 0),
                                     __ATOMIC_RELAXED);
                    __atomic_store_n(&worst_held_tag,
                                     me < SMP_MAX_HARTS ? tag_now[me] : 0u, __ATOMIC_RELAXED);
                }
            }
        }
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
