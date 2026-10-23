/* sys_proc — running a program from a program.
 *
 * What this is for: Go's `os/exec`. It does not use `fork` — it uses
 * **`vfork`** semantics, `clone(CLONE_VM|CLONE_VFORK)`, where the child
 * borrows its parent's address space and the parent is suspended until the
 * child execs or exits. That is deliberate on the runtime's part and
 * convenient here: there is no address space to copy, and no copy-on-write
 * to implement, because the parent cannot run while the child holds it.
 *
 * The sequence a runtime actually performs, which is what these implement:
 *
 *   1. `pipe2` — a pipe the child uses to report that `execve` failed.
 *   2. `clone(CLONE_VM|CLONE_VFORK|SIGCHLD)` — the child.
 *   3. in the child: `dup3` the descriptors into place, then `execve`.
 *   4. in the parent: the clone returns once the child has execed, then
 *      `wait4` collects its status.
 *
 * Step 4 is the part that is easy to get subtly wrong: the parent must not
 * resume until the child has stopped using its address space, or it will run
 * with a stack the child has been writing to.
 */
#include <stddef.h>

#include "core/console.h"
#include "core/errno.h"
#include "kernel/file.h"
#include "kernel/sched.h"
#include "kernel/signal.h"
#include "kernel/syscall.h"
#include "kernel/uspace.h"
#include "kernel/xfsmount.h"

/* `clone`'s flags, of which these are the ones that change what happens. */
#define CLONE_VM 0x00000100u
#define CLONE_VFORK 0x00004000u
#define CLONE_THREAD 0x00010000u

/* The largest program this will load, and the largest argument list. */
#define EXEC_MAX_BYTES (8u * 1024u * 1024u)
#define EXEC_MAX_ARGS 32u
#define EXEC_MAX_PATH 128u

/* `wait4`'s options. */
#define WNOHANG 1u

/* Copies a NUL-terminated string out of the process, refusing one that is
 * longer than the buffer rather than truncating it: a truncated path names a
 * different file, and a truncated argument is a different argument. */
static int copy_string_in(char *to, uint32_t cap, virt_t from) {
    for (uint32_t i = 0; i < cap; i++) {
        if (uspace_copy_in(&to[i], from + i, 1) != KERR_OK) {
            return KERR_FAULT;
        }
        if (to[i] == '\0') {
            return KERR_OK;
        }
    }
    return KERR_NAMETOOLONG;
}

/* Copies a NULL-terminated vector of string pointers, and the strings.
 *
 * Both halves are bounded: a caller supplies the count by where it puts its
 * NULL, so an unbounded loop here is a program's way of making the kernel
 * read for ever. */
static int copy_vector_in(virt_t vector, char storage[][EXEC_MAX_PATH], const char **out,
                          uint32_t *count) {
    *count = 0;
    if (vector == 0) {
        return KERR_OK;
    }
    for (uint32_t i = 0; i < EXEC_MAX_ARGS - 1u; i++) {
        uint64_t ptr = 0;
        int rc = uspace_copy_in(&ptr, vector + i * 8u, 8);
        if (rc != KERR_OK) {
            return rc;
        }
        if (ptr == 0) {
            *count = i;
            return KERR_OK;
        }
        rc = copy_string_in(storage[i], EXEC_MAX_PATH, ptr);
        if (rc != KERR_OK) {
            return rc;
        }
        out[i] = storage[i];
    }
    return KERR_2BIG;
}

/* `execve`: replace this process's program with another.
 *
 * The order matters and each step is a decision:
 *
 *   - **The file is read before anything is destroyed.** A failing `execve`
 *     must leave the process exactly as it was — a program that handles the
 *     failure is entitled to carry on — and that is impossible once the
 *     address space is gone.
 *   - **The other threads of the process end first.** They were running code
 *     that is about to disappear.
 *   - The descriptor table survives, minus close-on-exec; the signal
 *     handlers do not, because they pointed into code that is no longer
 *     there.
 *
 * Runs to 29 lines: the sequence is one transaction — read, then dismantle,
 * then rebuild — and splitting it would leave a fragment that cannot be
 * named honestly, since the middle of a replacement is not a state anything
 * else may see (Rule 11, Naming).
 */
/* The argument list is copied into **static** storage, not a stack frame:
 * thirty-two arguments of up to 128 bytes is four kilobytes, which the
 * frame-size guard refuses. One copy is enough because an `execve` holds the
 * kernel lock from beginning to end and never sleeps. */
