#include "kernel/signal.h"

#include <stddef.h>

#include "arch/riscv/fp.h"
#include "core/console.h"
#include "arch/riscv/sigframe.h"
#include "core/errno.h"
#include "kernel/sched.h"
#include "kernel/uspace.h"

/* The **process's** dispositions: which handler is installed for which
 * signal. That much is correctly process-wide — a handler is installed for
 * the program, not for a thread.
 *
 * Everything that belongs to a thread lives in the thread (`struct
 * thread_signals`): the mask, the pending set and the alternate stack. That
 * division is not cosmetic. A shared mask lets one thread unblock a signal
 * inside another thread's critical section; a shared alternate stack lets
 * two threads deliver handlers onto the same memory and clobber each other's
 * saved registers. The second one produced a Go program panicking with
 * "WaitGroup misuse" on code that misuses nothing — once in 36 runs, found
 * by a stress loop and by nothing else.
 *
 * A named struct because resetting it needs a type to name: `typeof` is not
 * C17, which this is built as. */
/* What is left here is a counter and a switch. The **dispositions live in
 * the process** (`struct uproc::actions`), because two processes must not
 * share handlers — a child that execs resets them all to default, and a
 * shared table would reset its parent's too. */
struct signal_state {
    uint64_t delivered;
};

static struct signal_state sig;

/* The running process's dispositions, or NULL outside a process. */
static struct user_sigaction *actions(void) {
    struct uproc *p = uspace_current();
    return p == NULL ? NULL : p->actions;
}

/* Not a secret, just a value that is not plausible as anything else. */
#define SIGFRAME_COOKIE 0x5349474652414d45ull /* "SIGFRAME" */

static uint64_t clobbered_frames;

/* The calling thread's signal state. Every path here runs on behalf of a
 * thread; a NULL means something asked about signals outside a process. */
static struct thread_signals *mine(void) {
    return sched_current_signals();
}

/* Delivery can be turned off from the command line (`minikern.nosignals`).
 *
 * It is a bisecting tool, kept because it earned its place: when a Go
 * program started failing its own internal consistency checks under load,
 * the only way to find out whether signal delivery was involved was to take
 * it away and run the same stress loop again. A kernel with several new
 * subsystems needs a way to subtract one. */
static bool delivery_off;

void signal_set_delivery(bool on) {
    delivery_off = !on;
}

/* `li a7, 139; ecall` — `rt_sigreturn`, as two instructions.
 *
 * Written out as bytes rather than assembled from a label, because this is
 * **copied into the process's address space**: it has to be position
 * independent and it has to be exactly these instructions, and a reader
 * should be able to see both facts without going to another file.
 *
 *   08b00893   addi a7, zero, 139
 *   00000073   ecall
 */
static const uint8_t trampoline[] = {0x93, 0x08, 0xb0, 0x08, 0x73, 0x00, 0x00, 0x00};

const uint8_t *signal_trampoline_code(uint32_t *bytes) {
    *bytes = (uint32_t)sizeof trampoline;
    return trampoline;
}

static bool valid_signal(uint32_t signo) {
    return signo >= 1u && signo < SIG_COUNT;
}

static uint64_t bit_of(uint32_t signo) {
    return 1ull << (signo - 1u);
}

void signal_reset(void) {
    struct user_sigaction *a = actions();
    sig.delivered = 0;
    /* Every handler back to default, which is what a process starting — or
     * one that has just execed — must see. The ABI says an exec keeps the
     * mask and the pending set but resets every *handler*, because the
     * handlers belonged to code that is no longer there. */
    for (uint32_t i = 0; a != NULL && i < SIG_COUNT; i++) {
        a[i] = (struct user_sigaction){0};
    }
}

