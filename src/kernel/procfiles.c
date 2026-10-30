#include "kernel/procfiles.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include "arch/riscv/smp.h"
#include "arch/riscv/timer.h"
#include "arch/riscv/trap.h"
#include "core/caps.h"
#include "core/vfs.h"
#include "core/vma.h"
#include "kernel/procfs.h"
#include "kernel/sched.h"
#include "kernel/synthfs.h"
#include "kernel/uspace.h"
#include "kernel/wallclock.h"
#include "lib/kfmt.h"

/* Text being generated into a bounded buffer. Unlike adding up
 * `kfmt_snprintf`'s results, it never runs past `room`: that returns the
 * length the output *would* have had. */
struct text {
    char *out;
    uint32_t room;
    uint32_t at;
};

static void put_char(void *ctx, char c) {
    struct text *t = ctx;
    if (t->at + 1u < t->room) {
        t->out[t->at++] = c;
    }
}

static void put(struct text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(struct text *t, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)kfmt_vformat(put_char, t, fmt, ap);
    va_end(ap);
}

/* The process a /proc/self (or /proc/<pid>) file describes. */
static const struct uproc *subject(void) {
    uint32_t pid = procfs_subject();
    return pid == 0 ? uspace_current() : uspace_by_pid(pid);
}

/* Whether the caller may see p's memory layout: its own process, one of
 * its own uid, or with CAP_SYS_PTRACE — Linux's rule for maps, which gives
 * away exactly what address randomisation hides. */
static bool layout_visible(const struct uproc *p) {
    const struct uproc *me = uspace_current();
    return p == me || me == NULL || p->uid == uspace_euid() || uspace_capable(CAP_SYS_PTRACE);
}

static const char *name_of_pid(uint32_t pid) {
    const struct uproc *p = uspace_by_pid(pid);
    return p != NULL ? p->name : NULL;
}

static uint32_t uid_of_pid(uint32_t pid) {
    const struct uproc *p = uspace_by_pid(pid);
    return p != NULL ? p->uid : UINT32_MAX;
}

#define PROT_R 1u
#define PROT_W 2u
#define PROT_X 4u

/* `/proc/self/maps`: one line per region, Linux's layout. Files show no
 * device, inode or name: the region records the open file, not its path. */
static uint32_t maps_file(char *out, uint32_t room) {
    struct text t = {out, room, 0};
    const struct uproc *p = subject();
    if (p != NULL && !layout_visible(p)) {
        return 0; /* another user's layout: an empty file, as Linux gives */
    }
    for (uint32_t i = 0; p != NULL && i < p->vmas.n; i++) {
        const struct vma *v = &p->vmas.v[i];
        put(&t, "%llx-%llx %c%c%cp %08llx 00:00 0", (unsigned long long)v->start,
            (unsigned long long)v->end, (v->prot & PROT_R) != 0 ? 'r' : '-',
            (v->prot & PROT_W) != 0 ? 'w' : '-', (v->prot & PROT_X) != 0 ? 'x' : '-',
            (unsigned long long)v->offset);
        put(&t, "%s\n", (v->flags & VMA_GROWSDOWN) != 0 ? "          [stack]" : "");
    }
    return t.at;
}

/* `/proc/self/status`: the name and the identity lines. */
static uint32_t status_file(char *out, uint32_t room) {
    struct text t = {out, room, 0};
    const struct uproc *p = subject();
    if (p == NULL) {
        return 0;
    }
    put(&t, "Name:\t%s\nPid:\t%u\nPPid:\t%u\n", p->name, p->pid, p->parent_pid);
    put(&t, "Uid:\t%u\t%u\t%u\t%u\n", p->uid, p->euid, p->suid, p->euid);
    put(&t, "Gid:\t%u\t%u\t%u\t%u\n", p->gid, p->egid, p->sgid, p->egid);
    put(&t, "Threads:\t%u\n", sched_live_threads_of(p));
    return t.at;
}

static uint32_t uptime_file(char *out, uint32_t room) {
    struct text t = {out, room, 0};
    uint64_t cs = timer_ns() / 10000000ull;
    put(&t, "%llu.%02llu 0.00\n", (unsigned long long)(cs / 100u), (unsigned long long)(cs % 100u));
    return t.at;
}

/* No load average is kept, so the three averages are zero; the running and
 * total counts are real. */
static uint32_t loadavg_file(char *out, uint32_t room) {
    struct text t = {out, room, 0};
    const struct uproc *p = uspace_current();
    put(&t, "0.00 0.00 0.00 %u/%u %u\n", smp_running_count(), sched_live_threads(),
        p != NULL ? p->pid : 0u);
    return t.at;
}

