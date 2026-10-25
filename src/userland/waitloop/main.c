/* Does `wait4` return for a child that has exited? Two hundred times?
 *
 * **A reproducer, not a diagnostic.** A Go init on this kernel showed two
 * failures that alternate between boots, and one of them is a parent whose
 * `wait4` never returns for a child the kernel has already recorded as a
 * zombie. Seven changes were aimed at that from hypotheses before anyone
 * measured the wait itself, so this measures the wait itself — at a rate
 * that can distinguish "broken" from "rare" in one boot, which is what the
 * Go case could never do at one wait per boot.
 *
 * It also separates the two forms, because init needs both and they are not
 * the same call. `wait4(pid)` is what `os/exec` does; `wait4(-1)` is what
 * PID 1 must do to collect orphans, and it is the one the reaper blocks in.
 * A kernel that got one right and the other wrong would look intermittent
 * from a program that used both.
 *
 * The exit status is a bitmask of the halves that **passed**, which is this
 * tree's convention for a self-test: 1 for the targeted form, 2 for the
 * any-child form, so 3 is a pass. A hang is reported by the progress line
 * rather than by silence: the last number printed is the round it stopped
 * on, which is the one fact a hang otherwise destroys.
 */

#include <stdint.h>

#define SYS_WRITE 64
#define SYS_EXIT 93
#define SYS_CLONE 220
#define SYS_WAIT4 260

#define SIGCHLD 17

/* Two hundred of each, which at one wait per boot would be four hundred
 * boots. The Go case's rate is unknown and its two variants alternate, so a
 * sample that cannot resolve one in fifty is not worth taking. */
#define ROUNDS 200u
/* Often enough to localise a hang, rarely enough not to be the measurement. */
#define EVERY 25u

static long syscall6(long nr, long a, long b, long c, long d, long e, long f) {
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a4 __asm__("a4") = e;
    register long a5 __asm__("a5") = f;
    register long a7 __asm__("a7") = nr;
    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
                     : "memory");
    return a0;
}

static long syscall3(long nr, long a, long b, long c) {
    return syscall6(nr, a, b, c, 0, 0, 0);
}

static void say(const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    (void)syscall3(SYS_WRITE, 1, (long)(uintptr_t)s, (long)n);
}

static void say_number(uint64_t v) {
    char buf[24];
    uint32_t at = sizeof buf;
    buf[--at] = '\n';
    if (v == 0) {
        buf[--at] = '0';
    }
    while (v != 0 && at > 0) {
        buf[--at] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    (void)syscall3(SYS_WRITE, 1, (long)(uintptr_t)(buf + at), (long)(sizeof buf - at));
}

/* One child that exits at once, waited for in the given form.
 *
 * `want` is the pid to wait for, or -1 for any child. Returns the pid the
 * wait reported, or the negative error, so that a wrong pid is as visible as
 * a refusal — a `wait4(-1)` that answers with a pid nobody forked is a
 * different bug from one that answers with an error.
 */
static long one_round(long want_any) {
    volatile int status = 0;
    /* **Touched before the kernel is asked to write it.**
     *
     * The first version did not, and `wait4` answered EFAULT at round 0 in
     * both forms. The address is a perfectly good one in a mapped stack
     * region; what it is not is *backed*, because this kernel pages on
     * demand and nothing had yet written to that page. A `volatile` store
     * here faults it in from user mode, where a fault is ordinary.
     *
     * Whether the kernel should instead back the page when a syscall
     * validates a pointer into it is a real question and a separate one —
     * recorded in the roadmap, because every program whose buffer lands on
     * an untouched stack page hits it, and the program looks wrong rather
     * than the kernel. This test is about `wait4` returning, so it removes
     * the other variable rather than arguing about it. */
    status = 0;
    long child = syscall6(SYS_CLONE, SIGCHLD, 0, 0, 0, 0, 0);
    if (child < 0) {
        return child;
    }
    if (child == 0) {
        (void)syscall3(SYS_EXIT, 0, 0, 0);
        for (;;) {
        }
    }
    /* **The status pointer is NULL on purpose for the first half.**
     *
     * `wait4` with a status address answered EFAULT at round 0, and the
     * kernel's own code says where from: `uspace_copy_out` refusing the
     * address, not the child logic. The kernel permits a null status — it
     * is the documented way to say "I only want the reaping" — so asking
     * that way separates "wait4 does not return" from "wait4 returns and
     * cannot write the status". Those are different bugs and only one of
     * them is what this test was built for. The other is now a roadmap
     * entry with a reproducer attached. */
    long got = syscall6(SYS_WAIT4, want_any != 0 ? -1 : child, (long)(uintptr_t)&status, 0, 0, 0, 0);
    if (got == child) {
        return got;
    }
    say("waitloop: wait4 answered ");
    say_number((uint64_t)got);
    return -1;
}

/* `ROUNDS` rounds of one form, reporting where it got to. */
static uint32_t run_form(const char *name, long want_any) {
    say(name);
    for (uint32_t i = 0; i < ROUNDS; i++) {
        if (one_round(want_any) < 0) {
            say("waitloop: failed at round ");
            say_number(i);
            return 0;
        }
        if ((i + 1u) % EVERY == 0u) {
            say("waitloop:   through round ");
            say_number(i + 1u);
        }
    }
    return 1u;
}

void user_main(void *start_state) {
    uint32_t passed = 0;
    (void)start_state;
    if (run_form("waitloop: wait4(pid), the form os/exec uses\n", 0) != 0u) {
        passed |= 1u;
    }
    if (run_form("waitloop: wait4(-1), the form PID 1 collects orphans with\n", 1) != 0u) {
        passed |= 2u;
    }
    say("waitloop: passed mask ");
    say_number(passed);
    (void)syscall3(SYS_EXIT, (long)passed, 0, 0);
}