int signal_action(uint32_t signo, const struct user_sigaction *in, struct user_sigaction *out) {
    struct user_sigaction *a = actions();
    if (!valid_signal(signo) || a == NULL) {
        return KERR_INVAL;
    }
    if (out != NULL) {
        *out = a[signo];
    }
    if (in != NULL) {
        /* SIGKILL cannot be caught or ignored. A kernel that allowed it
         * would have no way left to stop a process. */
        if (signo == SIGKILL) {
            return KERR_INVAL;
        }
        a[signo] = *in;
    }
    return KERR_OK;
}

int signal_mask(uint32_t how, const uint64_t *set, uint64_t *old) {
    struct thread_signals *me = mine();
    uint64_t was;
    if (me == NULL) {
        return KERR_SRCH;
    }
    was = me->blocked;
    if (old != NULL) {
        *old = was;
    }
    if (set == NULL) {
        return KERR_OK;
    }
    switch (how) {
    case SIG_BLOCK:
        me->blocked = was | *set;
        break;
    case SIG_UNBLOCK:
        me->blocked = was & ~*set;
        break;
    case SIG_SETMASK:
        me->blocked = *set;
        break;
    default:
        return KERR_INVAL;
    }
    /* SIGKILL is never blocked, whatever was asked. */
    me->blocked &= ~bit_of(SIGKILL);
    return KERR_OK;
}

/* Whether the interrupted code was already running on the alternate stack —
 * which it is whenever a signal arrives inside a handler. */
static bool already_on_altstack(const struct thread_signals *me, virt_t sp) {
    return me->altstack_sp != 0 && sp >= me->altstack_sp &&
           sp < me->altstack_sp + me->altstack_size;
}

int signal_altstack_at(virt_t current_sp, const struct user_sigaltstack *in,
                       struct user_sigaltstack *out) {
    struct thread_signals *me = mine();
    if (me == NULL) {
        return KERR_SRCH;
    }
    if (out != NULL) {
        out->ss_sp = me->altstack_sp;
        out->ss_size = me->altstack_size;
        out->ss_flags = me->altstack_flags;
        out->pad = 0;
    }
    /* Changing the alternate stack while running on it is refused, because
     * the frame that will be returned through is on it. */
    if (in != NULL && already_on_altstack(me, current_sp)) {
        return KERR_PERM;
    }
    if (in != NULL) {
        me->altstack_sp = in->ss_sp;
        me->altstack_size = in->ss_size;
        me->altstack_flags = in->ss_flags;
    }
    return KERR_OK;
}

int signal_altstack(const struct user_sigaltstack *in, struct user_sigaltstack *out) {
    /* No stack pointer to judge "am I on it?" with, so it cannot refuse a
     * change made from inside a handler. Kept for callers that have no
     * frame; the syscall uses the form above. */
    return signal_altstack_at(0, in, out);
}

void signal_raise(uint32_t signo, virt_t fault_address, int32_t code) {
    signal_raise_on(mine(), signo, fault_address, code);
}

void signal_raise_fault(uint32_t signo, virt_t fault_address, int32_t code) {
    struct thread_signals *me = mine();
    signal_raise_on(me, signo, fault_address, code);
    /* Marked as coming from a fault, which is what makes it un-ignorable:
     * see `signal_is_fatal`. */
    if (me != NULL) {
        me->forced = signo;
    }
}

void signal_raise_on(struct thread_signals *who, uint32_t signo, virt_t fault_address,
                     int32_t code) {
    if (who == NULL || !valid_signal(signo)) {
        return;
    }
    who->pending |= bit_of(signo);
    /* Only a fault carries an address, and only one fault can be pending at
     * a time: a second one means the process is ending anyway. */
    if (fault_address != 0 || code != 0) {
        who->fault_address = fault_address;
        who->fault_code = code;
    }
}

/* The lowest-numbered signal that is pending and not blocked, or 0. Lowest
 * first because that is the order the ABI promises, and a runtime that
 * expects SIGSEGV before SIGURG gets it. */
static uint32_t next_deliverable(const struct thread_signals *me) {
    for (uint32_t signo = 1; signo < SIG_COUNT; signo++) {
        if ((me->pending & bit_of(signo)) != 0 && (me->blocked & bit_of(signo)) == 0) {
            return signo;
        }
    }
    return 0;
}

