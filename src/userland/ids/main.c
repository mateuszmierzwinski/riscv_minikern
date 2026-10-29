/* Saved ids: a privilege drop that can be undone, and one that cannot.
 *
 * A daemon that must do one privileged thing later keeps root as its
 * *saved* uid and runs as somebody else; one that never needs it again
 * drops all three. Both are `setresuid`, and the difference is only in
 * what the capability sets do (Linux's rule, which this kernel follows).
 *
 * The exit status is a bitmask of the checks that **passed**:
 *   1   setresuid(1000, 1000, 0) as root
 *   2   getresuid reports (1000, 1000, 0)
 *   4   while dropped, an id it does not hold is refused (no privilege)
 *   8   setresuid(-1, 0, -1) returns the effective uid to root
 *   16  and the privilege with it: an id it does not hold is accepted
 *   32  after setresuid(1000, 1000, 1000), root cannot be reached again
 *   64  an unprivileged setuid to the uid it holds succeeds
 */
#include <stdint.h>

#define SYS_EXIT_GROUP 94
#define SYS_SETUID 146
#define SYS_SETRESUID 147
#define SYS_GETRESUID 148
#define SYS_GETEUID 175
#define KEEP (-1L)
#define EPERM 1

static long syscall3(long nr, long a, long b, long c) {
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a7 __asm__("a7") = nr;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static long setresuid(long r, long e, long s) {
    return syscall3(SYS_SETRESUID, r, e, s);
}

static uint32_t ids_are(uint32_t r, uint32_t e, uint32_t s) {
    uint32_t got[3] = {~0u, ~0u, ~0u};
    long rc = syscall3(SYS_GETRESUID, (long)&got[0], (long)&got[1], (long)&got[2]);
    return rc == 0 && got[0] == r && got[1] == e && got[2] == s;
}

/* The temporary drop and its undoing: bits 1 to 16. */
static uint32_t temporary(void) {
    uint32_t passed = 0;
    passed |= setresuid(1000, 1000, 0) == 0 ? 1u : 0u;
    passed |= ids_are(1000, 1000, 0) ? 2u : 0u;
    passed |= setresuid(2000, KEEP, KEEP) == -EPERM ? 4u : 0u;
    passed |= setresuid(KEEP, 0, KEEP) == 0 && syscall3(SYS_GETEUID, 0, 0, 0) == 0 ? 8u : 0u;
    passed |= setresuid(3000, KEEP, KEEP) == 0 ? 16u : 0u;
    return passed;
}

/* The permanent drop: bits 32 and 64. */
static uint32_t permanent(void) {
    uint32_t passed = 0;
    if (setresuid(1000, 1000, 1000) == 0 && syscall3(SYS_SETUID, 0, 0, 0) == -EPERM &&
        setresuid(KEEP, 0, KEEP) == -EPERM) {
        passed |= 32u;
    }
    passed |= syscall3(SYS_SETUID, 1000, 0, 0) == 0 && ids_are(1000, 1000, 1000) ? 64u : 0u;
    return passed;
}

void user_main(void) {
    uint32_t passed = temporary();
    passed |= permanent();
    (void)syscall3(SYS_EXIT_GROUP, passed, 0, 0);
}
