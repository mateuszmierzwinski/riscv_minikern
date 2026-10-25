#include "kernel/irqwait.h"

#include "arch/riscv/timer.h"
#include "core/console.h"
#include "core/errno.h"
#include "kernel/irq.h"
#include "kernel/klock.h"

/* The scheduler's sleep and wake, given rather than called: see
 * `irqwait_set_sleeper`. NULL until the kernel has a scheduler, which is
 * correct for the boot path and is why a wait has a `wfi` fallback. */
static int (*sleeper_wait)(const void *chan, uint64_t deadline_ns);
static uint32_t (*sleeper_wake)(const void *chan, uint32_t max);

void irqwait_set_sleeper(int (*wait)(const void *chan, uint64_t deadline_ns),
                         uint32_t (*wake)(const void *chan, uint32_t max)) {
    sleeper_wait = wait;
    sleeper_wake = wake;
}

static uint64_t woken_count;
static uint64_t timeout_count;

/* The handler's own view of a wait. The status reader is kept here rather
 * than in the public struct because nothing outside needs to call it, and a
 * function pointer a driver can reach is a function pointer a driver will
 * call at the wrong time. */
struct armed {
    struct irqwait *w;
    uint32_t (*read_status)(void);
};

/* One slot per source this kernel actually uses. A fixed table rather than
 * an allocation: arming happens at probe, the count is small and known, and
 * a handler that outlived its driver would be an interrupt into freed
 * memory. */
#define IRQWAIT_SLOTS 8u
static struct armed slots[IRQWAIT_SLOTS];
static uint32_t slots_used;

static void on_interrupt(void *self) {
    struct armed *a = self;
    if (a->read_status != NULL) {
        a->w->status = a->read_status();
    }
    a->w->fired = 1u;
    /* **Woken by channel, and the channel is the wait itself.** Two drivers
     * waiting at once are two addresses, so a completion cannot wake the
     * wrong one — which with a shared channel would show up as a transfer
     * returning somebody else's data. */
    if (sleeper_wake != NULL) {
        (void)sleeper_wake(a->w, 1u);
    }
}

int irqwait_arm(struct irqwait *w, uint32_t source, uint32_t (*read_status)(void)) {
    struct irq_handler h;
    int rc;
    if (w == NULL || source == 0u) {
        return KERR_INVAL;
    }
    if (slots_used == IRQWAIT_SLOTS) {
        return KERR_NOMEM;
    }
    slots[slots_used] = (struct armed){.w = w, .read_status = read_status};
    h.handle = on_interrupt;
    h.self = &slots[slots_used];
    rc = irq_register(source, &h);
    if (rc != KERR_OK) {
        return rc;
    }
    slots_used++;
    w->source = source;
    w->fired = 0;
    w->status = 0;
    w->armed = true;
    return KERR_OK;
}

void irqwait_rearm(struct irqwait *w) {
    if (w != NULL) {
        w->fired = 0;
        w->status = 0;
    }
}

static bool said_which_way;
static bool said_what_the_sleeper_did;
static bool said_how_the_spin_ended;

int irqwait_wait(struct irqwait *w, uint64_t timeout_ns) {
    uint64_t deadline;
    if (w == NULL || !w->armed) {
        return KERR_INVAL;
    }
    /* **Checked before waiting at all**, because the device may have
     * finished between being started and being waited for. A wait that
     * slept regardless would spend the whole timeout waiting for an
     * interrupt that had already arrived — and would look like hardware
     * that never answers. */
    if (w->fired != 0u) {
        woken_count++;
        return KERR_OK;
    }
    deadline = timer_ns() + timeout_ns;
    /* **One boot's worth of self-report, because this does not terminate.**
     *
     * A command whose interrupt never arrives must be reported inside the
     * timeout, and it is not: a caller's own report after this function
     * never prints. Every branch below ends either in a return or in a
     * bounded spin, so the fault is in which branch is taken — and that
     * cannot be read off the source, because it depends on whether the
     * caller is a thread. Guarded to one line so it cannot flood. */
    if (!said_which_way) {
        said_which_way = true;
        kprintf("irqwait: waiting, sleeper %s, deadline in %llu ms\n",
                sleeper_wait != NULL ? "installed" : "absent",
                (unsigned long long)(timeout_ns / 1000000ull));
    }
    while (w->fired == 0u) {
        if (timer_ns() >= deadline) {
            timeout_count++;
            return KERR_TIMEDOUT;
        }
        /* **Sleep if a thread owns this hart; otherwise watch the flag with
         * the lock let go.**
         *
         * Not `wfi` in the second case, which is what the first version did
         * and why the board stopped at the first transfer: `wfi` returns
         * when *some* interrupt arrives, and the deadline above is only
         * tested between waits — so a `wfi` waiting for an interrupt that
         * never comes is a hang the timeout cannot catch. During boot there
         * is no scheduler tick to rescue it, either.
         *
         * Spinning with the lock released is not a power win, and is not
         * claimed as one. It is a *concurrency* win, which is what the
         * polling it replaces costs: one hart is inside the kernel at a
         * time, so a driver that spins with the lock held stops every other
         * hart for the length of a transfer. Interrupts stay on so the
         * handler can set the flag. */
        if (sleeper_wait != NULL) {
            int slept = sleeper_wait(w, deadline);
            if (!said_what_the_sleeper_did) {
                said_what_the_sleeper_did = true;
                kprintf("irqwait: the sleeper returned %d\n", slept);
            }
            if (slept != KERR_INVAL) {
                continue;
            }
        }
        {
            bool held = klock_held();
            klock_exit(held);
            __asm__ volatile("csrsi sstatus, 2" : : : "memory");
            while (w->fired == 0u && timer_ns() < deadline) {
                /* Nothing: the handler is what ends this. */
            }
            if (!said_how_the_spin_ended) {
                said_how_the_spin_ended = true;
                kprintf("irqwait: the spin ended %s\n",
                        w->fired != 0u ? "on the handler" : "on the deadline");
            }
            __asm__ volatile("csrci sstatus, 2" : : : "memory");
            if (held) {
                (void)klock_enter();
            }
        }
    }
    woken_count++;
    return KERR_OK;
}

void irqwait_counts(uint64_t *woken, uint64_t *timed_out) {
    *woken = woken_count;
    *timed_out = timeout_count;
}
