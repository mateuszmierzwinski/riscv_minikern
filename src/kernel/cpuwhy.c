#include "kernel/cpuwhy.h"

#include <stdbool.h>
#include <stddef.h>

#include "arch/riscv/smp.h"
#include "arch/riscv/timer.h"
#include "arch/riscv/trap.h"
#include "core/console.h"
#include "kernel/klock.h"
#include "kernel/sched.h"
#include "kernel/syscall.h"
#include "kernel/uspace.h"

#define REPORT_EVERY_NS 10000000000ull /* 10 s */
#define PIDS 32u                       /* the pids a report names */
#define TOP_SYSCALLS 8u

/* The previous report's readings: a report is the difference. */
static struct {
    uint64_t at;
    uint64_t online[SMP_MAX_HARTS], idle[SMP_MAX_HARTS], user[SMP_MAX_HARTS];
    uint64_t held, waited, takes;
    uint64_t pid_user[PIDS];
    uint64_t calls[SYSCALL_COUNTED];
    uint64_t traps[2][TRAP_CAUSES];
} last;

static uint64_t pid_user_ns[PIDS];
static uint32_t (*cycle_khz)(void);

/* The user-time sink: charged to the process on this hart, before the lock
 * is taken, so atomically. */
static void charge_user_time(uint64_t ns) {
    const struct uproc *p = uspace_current();
    uint32_t pid = p != NULL && p->pid < PIDS ? p->pid : 0u;
    __atomic_add_fetch(&pid_user_ns[pid], ns, __ATOMIC_RELAXED);
}

static uint64_t delta(uint64_t now, uint64_t *was) {
    uint64_t d = now - *was;
    *was = now;
    return d;
}

static uint32_t per_mille(uint64_t part, uint64_t whole) {
    return whole == 0 ? 0u : (uint32_t)(part * 1000u / whole);
}

/* One hart's split of the interval: user, kernel (the rest), idle. */
static void report_hart(uint32_t i) {
    uint64_t online = delta(sched_hart_online_ns(i), &last.online[i]);
    uint64_t idle = delta(sched_hart_idle_ns(i), &last.idle[i]);
    uint64_t user = delta(trap_user_ns(i), &last.user[i]);
    uint64_t kernel = online > idle + user ? online - idle - user : 0u;
    uint32_t u = per_mille(user, online), k = per_mille(kernel, online);
    uint32_t z = per_mille(idle, online);
    kprintf("cpuwhy: hart %u user %u.%u%% kernel %u.%u%% idle %u.%u%%\n", i, u / 10u, u % 10u,
            k / 10u, k % 10u, z / 10u, z % 10u);
}

/* Cycles as microseconds, or as thousands of cycles ("kc") when the rate is
 * not known — this boot's frequency driver may not have started. */
static void put_cycles(const char *what, uint64_t cycles) {
    uint32_t khz = cycle_khz != NULL ? cycle_khz() : 0u;
    if (khz == 0) {
        kprintf(" %s %llu kc", what, (unsigned long long)(cycles / 1000u));
    } else {
        kprintf(" %s %llu us", what, (unsigned long long)(cycles * 1000u / khz));
    }
}

/* The kernel lock: how long it was held in all, and how long harts waited. */
static void report_lock(void) {
    uint64_t takes, waited_n, wait_cycles, worst, held, worst_held;
    klock_contention(&takes, &waited_n, &wait_cycles, &worst);
    klock_held_times(&held, &worst_held);
    (void)waited_n;
    (void)worst;
    (void)worst_held;
    kprintf("cpuwhy: lock taken %llu times;", (unsigned long long)delta(takes, &last.takes));
    put_cycles("held", delta(held, &last.held));
    put_cycles("waited for", delta(wait_cycles, &last.waited));
    kprintf("\n");
}

static void report_pids(void) {
    kprintf("cpuwhy: user time by pid (us):");
    for (uint32_t pid = 0; pid < PIDS; pid++) {
        uint64_t d = delta(__atomic_load_n(&pid_user_ns[pid], __ATOMIC_RELAXED), &last.pid_user[pid]);
        if (d != 0) {
            kprintf(" %u=%llu", pid, (unsigned long long)(d / 1000u));
        }
    }
    kprintf("\n");
}

/* The busiest number not yet printed, or SYSCALL_COUNTED for none. */
static uint32_t busiest(const uint64_t *d, const bool *said) {
    uint32_t best = SYSCALL_COUNTED;
    for (uint32_t nr = 0; nr < SYSCALL_COUNTED; nr++) {
        if (!said[nr] && d[nr] != 0 && (best == SYSCALL_COUNTED || d[nr] > d[best])) {
            best = nr;
        }
    }
    return best;
}

static uint64_t calls_now[SYSCALL_COUNTED];
static bool said_now[SYSCALL_COUNTED];

static void report_syscalls(void) {
    uint64_t all = 0;
    for (uint32_t nr = 0; nr < SYSCALL_COUNTED; nr++) {
        calls_now[nr] = delta(syscall_calls_of(nr), &last.calls[nr]);
        said_now[nr] = false;
        all += calls_now[nr];
    }
    kprintf("cpuwhy: %llu syscalls:", (unsigned long long)all);
    for (uint32_t i = 0; i < TOP_SYSCALLS; i++) {
        uint32_t nr = busiest(calls_now, said_now);
        if (nr == SYSCALL_COUNTED) {
            break;
        }
        said_now[nr] = true;
        kprintf(" %s %llu", syscall_name(nr), (unsigned long long)calls_now[nr]);
    }
    kprintf("\n");
}

static void report_traps(void) {
    kprintf("cpuwhy: traps:");
    for (uint32_t half = 0; half < 2u; half++) {
        for (uint32_t code = 0; code < TRAP_CAUSES; code++) {
            uint64_t d = delta(trap_cause_count(half == 1u, code), &last.traps[half][code]);
            if (d != 0) {
                kprintf(" %s%u=%llu", half == 1u ? "irq" : "exc", code, (unsigned long long)d);
            }
        }
    }
    kprintf("\n");
}

/* The beat's hook: a report once the interval has passed. */
static void report_from_beat(void) {
    uint64_t now = timer_ns();
    if (now - last.at < REPORT_EVERY_NS) {
        return;
    }
    kprintf("cpuwhy: the last %llu ms\n", (unsigned long long)((now - last.at) / 1000000u));
    last.at = now;
    for (uint32_t i = 0; i < smp_hart_count(); i++) {
        const struct smp_hart *h = smp_hart(i);
        if (h != NULL && h->has_sv39) {
            report_hart(i);
        }
    }
    report_lock();
    report_pids();
    report_syscalls();
    report_traps();
}

void cpuwhy_install(uint32_t (*khz)(void)) {
    cycle_khz = khz;
    last.at = timer_ns();
    trap_set_user_time_sink(charge_user_time);
    (void)timer_add_housekeeping_hook(report_from_beat);
}