static char exec_strings[EXEC_MAX_ARGS][EXEC_MAX_PATH];
static const char *exec_argv[EXEC_MAX_ARGS];

static int64_t sys_execve(struct trapframe *tf) {
    char path[EXEC_MAX_PATH];
    char (*storage)[EXEC_MAX_PATH] = exec_strings;
    const char **argv = exec_argv;
    uint32_t argc = 0;
    struct uproc *me = uspace_current();
    uint8_t *image = NULL;
    uint32_t size = 0;
    int rc = copy_string_in(path, sizeof path, sysarg(tf, 0));
    if (rc != KERR_OK || me == NULL) {
        return rc != KERR_OK ? rc : KERR_SRCH;
    }
    for (uint32_t i = 0; i < EXEC_MAX_ARGS; i++) {
        argv[i] = NULL;
    }
    rc = copy_vector_in(sysarg(tf, 1), storage, argv, &argc);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfsmount_read_file(path, EXEC_MAX_BYTES, &image, &size);
    if (rc != KERR_OK) {
        /* Traced, because a failing `execve` is reported to the caller as one
         * number and the caller usually turns it into "exec failed": without
         * this the kernel's own reason is invisible. */
        if (syscall_tracing_sleeps()) {
            kprintf("exec: cannot read %s (rc %d)\n", path, rc);
        }
        return rc;
    }
    (void)sched_end_siblings(me);
    rc = uspace_exec(me, image, size, path, argv, argc);
    kfree_image(image);
    if (rc != KERR_OK) {
        if (syscall_tracing_sleeps()) {
            kprintf("exec: %s will not load (rc %d)\n", path, rc);
        }
        return rc;
    }
    /* `execve` does not return: the thread restarts at the new program's
     * entry point with a new stack, which `uspace_exec` has written into the
     * frame. */
    uspace_restart_thread(tf, me);
    return 0;
}

/* `clone`: a thread, a **fork**, or a vfork child.
 *
 * A thread (`CLONE_THREAD`) shares everything and is what the scheduler's
 * `clone` already did.
 *
 * Without `CLONE_VM` it is a real `fork`: the child gets an address space of
 * its own, made of the parent's pages copy-on-write, and the two are
 * **isolated** from the moment the call returns. Neither can reach the
 * other's memory whatever it does, and nothing is copied until one of them
 * writes — so a process that forks and immediately execs copies page tables
 * and nothing else.
 *
 * With `CLONE_VM` and `CLONE_VFORK` it is still a vfork child, which borrows
 * its parent's address space while the parent is suspended. That remains
 * because it is what a runtime asks for when it wants to exec something
 * cheaply, and it is genuinely cheaper than a fork even now.
 */
static int64_t sys_clone(struct trapframe *tf) {
    uint64_t flags = sysarg(tf, 0), stack = sysarg(tf, 1);
    struct uproc *parent = uspace_current();
    struct uproc *child;
    struct trapframe child_frame = *tf;
    uint64_t tid = 0;
    int rc;
    if (parent == NULL) {
        return KERR_SRCH;
    }
    if ((flags & CLONE_THREAD) != 0) {
        return syscall_clone_thread(tf);
    }
    child = uspace_alloc();
    if (child == NULL) {
        return KERR_AGAIN;
    }
    /* The one decision that matters: a copied address space, or a borrowed
     * one. */
    rc = (flags & CLONE_VM) == 0 ? uspace_fork(child, parent)
                                 : uspace_fork_shared(child, parent);
    if (rc != KERR_OK) {
        uspace_release(child);
        return rc;
    }
    /* The child resumes after the `ecall` with 0 in `a0`, which is how it
     * knows it is the child. Its stack is the one the caller gave, or its
     * parent's if none was. */
    child_frame.sepc = tf->sepc + 4u;
    child_frame.x[10] = 0;
    /* A forked child continues on **its own copy of the parent's stack**, at
     * the same address — that is the whole point of copying the address
     * space, and giving it a different stack pointer would put it somewhere
     * its own copy does not have a frame. */
    rc = sched_spawn_in(child, &child_frame, stack != 0 ? stack : tf->x[2], &tid);
    if (rc != KERR_OK) {
        uspace_release(child);
        return rc;
    }
    child->main_tid = tid;
    /* **`CLONE_VFORK`: the parent waits.** Not as a nicety — the child is
     * using the parent's address space, including its stack, so a parent
     * that carried on would be writing where the child is. It resumes when
     * the child execs (and so has its own) or exits. */
    if ((flags & CLONE_VFORK) != 0) {
        uspace_wait_for_vfork(child);
    }
    return (int64_t)child->pid;
}