bool signal_is_fatal(uint32_t *signo) {
    struct thread_signals *me = mine();
    const struct user_sigaction *a = actions();
    uint32_t s;
    if (me == NULL || a == NULL) {
        return false;
    }
    /* A **fault's** signal that cannot be delivered is fatal whatever the
     * program asked for: blocked, ignored, or arriving while its own handler
     * runs. Linux calls this forcing the signal, and the reason is that the
     * alternative is not "the program continues" — the faulting instruction
     * is retried, faults again, and the machine spends the rest of its life
     * on it. This kernel printed four million identical fault lines before
     * that was fixed. */
    if (me->forced != 0 && (me->pending & bit_of(me->forced)) != 0) {
        bool blocked = (me->blocked & bit_of(me->forced)) != 0;
        uint64_t handler = a[me->forced].sa_handler;
        if (blocked || handler == SIG_DFL || handler == SIG_IGN) {
            *signo = me->forced;
            return true;
        }
    }
    s = next_deliverable(me);
    if (s == 0) {
        return false;
    }
    /* No handler means the default action, and for everything this kernel
     * raises the default action is to end the process. Ignoring is the one
     * exception and is handled by clearing it here. */
    if (a[s].sa_handler == SIG_IGN) {
        me->pending &= ~bit_of(s);
        return signal_is_fatal(signo);
    }
    if (a[s].sa_handler == SIG_DFL) {
        *signo = s;
        return true;
    }
    return false;
}

/* Where the handler's stack starts: the alternate stack when the action
 * asked for one and one is set, otherwise the interrupted stack.
 *
 * The alternate stack matters for the case it exists for — a stack that has
 * overflowed cannot carry the frame that would report the overflow — and a
 * runtime sets one per thread for exactly that. */
/* Where the handler's frame goes.
 *
 * The alternate stack when the action asked for one and the thread has one —
 * that is what it is for, since a stack that has overflowed cannot carry the
 * frame that would report the overflow.
 *
 * **But not from the top if the thread is already on it.** A second signal
 * arriving inside a handler must put its frame *below* the first one's, not
 * back at the top: starting again at the top overwrites the frame the outer
 * handler will return through, so the outer handler resumes with whatever
 * the inner one left there. That is a corrupted register set delivered to
 * ordinary code, and it showed up as a Go program failing its own
 * consistency checks about one run in twelve under load — never as anything
 * to do with signals. Go's asynchronous preemption makes nested signals
 * ordinary rather than rare. */
static virt_t handler_stack(const struct thread_signals *me, const struct user_sigaction *a,
                            const struct trapframe *tf) {
    bool want_alt = (a->sa_flags & SA_ONSTACK) != 0 && me->altstack_sp != 0 &&
                    (me->altstack_flags & SS_DISABLE) == 0;
    if (!want_alt) {
        return tf->x[2];
    }
    if (already_on_altstack(me, tf->x[2])) {
        return tf->x[2];
    }
    return me->altstack_sp + me->altstack_size;
}

/* Fills the context a handler will read, and that `rt_sigreturn` will read
 * back. The register order is the ABI's: `pc`, then `x1`..`x31`. */
static void save_context(const struct thread_signals *me, struct rt_sigframe *frame,
                         const struct trapframe *tf, uint32_t signo) {
    frame->info = (struct user_siginfo){.si_signo = (int32_t)signo,
                                        .si_code = me->fault_code,
                                        .si_addr = me->fault_address};
    frame->uc = (struct user_ucontext){0};
    frame->uc.uc_sigmask[0] = me->blocked;
    frame->uc.uc_stack.ss_sp = me->altstack_sp;
    frame->uc.uc_stack.ss_size = me->altstack_size;
    /* A cookie in a word no part of the ABI reads, checked on the way back.
     * A frame that comes back changed has been written over by something —
     * a nested handler placed wrongly, a stack that was too small — and
     * without this the only evidence is the program misbehaving later with
     * a register set that is nearly right. */
    frame->uc.pad = SIGFRAME_COOKIE ^ (uint64_t)(uintptr_t)frame;
    frame->uc.uc_mcontext.sc_regs.pc = tf->sepc;
    for (uint32_t i = 1; i < 32u; i++) {
        frame->uc.uc_mcontext.sc_regs.x[i - 1u] = tf->x[i];
    }
    /* The floating-point registers too, into the place the ABI puts them.
     * A handler is ordinary code and will use them; without this it
     * overwrites whatever the interrupted code was holding, and the damage
     * appears after the handler returns, in code that has nothing to do
     * with it. */
    fp_save(&frame->uc.uc_mcontext.sc_fpregs);
}

