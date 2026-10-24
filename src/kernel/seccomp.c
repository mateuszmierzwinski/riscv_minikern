#include "kernel/seccomp.h"

#include "core/console.h"
#include "core/errno.h"
#include "kernel/mem.h"
#include "kernel/signal.h"
#include "kernel/syscall.h"
#include "kernel/uspace.h"

/* The four calls strict mode leaves: enough to read input, write output,
 * return from a signal handler and die. Anything else kills the process.
 *
 * `exit_group` is included where Linux includes only `exit`, because a
 * threaded program's exit path goes through it and a strict-mode process
 * that could not exit would have to be killed to do so — which is the same
 * outcome by a worse route. The difference is recorded rather than hidden
 * because it is a divergence from the ABI. */
static bool strict_allows(uint64_t nr) {
    return nr == SYS_READ || nr == SYS_WRITE || nr == SYS_EXIT || nr == SYS_EXIT_GROUP ||
           nr == SYS_RT_SIGRETURN;
}

/* Fills the structure a filter reads. The layout is the ABI's, and the
 * fields are written at their fixed offsets rather than through a struct so
 * that the interpreter's verified offsets and these agree by construction. */
static void fill_data(uint8_t *data, uint64_t nr, const uint64_t *args, uint64_t pc) {
    static const uint32_t ARG_OFF[6] = {16u, 24u, 32u, 40u, 48u, 56u};
    for (uint32_t i = 0; i < BPF_SECCOMP_DATA_BYTES; i++) {
        data[i] = 0;
    }
    for (uint32_t i = 0; i < 4u; i++) {
        data[BPF_SECCOMP_OFF_NR + i] = (uint8_t)(nr >> (8u * i));
        data[BPF_SECCOMP_OFF_ARCH + i] = (uint8_t)(SECCOMP_AUDIT_ARCH_RISCV64 >> (8u * i));
    }
    for (uint32_t i = 0; i < 8u; i++) {
        data[BPF_SECCOMP_OFF_IP + i] = (uint8_t)(pc >> (8u * i));
    }
    for (uint32_t a = 0; a < 6u; a++) {
        for (uint32_t i = 0; i < 8u; i++) {
            data[ARG_OFF[a] + i] = (uint8_t)(args[a] >> (8u * i));
        }
    }
}

bool seccomp_no_new_privs(const struct uproc *p) {
    return p != NULL && p->seccomp.no_new_privs;
}

void seccomp_set_no_new_privs(struct uproc *p) {
    if (p != NULL) {
        /* One-way, like the mode: a process that could clear this could
         * clear the reason it was allowed to install a filter. */
        p->seccomp.no_new_privs = true;
    }
}

int seccomp_set_strict(struct uproc *p) {
    if (p == NULL) {
        return KERR_INVAL;
    }
    if (p->seccomp.mode == SECCOMP_MODE_FILTER) {
        /* Strict is not stricter than a filter in any way this kernel can
         * reason about — it permits four calls, and a filter may permit
         * fewer. Refusing rather than replacing keeps "a filter is never
         * loosened" true without having to compare two things that are not
         * comparable. */
        return KERR_INVAL;
    }
    p->seccomp.mode = SECCOMP_MODE_STRICT;
    return KERR_OK;
}

int seccomp_add_filter(struct uproc *p, const struct bpf_insn *prog, uint32_t count, bool log) {
    struct bpf_insn *copy;
    if (p == NULL || prog == NULL || count == 0u) {
        return KERR_INVAL;
    }
    if (p->seccomp.mode == SECCOMP_MODE_STRICT) {
        return KERR_INVAL; /* strict mode is already stricter than this */
    }
    if (p->seccomp.count >= SECCOMP_MAX_FILTERS) {
        return KERR_NOMEM;
    }
    copy = kzalloc(count * (uint32_t)sizeof *copy);
    if (copy == NULL) {
        return KERR_NOMEM;
    }
    for (uint32_t i = 0; i < count; i++) {
        copy[i] = prog[i];
    }
    p->seccomp.prog[p->seccomp.count] = copy;
    p->seccomp.insns[p->seccomp.count] = count;
    p->seccomp.count++;
    p->seccomp.mode = SECCOMP_MODE_FILTER;
    p->seccomp.log_all = p->seccomp.log_all || log;
    return KERR_OK;
}

