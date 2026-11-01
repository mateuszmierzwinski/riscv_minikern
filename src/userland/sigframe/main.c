/* Rule 25's syscall surface, the parts the first program does not reach.
 *
 * The signal path, the descriptor table and a check-then-use race, each
 * attacked from a child process: a kernel that mishandles one faults on a
 * user address or writes where a program aimed it, and a fault in the
 * kernel stops the machine. So the parent reaching its report is half of
 * every result; the child's ending is the other half.
 *
 * The exit status is a bitmask of the checks that **passed**:
 *   1   a signal delivered to a thread whose sp points into the kernel ends
 *       it with SIGSEGV (the frame is not written there)
 *   2   the same with sp on an unmapped user page
 *   4   rt_sigreturn with sp on an unmapped user page ends it with SIGSEGV
 *   8   rt_sigreturn with sp in the kernel's half ends it with SIGSEGV
 *   16  opening /dev/null repeatedly stops at EMFILE
 *   32  after one close the next open succeeds, and dup3 to a descriptor
 *       past the table's limit is EBADF
 *   64  a path that another thread unmaps and remaps while openat reads it
 *       is answered every time (a descriptor, ENOENT or EFAULT)
 * 127 is all seven.
 */
#include <stdint.h>

#define SYS_DUP3 24
#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_EXIT 93
#define SYS_EXIT_GROUP 94
#define SYS_NANOSLEEP 101
#define SYS_KILL 129
#define SYS_RT_SIGACTION 134
#define SYS_MUNMAP 215
#define SYS_CLONE 220
#define SYS_MMAP 222
#define SYS_WAIT4 260
#define AT_FDCWD (-100L)
#define SIGKILL 9L
#define SIGUSR1 10L
#define SIGSEGV 11
#define SIGCHLD 17L
#define WNOHANG 1L
#define ENOENT 2
#define EBADF 9
#define EFAULT 14
#define EMFILE 24
#define KERNEL_ADDRESS 0xffffffc000400000L
#define UNMAPPED_ADDRESS 0x8000L /* below the program, which links at 0x10000 */
#define PAGE 4096L
#define MAP_PRIVATE_ANON 0x22L
#define MAP_FIXED 0x10L
#define OPENS_MAX 70000u
#define RACE_ROUNDS 3000u
#define REAP_TRIES 1000u /* 10 ms each: ten seconds */
#define CLONE_THREAD_FLAGS 0x00010f00 /* VM|FS|FILES|SIGHAND|THREAD */

long clone_thread(long flags, void *stack_top, void (*entry)(void));

struct sigaction_raw {
    uint64_t handler;
    uint64_t flags;
    uint64_t mask;
};

static char racer_stack[16384] __attribute__((aligned(16)));
static volatile uint32_t stop_racing, racer_done;
static volatile long race_page;

static long syscall4(long nr, long a, long b, long c, long d) {
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a7 __asm__("a7") = nr;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0;
}

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

/* Moves sp to `at` and spins: the next signal's frame goes below it. */
static void __attribute__((noreturn, naked)) spin_on_stack(long at) {
    __asm__ volatile("mv sp, a0\n1: j 1b\n");
}

/* Moves sp to `at` and returns from a handler that never ran. */
static void __attribute__((noreturn, naked)) sigreturn_on_stack(long at) {
    __asm__ volatile("mv sp, a0\nli a7, 139\necall\n1: j 1b\n");
}

static void on_usr1(int signo) {
    (void)signo;
}

static void sleep_ms(long ms) {
    long ts[2] = {0, ms * 1000000L};
    (void)syscall4(SYS_NANOSLEEP, (long)ts, 0, 0, 0);
}

static long fork_child(void) {
    return syscall6(SYS_CLONE, SIGCHLD, 0, 0, 0, 0, 0);
}

/* The child's wait status, or -1 when it would not end in ten seconds
 * (then it is killed, so a stuck child does not outlive the test). */
static long reap(long pid) {
    int32_t status = 0;
    for (uint32_t i = 0; i < REAP_TRIES; i++) {
        if (syscall4(SYS_WAIT4, pid, (long)&status, WNOHANG, 0) == pid) {
            return status;
        }
        sleep_ms(10);
    }
    (void)syscall4(SYS_KILL, pid, SIGKILL, 0, 0);
    (void)syscall4(SYS_WAIT4, pid, (long)&status, 0, 0);
    return -1;
}

static uint32_t ended_by_sigsegv(long status) {
    return status >= 0 && (status & 0x7f) == SIGSEGV ? 1u : 0u;
}