static uint32_t cpuinfo_file(char *out, uint32_t room) {
    struct text t = {out, room, 0};
    uint32_t n = 0;
    for (uint32_t i = 0; i < smp_hart_count(); i++) {
        const struct smp_hart *h = smp_hart(i);
        if (h == NULL || !h->has_sv39) {
            continue; /* the S7 monitor core runs no programs */
        }
        put(&t, "processor\t: %u\nhart\t\t: %llu\nisa\t\t: rv64imafdc_zicsr_zifencei\n", n++,
            (unsigned long long)h->id);
        put(&t, "mmu\t\t: sv39\nuarch\t\t: sifive,u74-mc\n\n");
    }
    return t.at;
}

static uint32_t mounts_file(char *out, uint32_t room) {
    struct text t = {out, room, 0};
    put(&t, "ROOT / xfs rw 0 0\n");
    for (uint32_t i = 0; i < vfs_count(); i++) {
        const struct vfs_mount *m = vfs_at(i);
        put(&t, "%s %s %s rw 0 0\n", m->ops->name, m->at, m->ops->name);
    }
    return t.at;
}

static uint32_t version_file(char *out, uint32_t room) {
    struct text t = {out, room, 0};
    put(&t, "minikern version 1.0 (riscv64, rv64gc) #1 SMP\n");
    return t.at;
}

static uint32_t thp_enabled_file(char *out, uint32_t room) {
    struct text t = {out, room, 0};
    put(&t, "%s\n", uspace_transparent_huge_on() ? "[always] madvise never" : "always madvise [never]");
    return t.at;
}

static uint32_t thp_size_file(char *out, uint32_t room) {
    struct text t = {out, room, 0};
    put(&t, "2097152\n");
    return t.at;
}

/* The harts that run programs, as a range: "0-3". */
static uint32_t cpus_file(char *out, uint32_t room) {
    struct text t = {out, room, 0};
    uint32_t n = 0;
    for (uint32_t i = 0; i < smp_hart_count(); i++) {
        const struct smp_hart *h = smp_hart(i);
        n += h != NULL && h->has_sv39 ? 1u : 0u;
    }
    put(&t, n > 1u ? "0-%u\n" : "0\n", n - 1u);
    return t.at;
}

/* One hart's time in /proc/stat's ticks (1/100 s): user, system, idle. */
struct hart_ticks {
    uint64_t user, system, idle;
};

static struct hart_ticks ticks_of(uint32_t i) {
    uint64_t online = sched_hart_online_ns(i) / 10000000ull;
    uint64_t idle = sched_hart_idle_ns(i) / 10000000ull;
    uint64_t user = trap_user_ns(i) / 10000000ull;
    uint64_t busy = online > idle ? online - idle : 0;
    struct hart_ticks t = {.user = user < busy ? user : busy, .idle = idle};
    t.system = busy - t.user;
    return t;
}

/* "cpu " for the total (n < 0), "cpuN" for a hart. */
static void put_cpu_line(struct text *t, int32_t n, struct hart_ticks k) {
    if (n < 0) {
        put(t, "cpu ");
    } else {
        put(t, "cpu%u", (uint32_t)n);
    }
    put(t, " %llu 0 %llu %llu 0 0 0 0 0 0\n", (unsigned long long)k.user,
        (unsigned long long)k.system, (unsigned long long)k.idle);
}

/* The sum over every hart that runs programs (the S7 monitor does not). */
static struct hart_ticks all_ticks(uint64_t *ctxt) {
    struct hart_ticks sum = {0, 0, 0};
    for (uint32_t i = 0; i < smp_hart_count(); i++) {
        const struct smp_hart *h = smp_hart(i);
        *ctxt += sched_hart_switches(i);
        if (h != NULL && h->has_sv39) {
            struct hart_ticks k = ticks_of(i);
            sum.user += k.user;
            sum.system += k.system;
            sum.idle += k.idle;
        }
    }
    return sum;
}

/* `/proc/stat`: the total and one `cpuN` line per hart that runs programs —
 * user time from the trap path's stamps, idle from the scheduler, system
 * the rest of the time since the hart joined — then the switch and process
 * counts. */
static uint32_t stat_file(char *out, uint32_t room) {
    struct text t = {out, room, 0};
    uint64_t wall = timer_ns() / 10000000ull, ctxt = 0;
    int32_t n = 0;
    put_cpu_line(&t, -1, all_ticks(&ctxt));
    for (uint32_t i = 0; i < smp_hart_count(); i++) {
        const struct smp_hart *h = smp_hart(i);
        if (h != NULL && h->has_sv39) {
            put_cpu_line(&t, n++, ticks_of(i));
        }
    }
    put(&t, "ctxt %llu\nbtime %llu\nprocesses %u\nprocs_running %u\nprocs_blocked 0\n",
        (unsigned long long)ctxt, (unsigned long long)(wallclock_seconds() - wall / 100u),
        uspace_processes_created(), smp_running_count());
    return t.at;
}

