/* The syscalls that change what a process is allowed to do: `capget`,
 * `capset`, `seccomp`, and the parts of `prctl` that belong with them.
 *
 * Kept in their own file because they are the only calls whose *purpose* is
 * to reduce privilege, and because each of them has to be careful in the
 * same particular way: a mistake here is not a wrong answer but a privilege
 * that was not actually dropped, which is invisible until it matters.
 */
#include "core/caps.h"
#include "core/console.h"
#include "core/errno.h"
#include "kernel/mem.h"
#include "kernel/seccomp.h"
#include "kernel/syscall.h"
#include "kernel/uspace.h"

#include <stddef.h>

/* `prctl` operations this kernel implements. The rest return `EINVAL`
 * rather than zero: a program that asks for something and is told "fine"
 * will act as though it got it, which for a privilege operation means
 * acting as though it is confined when it is not. */
#define PR_SET_SECCOMP       22
#define PR_GET_SECCOMP       21
#define PR_SET_NO_NEW_PRIVS  38
#define PR_GET_NO_NEW_PRIVS  39
#define PR_CAPBSET_READ      23
#define PR_CAPBSET_DROP      24

/* The `capget`/`capset` ABI. Version 3 is the current one and carries two
 * 32-bit words per set, because capabilities outgrew one word long ago. */
#define LINUX_CAPABILITY_VERSION_3 0x20080522u
#define LINUX_CAPABILITY_U32S_3    2u

struct cap_header {
    uint32_t version;
    int32_t pid;
};

/* Three sets, low word then high word — the layout the ABI fixes. */
struct cap_data {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
};

/* Whether a process may install a syscall filter.
 *
 * **`no_new_privs` or `CAP_SYS_ADMIN`, and the reason is not obvious.**
 * Installing a filter is a way to make a chosen syscall fail, and a setuid
 * program that relied on a syscall failing could be steered into granting
 * privilege it should not. Requiring `no_new_privs` first removes the
 * setuid program from the picture: an exec can no longer raise privilege at
 * all, so there is nothing left to confuse. A process holding
 * `CAP_SYS_ADMIN` is already trusted with more than this. */
static bool may_install_filter(const struct uproc *p) {
    return seccomp_no_new_privs(p) || uspace_capable(CAP_SYS_ADMIN);
}

/* Reads a filter program out of user memory and installs it.
 *
 * The program is copied in **before** it is verified and verified before it
 * is stored, which is the only safe order: verifying it in user memory
 * would let the process rewrite it between the check and the run. */
static int64_t install_filter(virt_t user_fprog, bool log) {
    /* `struct sock_fprog`: a count and a pointer. */
    struct {
        uint16_t len;
        uint16_t pad[3];
        uint64_t filter;
    } fprog;
    struct bpf_insn *prog;
    const char *why = "";
    struct uproc *me = uspace_current();
    uint32_t count;
    int rc;
    if (uspace_copy_in(&fprog, user_fprog, sizeof fprog) != KERR_OK) {
        return KERR_FAULT;
    }
    count = fprog.len;
    if (count == 0u || count > BPF_MAX_INSNS) {
        return KERR_INVAL;
    }
    prog = kzalloc(count * (uint32_t)sizeof *prog);
    if (prog == NULL) {
        return KERR_NOMEM;
    }
    if (uspace_copy_in(prog, (virt_t)fprog.filter, count * (uint32_t)sizeof *prog) != KERR_OK) {
        kfree(prog);
        return KERR_FAULT;
    }
    if (!bpf_filter_valid(prog, count, &why)) {
        /* **Named, not merely refused.** A filter is generated code and the
         * author cannot see it; "EINVAL" sends them to read this file,
         * while the reason tells them which instruction to look at. */
        kprintf("seccomp: pid %u offered a filter with %s\n", uspace_pid_of(uspace_current()), why);
        kfree(prog);
        return KERR_INVAL;
    }
    rc = seccomp_add_filter(me, prog, count, log);
    kfree(prog); /* `seccomp_add_filter` keeps its own copy */
    return rc;
}