bool signal_deliver(struct trapframe *tf) {
    struct thread_signals *me = delivery_off ? NULL : mine();
    uint32_t signo;
    const struct user_sigaction *a;
    struct rt_sigframe *frame;
    virt_t sp;
    if (me == NULL) {
        return false;
    }
    signo = next_deliverable(me);
    if (signo == 0) {
        return false;
    }
    a = &actions()[signo];
    if (a->sa_handler == SIG_DFL || a->sa_handler == SIG_IGN) {
        return false; /* the caller deals with the default action */
    }
    /* The frame goes **below** the stack pointer, 16-byte aligned, which the
     * ABI requires of any stack pointer — a handler that spills a 128-bit
     * value relative to an unaligned one faults inside the handler, which is
     * the hardest kind of fault to read. */
    sp = (handler_stack(me, a, tf) - sizeof(struct rt_sigframe)) & ~15ull;
    frame = (struct rt_sigframe *)(uintptr_t)sp;
    save_context(me, frame, tf, signo);
    me->pending &= ~bit_of(signo);
    /* Blocked while the handler runs, plus whatever it asked to block, so a
     * fault inside a SIGSEGV handler does not recurse for ever. */
    me->blocked |= a->sa_mask | ((a->sa_flags & SA_NODEFER) != 0 ? 0u : bit_of(signo));
    tf->x[2] = sp;                       /* sp: the frame */
    tf->x[10] = signo;                   /* a0: which signal */
    tf->x[11] = (uint64_t)(uintptr_t)&frame->info; /* a1: the siginfo */
    tf->x[12] = (uint64_t)(uintptr_t)&frame->uc;   /* a2: the context */
    tf->x[1] = SIGNAL_TRAMPOLINE;        /* ra: where a returning handler lands */
    tf->sepc = a->sa_handler;
    sig.delivered++;
    return true;
}

int signal_return(struct trapframe *tf) {
    const struct rt_sigframe *frame = (const struct rt_sigframe *)(uintptr_t)tf->x[2];
    struct thread_signals *me = mine();
    if (me == NULL || tf->x[2] == 0 || tf->x[2] >= USER_TOP) {
        return KERR_FAULT;
    }
    if (frame->uc.pad != (SIGFRAME_COOKIE ^ (uint64_t)(uintptr_t)frame)) {
        /* The frame this handler must return through has been overwritten.
         * Restoring from it would hand ordinary code a register set that is
         * nearly right, which is far worse than stopping. */
        clobbered_frames++;
        kprintf("\nsignal: the frame at %llx was overwritten while a handler ran\n",
                (unsigned long long)(uintptr_t)frame);
        return KERR_FAULT;
    }
    me->blocked = frame->uc.uc_sigmask[0];
    tf->sepc = frame->uc.uc_mcontext.sc_regs.pc;
    for (uint32_t i = 1; i < 32u; i++) {
        tf->x[i] = frame->uc.uc_mcontext.sc_regs.x[i - 1u];
    }
    fp_restore(&frame->uc.uc_mcontext.sc_fpregs);
    return KERR_OK;
}

uint64_t signal_delivered(void) {
    return sig.delivered;
}

uint64_t signal_clobbered_frames(void) {
    return clobbered_frames;
}
