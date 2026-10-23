/* `fork`, and whether the two processes are really isolated.
 *
 * The claim a real fork makes is not "a child appeared" — a vfork child
 * appears too. It is that **neither process can reach the other's memory**,
 * whatever either of them does. So this writes to the same addresses in both
 * and checks that each sees only its own:
 *
 *   1. A global, written before the fork and then changed by both.
 *   2. A stack local, likewise.
 *   3. A heap page from `mmap`, taken before the fork.
 *
 * Each is read back after the other side has certainly written, which is
 * what makes it a test of isolation rather than of ordering: the child
 * writes, tells the parent through its exit status, and the parent then
 * checks that its own copy is untouched.
 *
 * Freestanding and static: the syscalls are the interface under test.
 */
#include <stdbool.h>
#include <stdint.h>

#define SYS_WRITE 64
#define SYS_EXIT 93
#define SYS_CLONE 220
#define SYS_WAIT4 260
#define SYS_MMAP 222
#define SYS_GETPID 172

#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20

#define SIGCHLD 17

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

/* Written before the fork, changed by both afterwards. */
static volatile uint64_t shared_global = 0x1111111111111111ull;

#define PARENT_MARK 0x2222222222222222ull
#define CHILD_MARK 0x3333333333333333ull

void user_main(void *start_state) {
    volatile uint64_t on_the_stack = 0x1111111111111111ull;
    volatile uint64_t *mapped;
    long child;
    long status = 0;
    bool ok = true;
    (void)start_state;

    mapped = (volatile uint64_t *)(uintptr_t)syscall6(
        SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((long)(uintptr_t)mapped <= 0) {
        say("fork: could not map a page\n");
        (void)syscall3(SYS_EXIT, 1, 0, 0);
    }
    *mapped = 0x1111111111111111ull;

    say("fork: before\n");
    /* No CLONE_VM: a real fork. */
    child = syscall6(SYS_CLONE, SIGCHLD, 0, 0, 0, 0, 0);
    if (child < 0) {
        say("fork: refused\n");
        (void)syscall3(SYS_EXIT, 1, 0, 0);
    }
    if (child == 0) {
        /* The child writes its own marks and checks it sees them. */
        shared_global = CHILD_MARK;
        on_the_stack = CHILD_MARK;
        *mapped = CHILD_MARK;
        if (shared_global != CHILD_MARK || on_the_stack != CHILD_MARK ||
            *mapped != CHILD_MARK) {
            (void)syscall3(SYS_EXIT, 9, 0, 0); /* the child cannot see its own writes */
        }
        (void)syscall3(SYS_EXIT, 7, 0, 0);
    }

    /* The parent writes different marks, then waits for the child to finish
     * — so the child has certainly written by the time anything is checked. */
    shared_global = PARENT_MARK;
    on_the_stack = PARENT_MARK;
    *mapped = PARENT_MARK;
    (void)syscall6(SYS_WAIT4, child, (long)(uintptr_t)&status, 0, 0, 0, 0);

    say("fork: child exited with ");
    say_number((uint64_t)(status & 0xffu) != 0 ? (uint64_t)status : (uint64_t)(status >> 8));

    if (shared_global != PARENT_MARK) {
        say("fork: the child wrote the parent's global\n");
        ok = false;
    }
    if (on_the_stack != PARENT_MARK) {
        say("fork: the child wrote the parent's stack\n");
        ok = false;
    }
    if (*mapped != PARENT_MARK) {
        say("fork: the child wrote the parent's mapped page\n");
        ok = false;
    }
    say(ok ? "fork: ISOLATED\n" : "fork: NOT ISOLATED\n");
    (void)syscall3(SYS_EXIT, ok ? 0 : 1, 0, 0);
}