static int64_t sys_seccomp(struct trapframe *tf) {
    uint64_t op = sysarg(tf, 0), flags = sysarg(tf, 1), arg = sysarg(tf, 2);
    struct uproc *me = uspace_current();
    if (me == NULL) {
        return KERR_INVAL;
    }
    switch (op) {
    case SECCOMP_SET_MODE_STRICT:
        if (flags != 0u || arg != 0u) {
            return KERR_INVAL;
        }
        return seccomp_set_strict(me);
    case SECCOMP_SET_MODE_FILTER:
        /* `TSYNC` asks for the filter to be applied to every thread of the
         * process. This kernel's filters live on the process, so every
         * thread is already covered — the flag is accepted because
         * refusing it would make a program that asked for *more* safety
         * fail, and what it asks for is already true. */
        if ((flags & ~(uint64_t)(SECCOMP_FILTER_FLAG_TSYNC | SECCOMP_FILTER_FLAG_LOG |
                                 SECCOMP_FILTER_FLAG_SPEC_ALLOW)) != 0u) {
            return KERR_INVAL;
        }
        if (!may_install_filter(me)) {
            return KERR_PERM;
        }
        return install_filter((virt_t)arg, (flags & SECCOMP_FILTER_FLAG_LOG) != 0u);
    case SECCOMP_GET_ACTION_AVAIL: {
        uint32_t action = 0;
        if (uspace_copy_in(&action, (virt_t)arg, sizeof action) != KERR_OK) {
            return KERR_FAULT;
        }
        /* Only the actions this kernel actually performs. Claiming one it
         * ignores would make a sandbox believe a rule was being applied. */
        switch (action & SECCOMP_RET_ACTION_FULL) {
        case SECCOMP_RET_KILL_PROCESS:
        case SECCOMP_RET_KILL_THREAD:
        case SECCOMP_RET_ERRNO:
        case SECCOMP_RET_TRAP:
        case SECCOMP_RET_LOG:
        case SECCOMP_RET_ALLOW:
            return KERR_OK;
        default:
            return KERR_NOTSUP;
        }
    }
    default:
        return KERR_INVAL;
    }
}

/* `prctl`, for the operations that are about privilege.
 *
 * Everything else returns `EINVAL`. The call used to return zero for
 * anything, which is the shape of answer that makes a program believe it
 * has been confined — the worst possible reply to a request about
 * privilege. */
static int64_t sys_prctl(struct trapframe *tf) {
    uint64_t op = sysarg(tf, 0), a = sysarg(tf, 1);
    struct uproc *me = uspace_current();
    if (me == NULL) {
        return KERR_INVAL;
    }
    switch (op) {
    case PR_SET_NO_NEW_PRIVS:
        if (a != 1u) {
            return KERR_INVAL; /* it cannot be turned off */
        }
        seccomp_set_no_new_privs(me);
        return KERR_OK;
    case PR_GET_NO_NEW_PRIVS:
        return seccomp_no_new_privs(me) ? 1 : 0;
    case PR_GET_SECCOMP:
        return (int64_t)me->seccomp.mode;
    case PR_SET_SECCOMP:
        if (a == SECCOMP_MODE_STRICT) {
            return seccomp_set_strict(me);
        }
        if (a == SECCOMP_MODE_FILTER) {
            if (!may_install_filter(me)) {
                return KERR_PERM;
            }
            return install_filter((virt_t)sysarg(tf, 2), false);
        }
        return KERR_INVAL;
    case PR_CAPBSET_READ:
        if (!cap_valid((uint32_t)a)) {
            return KERR_INVAL;
        }
        return cap_in(me->caps.bounding, (uint32_t)a) ? 1 : 0;
    case PR_CAPBSET_DROP:
        if (!cap_valid((uint32_t)a)) {
            return KERR_INVAL;
        }
        if (!uspace_capable(CAP_SETPCAP)) {
            return KERR_PERM;
        }
        /* **Irreversible, and that is the whole value.** Dropping a
         * capability from the bounding set means no descendant can ever
         * regain it, which is what turns "we do not use this" into "we
         * cannot use this". */
        me->caps.bounding &= ~cap_bit((uint32_t)a);
        me->caps.permitted &= me->caps.bounding;
        me->caps.effective &= me->caps.permitted;
        me->caps.inheritable &= me->caps.bounding;
        me->caps.ambient &= me->caps.permitted & me->caps.inheritable;
        return KERR_OK;
    default:
        return KERR_INVAL;
    }
}