/* ── process groups and sessions ───────────────────────────────────────────
 *
 * What a shell needs to interrupt a pipeline rather than one of its members.
 * A process's group and session are inherited at `fork` and default to its
 * own pid, which makes the first process a group and session leader with no
 * special case anywhere.
 *
 * `setpgid(0, 0)` means "me, into a group of my own", which is the form a
 * shell uses for each job it starts; the general form is restricted to the
 * caller and its children, as POSIX requires — a process may not move an
 * unrelated one between groups, because that would let it redirect
 * somebody else's signals.
 */
static int64_t sys_setpgid(struct trapframe *tf) {
    uint32_t pid = (uint32_t)sysarg(tf, 0);
    uint32_t pgid = (uint32_t)sysarg(tf, 1);
    struct uproc *me = uspace_current();
    struct uproc *target;
    if (me == NULL) {
        return KERR_SRCH;
    }
    target = pid == 0 ? me : uspace_by_pid(pid);
    if (target == NULL) {
        return KERR_SRCH;
    }
    /* The caller or one of its children, and nothing else. */
    if (target != me && target->parent_pid != me->pid) {
        return KERR_PERM;
    }
    /* A session leader cannot leave its own session, which is what keeps a
     * session's members from being scattered across groups it does not own. */
    if (target->sid == target->pid) {
        return KERR_PERM;
    }
    target->pgid = pgid == 0 ? target->pid : pgid;
    return 0;
}

static int64_t sys_getpgid(struct trapframe *tf) {
    uint32_t pid = (uint32_t)sysarg(tf, 0);
    struct uproc *me = uspace_current();
    struct uproc *target = pid == 0 ? me : uspace_by_pid(pid);
    return target == NULL ? KERR_SRCH : (int64_t)target->pgid;
}

static int64_t sys_getsid(struct trapframe *tf) {
    uint32_t pid = (uint32_t)sysarg(tf, 0);
    struct uproc *me = uspace_current();
    struct uproc *target = pid == 0 ? me : uspace_by_pid(pid);
    return target == NULL ? KERR_SRCH : (int64_t)target->sid;
}

/* `setsid`: a new session, with the caller leading it and a new group of its
 * own. Refused for a process that already leads a group, which POSIX
 * requires and which exists so that a session's id cannot collide with a
 * group id already in use. */
static int64_t sys_setsid(struct trapframe *tf) {
    struct uproc *me = uspace_current();
    (void)tf;
    if (me == NULL) {
        return KERR_SRCH;
    }
    if (me->pgid == me->pid) {
        return KERR_PERM;
    }
    me->sid = me->pid;
    me->pgid = me->pid;
    return (int64_t)me->sid;
}

/* `kill`: a signal to a process, or to a whole group.
 *
 * `pid > 0` is one process, `pid < 0` is the group `-pid`, and `pid == 0`
 * is the caller's own group — the three forms a shell uses. The group forms
 * are the point of this step: a job-control signal has to reach every member
 * of a pipeline, and reaching one of them is worse than reaching none,
 * because the survivors keep running with their stdin gone.
 *
 * `SIGSTOP` and `SIGCONT` act on the process rather than being delivered to
 * a handler, so they go to the scheduler. The rest are raised on the
 * process's first thread: this kernel has no notion of a process-wide
 * pending set, so "the process" means the thread that will notice first,
 * which is right for the signals a shell sends and would not be for a
 * multi-threaded program catching them. Stated rather than hidden. */
static void deliver_to_one(struct uproc *p, uint32_t signo) {
    if (signo == SIGSTOP) {
        (void)sched_stop_process(uspace_pid_of(p));
        return;
    }
    if (signo == SIGCONT) {
        (void)sched_continue_process(uspace_pid_of(p));
        return;
    }
    sched_raise_on_process(uspace_pid_of(p), signo);
}

static void deliver_in_group(struct uproc *p, uint32_t signo) {
    deliver_to_one(p, signo);
}