uint32_t seccomp_decide(const struct uproc *p, uint64_t nr, const uint64_t *args, uint64_t pc) {
    uint8_t data[BPF_SECCOMP_DATA_BYTES];
    uint32_t worst = SECCOMP_RET_ALLOW;
    if (p == NULL || p->seccomp.mode == SECCOMP_MODE_DISABLED) {
        return SECCOMP_RET_ALLOW;
    }
    if (p->seccomp.mode == SECCOMP_MODE_STRICT) {
        return strict_allows(nr) ? SECCOMP_RET_ALLOW : SECCOMP_RET_KILL_PROCESS;
    }
    fill_data(data, nr, args, pc);
    for (uint32_t i = 0; i < p->seccomp.count; i++) {
        uint32_t got = bpf_filter_run(p->seccomp.prog[i], p->seccomp.insns[i], data);
        /* **The minimum, and the comparison is unsigned on the whole
         * word.** `KILL_PROCESS` is `0x80000000`, which as a signed value
         * is the most negative number — so a signed comparison would make
         * the strictest action look like the loosest, and a filter that
         * asked to kill would be ignored in favour of one that allowed. */
        worst = got < worst ? got : worst;
    }
    return worst;
}

bool seccomp_inherit(struct uproc *child, const struct uproc *parent) {
    if (child == NULL || parent == NULL) {
        return false;
    }
    child->seccomp.mode = parent->seccomp.mode;
    child->seccomp.no_new_privs = parent->seccomp.no_new_privs;
    child->seccomp.log_all = parent->seccomp.log_all;
    child->seccomp.count = 0;
    for (uint32_t i = 0; i < parent->seccomp.count; i++) {
        uint32_t n = parent->seccomp.insns[i];
        struct bpf_insn *copy = kzalloc(n * (uint32_t)sizeof *copy);
        if (copy == NULL) {
            /* **The child must not run unconfined.** Freeing what was
             * copied and failing is the only safe answer: a child of a
             * sandboxed parent that escaped the sandbox because memory was
             * short is precisely the hole this feature exists to close. */
            seccomp_release(child);
            return false;
        }
        for (uint32_t k = 0; k < n; k++) {
            copy[k] = parent->seccomp.prog[i][k];
        }
        child->seccomp.prog[i] = copy;
        child->seccomp.insns[i] = n;
        child->seccomp.count++;
    }
    return true;
}

void seccomp_release(struct uproc *p) {
    if (p == NULL) {
        return;
    }
    for (uint32_t i = 0; i < p->seccomp.count; i++) {
        kfree(p->seccomp.prog[i]);
        p->seccomp.prog[i] = NULL;
        p->seccomp.insns[i] = 0;
    }
    p->seccomp.count = 0;
    /* **The mode is not cleared.** This runs when a process is torn down,
     * and it also runs on the failure path of `seccomp_inherit` — where
     * leaving the mode set is what keeps a half-built child from being
     * reported as unconfined. A process being freed has its whole `uproc`
     * zeroed by its owner afterwards. */
}

uint32_t seccomp_action(const struct trapframe *tf, uint64_t nr) {
    const struct uproc *me = uspace_current();
    uint64_t args[6];
    if (me == NULL || me->seccomp.mode == SECCOMP_MODE_DISABLED) {
        return SECCOMP_RET_ALLOW; /* the common case, and one comparison */
    }
    for (uint32_t i = 0; i < 6u; i++) {
        args[i] = sysarg(tf, i);
    }
    return seccomp_decide(me, nr, args, tf->sepc);
}

bool seccomp_apply(struct trapframe *tf, uint64_t nr, uint32_t action) {
    uint32_t what = action & SECCOMP_RET_ACTION_FULL;
    if (what == SECCOMP_RET_ALLOW) {
        return false;
    }
    if (what == SECCOMP_RET_LOG) {
        /* **The only action that lets the call through.** It exists so a
         * sandbox can be developed by watching what a program actually
         * asks for, which means the program has to keep working. */
        kprintf("seccomp: pid %u called %llu (logged)\n", uspace_pid_of(uspace_current()),
                (unsigned long long)nr);
        return false;
    }
    if (what == SECCOMP_RET_ERRNO) {
        /* The low sixteen bits are the errno to report, and the ABI has
         * them as a positive number while this kernel returns negatives. */
        uint32_t err = action & SECCOMP_RET_DATA;
        tf->x[10] = (uint64_t)(int64_t)-(int64_t)(err > 4095u ? 4095u : err);
        return true;
    }
    if (what == SECCOMP_RET_TRAP) {
        /* `SIGSYS`, which a program may handle — that is the difference
         * between `TRAP` and `KILL`, and the reason a runtime can emulate a
         * syscall the filter refuses. */
        kprintf("seccomp: pid %u trapped on call %llu\n", uspace_pid_of(uspace_current()),
                (unsigned long long)nr);
        signal_raise_fault(SIGSYS, 0, 0);
        return true;
    }
    /* `KILL_THREAD` and `KILL_PROCESS`. This kernel's `uspace_exit` ends
     * the process either way, and the difference is recorded rather than
     * pretended away: a threaded program asking for one thread to die would
     * lose all of them. */
    kprintf("seccomp: pid %u killed for call %llu\n", uspace_pid_of(uspace_current()),
            (unsigned long long)nr);
    uspace_exit(-(int64_t)SIGSYS);
    return true;
}