/* Bits 1 and 2: a signal sent to a child spinning on a hostile stack. */
static uint32_t signalled_on(long sp) {
    long pid = fork_child();
    if (pid == 0) {
        spin_on_stack(sp);
    }
    sleep_ms(100);
    (void)syscall4(SYS_KILL, pid, SIGUSR1, 0, 0);
    return ended_by_sigsegv(reap(pid));
}

/* Bits 4 and 8: rt_sigreturn through a hostile stack. */
static uint32_t returned_on(long sp) {
    long pid = fork_child();
    if (pid == 0) {
        sigreturn_on_stack(sp);
    }
    return ended_by_sigsegv(reap(pid));
}

/* The descriptor table, filled and recovered; the child's exit is bits. */
static void fill_the_table(void) {
    uint32_t passed = 0;
    long fd = 0;
    for (uint32_t i = 0; i < OPENS_MAX && fd >= 0; i++) {
        fd = syscall4(SYS_OPENAT, AT_FDCWD, (long)"/dev/null", 0, 0);
    }
    passed |= fd == -EMFILE ? 1u : 0u;
    (void)syscall4(SYS_CLOSE, 3, 0, 0, 0);
    if (syscall4(SYS_OPENAT, AT_FDCWD, (long)"/dev/null", 0, 0) == 3 &&
        syscall4(SYS_DUP3, 0, 1L << 20, 0, 0) == -EBADF) {
        passed |= 2u;
    }
    (void)syscall4(SYS_EXIT_GROUP, passed, 0, 0, 0);
}

/* Maps the race page at its fixed address and writes the path into it. */
static void put_the_path(void) {
    static const char path[] = "/dev/null";
    long at = syscall6(SYS_MMAP, race_page, PAGE, 3, MAP_PRIVATE_ANON | MAP_FIXED, -1, 0);
    for (uint32_t i = 0; at == race_page && i < sizeof path; i++) {
        ((volatile char *)at)[i] = path[i];
    }
}

/* The other thread: the path's page, taken away and put back, until told. */
static void racer(void) {
    while (!stop_racing) {
        (void)syscall4(SYS_MUNMAP, race_page, PAGE, 0, 0);
        put_the_path();
    }
    racer_done = 1;
    (void)syscall4(SYS_EXIT, 0, 0, 0, 0);
}

/* The child's side of bit 64: exits 1 when every open was answered. */
static void race_the_path(void) {
    uint32_t odd = 0;
    race_page = syscall6(SYS_MMAP, 0, PAGE, 3, MAP_PRIVATE_ANON, -1, 0);
    put_the_path();
    (void)clone_thread(CLONE_THREAD_FLAGS, racer_stack + sizeof racer_stack, racer);
    for (uint32_t i = 0; i < RACE_ROUNDS; i++) {
        long fd = syscall4(SYS_OPENAT, AT_FDCWD, race_page, 0, 0);
        if (fd >= 0) {
            (void)syscall4(SYS_CLOSE, fd, 0, 0, 0);
        } else if (fd != -ENOENT && fd != -EFAULT) {
            odd++;
        }
    }
    stop_racing = 1;
    while (!racer_done) {
        sleep_ms(1);
    }
    (void)syscall4(SYS_EXIT_GROUP, odd == 0 ? 1 : 0, 0, 0, 0);
}

/* Runs `body` in a child and returns its exit code, or 0 if it did not exit. */
static uint32_t child_exit_code(void (*body)(void)) {
    long pid = fork_child(), status;
    if (pid == 0) {
        body();
    }
    status = reap(pid);
    return status >= 0 && (status & 0x7f) == 0 ? (uint32_t)((status >> 8) & 0xff) : 0u;
}

void user_main(void) {
    struct sigaction_raw act = {.handler = (uint64_t)(uintptr_t)on_usr1};
    uint32_t passed = 0, table;
    (void)syscall4(SYS_RT_SIGACTION, SIGUSR1, (long)&act, 0, 8);
    passed |= signalled_on(KERNEL_ADDRESS) ? 1u : 0u;
    passed |= signalled_on(UNMAPPED_ADDRESS) ? 2u : 0u;
    passed |= returned_on(UNMAPPED_ADDRESS) ? 4u : 0u;
    passed |= returned_on(KERNEL_ADDRESS) ? 8u : 0u;
    table = child_exit_code(fill_the_table);
    passed |= (table & 1u) != 0 ? 16u : 0u;
    passed |= (table & 2u) != 0 ? 32u : 0u;
    passed |= child_exit_code(race_the_path) == 1u ? 64u : 0u;
    (void)syscall4(SYS_EXIT_GROUP, passed, 0, 0, 0);
}