static int64_t sys_kill(struct trapframe *tf) {
    int64_t pid = (int64_t)sysarg(tf, 0);
    uint32_t signo = (uint32_t)sysarg(tf, 1);
    struct uproc *me = uspace_current();
    if (signo >= SIG_COUNT) {
        return KERR_INVAL;
    }
    if (pid > 0) {
        struct uproc *target = uspace_by_pid((uint32_t)pid);
        if (target == NULL || target->zombie) {
            return KERR_SRCH;
        }
        if (signo != 0) {
            deliver_to_one(target, signo);
        }
        return 0;
    }
    {
        uint32_t pgid = pid == 0 ? (me == NULL ? 0u : me->pgid) : (uint32_t)(-pid);
        uint32_t n = signo == 0 ? uspace_for_each_in_group(pgid, NULL, 0)
                                : uspace_for_each_in_group(pgid, deliver_in_group, signo);
        /* A group with no members is `ESRCH`, the same as a pid with no
         * process: "nobody got it" is the fact the caller needs either way. */
        return n == 0 ? KERR_SRCH : 0;
    }
}

/* `wait4`: collect a finished child's status.
 *
 * Only what a runtime uses: a specific child or any child, with or without
 * `WNOHANG`, and the status written back in the shape `WEXITSTATUS` expects
 * — the exit code in bits 8..15, which is the detail that makes a correct
 * kernel look wrong if it is missed, because the caller then reads a status
 * 256 times too small. */
static int64_t sys_wait4(struct trapframe *tf) {
    int64_t want = (int64_t)sysarg(tf, 0);
    virt_t status_at = sysarg(tf, 1);
    uint64_t options = sysarg(tf, 2);
    struct uproc *me = uspace_current();
    struct uproc *child;
    uint32_t pid;
    uint32_t rounds = 0;
    int32_t encoded;
    if (me == NULL) {
        return KERR_SRCH;
    }
    for (;;) {
        child = want > 0 ? uspace_by_pid((uint32_t)want) : uspace_reapable_child(me->pid);
        if (child != NULL && want > 0 && (child->parent_pid != me->pid || !child->zombie)) {
            child = child->parent_pid == me->pid ? NULL : child;
            if (child != NULL) {
                return KERR_CHILD; /* not ours */
            }
        }
        if (child != NULL && child->zombie) {
            break;
        }
        if ((options & WNOHANG) != 0) {
            return 0; /* nothing finished, and the caller does not want to wait */
        }
        if (uspace_count_children(me->pid) == 0) {
            return KERR_CHILD;
        }
        if (rounds < 4u && syscall_tracing_errors()) {
            struct uproc *c = uspace_by_pid((uint32_t)(want > 0 ? want : 0));
            kprintf("wait4: pid %u waiting; child %u used %u zombie %u parent %u; ticks %u "
                    "deadline-wakes %u\n",
                    me->pid, want > 0 ? (uint32_t)want : 0u, c != NULL && c->used ? 1u : 0u,
                    c != NULL && c->zombie ? 1u : 0u, c != NULL ? c->parent_pid : 0u,
                    sched_ticks_seen(), sched_deadline_wakes());
        }
        rounds++;
        uspace_wait_for_child(me);
    }
    pid = child->pid;
    if (syscall_tracing_errors()) {
        kprintf("wait4: pid %u reaping child %u after %u rounds\n", me->pid, pid, rounds);
    }
    /* The exit code goes in bits 8..15, which is what `WEXITSTATUS` shifts
     * back out. A status written in the low byte reads as 0 for every
     * program that exits with anything under 256. */
    encoded = (int32_t)((child->status & 0xff) << 8);
    if (status_at != 0 && uspace_copy_out(status_at, &encoded, 4) != KERR_OK) {
        return KERR_FAULT;
    }
    uspace_destroy(child);
    uspace_release(child);
    return (int64_t)pid;
}

static int64_t sys_getppid(struct trapframe *tf) {
    struct uproc *me = uspace_current();
    (void)tf;
    return me == NULL ? 0 : (int64_t)me->parent_pid;
}

const struct syscall_entry sys_proc_table[] = {
    {220, "clone", sys_clone},
    {221, "execve", sys_execve},
    {260, "wait4", sys_wait4},
    {129, "kill", sys_kill},
    {154, "setpgid", sys_setpgid},
    {155, "getpgid", sys_getpgid},
    {156, "getsid", sys_getsid},
    {157, "setsid", sys_setsid},
    {173, "getppid", sys_getppid},
    {0, NULL, NULL},
};