/* `/proc/ps/...`: one value per file, a decimal number and a newline, so
 * a shell or the Go coreutils read it without parsing a format. */
static uint32_t one_value(char *out, uint32_t room, uint64_t v) {
    struct text t = {out, room, 0};
    put(&t, "%llu\n", (unsigned long long)v);
    return t.at;
}

static uint32_t ps_pid(char *out, uint32_t room) {
    const struct uproc *p = subject();
    return p != NULL ? one_value(out, room, p->pid) : 0u;
}

static uint32_t ps_ppid(char *out, uint32_t room) {
    const struct uproc *p = subject();
    return p != NULL ? one_value(out, room, p->parent_pid) : 0u;
}

static uint32_t ps_uid(char *out, uint32_t room) {
    const struct uproc *p = subject();
    return p != NULL ? one_value(out, room, p->uid) : 0u;
}

/* The program's name and a newline: `/proc/self/comm`, `/proc/ps/…/name`. */
static uint32_t ps_name(char *out, uint32_t room) {
    struct text t = {out, room, 0};
    const struct uproc *p = subject();
    if (p != NULL) {
        put(&t, "%s\n", p->name);
    }
    return t.at;
}

static uint32_t ps_memory(char *out, uint32_t room) {
    const struct uproc *p = subject();
    return p != NULL ? one_value(out, room, uspace_resident_bytes(p)) : 0u;
}

/* Whole seconds since the process was made. */
static uint32_t ps_time_alive(char *out, uint32_t room) {
    const struct uproc *p = subject();
    return p != NULL ? one_value(out, room, (timer_ns() - p->started_ns) / 1000000000ull) : 0u;
}

static uint32_t ps_niceness(char *out, uint32_t room) {
    struct text t = {out, room, 0};
    const struct uproc *p = subject();
    if (p != NULL) {
        put(&t, "%d\n", sched_nice_of(p));
    }
    return t.at;
}

static uint32_t ps_fds(char *out, uint32_t room) {
    const struct uproc *p = subject();
    return p != NULL ? one_value(out, room, uspace_open_fds(p)) : 0u;
}

/* "Z" for a process that has exited and not been reaped, "R" otherwise:
 * a process here is running or runnable until it ends. */
static uint32_t ps_state(char *out, uint32_t room) {
    struct text t = {out, room, 0};
    const struct uproc *p = subject();
    if (p != NULL) {
        put(&t, "%s\n", p->zombie ? "Z" : "R");
    }
    return t.at;
}

/* The /proc/ps files and their generators. */
static void install_ps(void) {
    (void)procfs_add("/proc/self/ps/pid", ps_pid);
    (void)procfs_add("/proc/self/ps/ppid", ps_ppid);
    (void)procfs_add("/proc/self/ps/uid", ps_uid);
    (void)procfs_add("/proc/self/ps/name", ps_name);
    (void)procfs_add("/proc/self/comm", ps_name);
    (void)procfs_add("/proc/self/ps/state", ps_state);
    (void)procfs_add("/proc/self/ps/memory_bytes", ps_memory);
    (void)procfs_add("/proc/self/ps/time_alive", ps_time_alive);
    (void)procfs_add("/proc/self/ps/niceness", ps_niceness);
    (void)procfs_add("/proc/self/ps/fds_opened", ps_fds);
}

void procfiles_install(void) {
    static const char *const devs[] = {"/dev/null", "/dev/zero", "/dev/urandom", "/dev/random",
                                       "/dev/console", "/dev/tty", "/dev/virtual/kernel_logger"};
    procfs_set_uid_of(uid_of_pid);
    procfs_set_name_of(name_of_pid);
    install_ps();
    synthfs_set_pids(uspace_pid_at);
    (void)procfs_add("/proc/stat", stat_file);
    (void)procfs_add("/proc/self/maps", maps_file);
    (void)procfs_add("/proc/self/status", status_file);
    (void)procfs_add("/proc/uptime", uptime_file);
    (void)procfs_add("/proc/loadavg", loadavg_file);
    (void)procfs_add("/proc/cpuinfo", cpuinfo_file);
    (void)procfs_add("/proc/mounts", mounts_file);
    (void)procfs_add("/proc/version", version_file);
    (void)procfs_add("/sys/kernel/mm/transparent_hugepage/enabled", thp_enabled_file);
    (void)procfs_add("/sys/kernel/mm/transparent_hugepage/hpage_pmd_size", thp_size_file);
    (void)procfs_add("/sys/devices/system/cpu/online", cpus_file);
    (void)procfs_add("/sys/devices/system/cpu/possible", cpus_file);
    for (uint32_t i = 0; i < sizeof devs / sizeof devs[0]; i++) {
        (void)synthfs_add_device(devs[i]);
    }
    (void)synthfs_mount();
}