/* Splits a 64-bit set into the ABI's two words, and back. */
static void sets_out(struct cap_data *out, const struct cap_sets *s) {
    out[0].effective = (uint32_t)s->effective;
    out[0].permitted = (uint32_t)s->permitted;
    out[0].inheritable = (uint32_t)s->inheritable;
    out[1].effective = (uint32_t)(s->effective >> 32);
    out[1].permitted = (uint32_t)(s->permitted >> 32);
    out[1].inheritable = (uint32_t)(s->inheritable >> 32);
}

static void sets_in(struct cap_sets *s, const struct cap_data *in) {
    s->effective = (cap_mask)in[0].effective | ((cap_mask)in[1].effective << 32);
    s->permitted = (cap_mask)in[0].permitted | ((cap_mask)in[1].permitted << 32);
    s->inheritable = (cap_mask)in[0].inheritable | ((cap_mask)in[1].inheritable << 32);
}

static int64_t sys_capget(struct trapframe *tf) {
    struct cap_header hdr;
    struct cap_data data[LINUX_CAPABILITY_U32S_3] = {{0, 0, 0}, {0, 0, 0}};
    struct uproc *me = uspace_current();
    if (me == NULL || uspace_copy_in(&hdr, (virt_t)sysarg(tf, 0), sizeof hdr) != KERR_OK) {
        return KERR_FAULT;
    }
    if (hdr.version != LINUX_CAPABILITY_VERSION_3) {
        /* **The version is written back on refusal**, which is the ABI's
         * handshake: a library asks with the version it prefers and reads
         * back the one the kernel speaks. Refusing without it would leave
         * the library no way to find out. */
        hdr.version = LINUX_CAPABILITY_VERSION_3;
        (void)uspace_copy_out((virt_t)sysarg(tf, 0), &hdr, sizeof hdr);
        return KERR_INVAL;
    }
    /* Only this process. Reading another's sets needs `CAP_SYS_PTRACE` and
     * a reason; neither is here, so asking is refused rather than answered
     * about the wrong process. */
    if (hdr.pid != 0 && (uint32_t)hdr.pid != uspace_pid_of(uspace_current())) {
        return KERR_PERM;
    }
    sets_out(data, &me->caps);
    if (sysarg(tf, 1) == 0u) {
        return KERR_OK; /* a query for the version alone */
    }
    return uspace_copy_out((virt_t)sysarg(tf, 1), data, sizeof data) == KERR_OK ? KERR_OK
                                                                               : KERR_FAULT;
}

static int64_t sys_capset(struct trapframe *tf) {
    struct cap_header hdr;
    struct cap_data data[LINUX_CAPABILITY_U32S_3];
    struct cap_sets want;
    struct uproc *me = uspace_current();
    if (me == NULL || uspace_copy_in(&hdr, (virt_t)sysarg(tf, 0), sizeof hdr) != KERR_OK) {
        return KERR_FAULT;
    }
    if (hdr.version != LINUX_CAPABILITY_VERSION_3) {
        hdr.version = LINUX_CAPABILITY_VERSION_3;
        (void)uspace_copy_out((virt_t)sysarg(tf, 0), &hdr, sizeof hdr);
        return KERR_INVAL;
    }
    if (hdr.pid != 0 && (uint32_t)hdr.pid != uspace_pid_of(uspace_current())) {
        return KERR_PERM; /* changing another process's is not permitted */
    }
    if (uspace_copy_in(data, (virt_t)sysarg(tf, 1), sizeof data) != KERR_OK) {
        return KERR_FAULT;
    }
    want = me->caps; /* bounding and ambient are not in the ABI's three sets */
    sets_in(&want, data);
    /* **The policy decides, in one place.** Every rule about what may
     * change lives in `cap_change_allowed`, so this call cannot disagree
     * with the one in `prctl` about what dropping privilege means. */
    if (!cap_change_allowed(&me->caps, &want, uspace_capable(CAP_SETPCAP))) {
        return KERR_PERM;
    }
    me->caps.permitted = want.permitted;
    me->caps.effective = want.effective;
    me->caps.inheritable = want.inheritable;
    me->caps.ambient &= me->caps.permitted & me->caps.inheritable;
    return KERR_OK;
}

const struct syscall_entry sys_priv_table[] = {
    {SYS_CAPGET, "capget", sys_capget},
    {SYS_CAPSET, "capset", sys_capset},
    {SYS_PRCTL, "prctl", sys_prctl},
    {SYS_SECCOMP, "seccomp", sys_seccomp},
    {0, NULL, NULL},
};
