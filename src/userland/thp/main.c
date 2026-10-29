/* Transparent huge pages, and everything that must still work on them.
 *
 * An ordinary anonymous mapping now gets a 2 MiB page on first touch of an
 * aligned window it covers whole. The page-by-page operations then meet a
 * 2 MiB leaf where they used to meet 512 small ones, and each check here is
 * one of them, done to part of a window full of a known pattern: the named
 * part must do what was asked, and **the rest of the window must be
 * untouched**. A kernel that dropped, re-mapped or copied the whole
 * megapage fails on a neighbour.
 *
 * The exit status is a bitmask of the checks that **passed**:
 *   1   MADV_DONTNEED of 4 KiB: zero there, the pattern either side
 *   2   mprotect of 4 KiB read-only: still readable, neighbours writable
 *   4   munmap of 4 KiB: the pattern either side survives
 *   8   fork: the child reads the pattern and its writes stay its own
 *   16  mremap moves a window that was a megapage, contents intact
 *   32  mprotect of a whole window read-only and back: contents intact
 *   64  MADV_PAGEOUT of 4 KiB: that page goes to swap (when there is
 *       swap) and comes back by a fault, the neighbours never left
 */
#include <stdint.h>

#define SYS_EXIT 93
#define SYS_EXIT_GROUP 94
#define SYS_MUNMAP 215
#define SYS_MREMAP 216
#define SYS_CLONE 220
#define SYS_MMAP 222
#define SYS_MPROTECT 226
#define SYS_MADVISE 233
#define SYS_WAIT4 260
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE_ANON 0x22
#define MADV_DONTNEED 4
#define MADV_PAGEOUT 21
#define MREMAP_MAYMOVE 1
#define SIGCHLD 17
#define MEGA 0x200000ul
#define PAGE 4096ul

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

static uint8_t want(uintptr_t at) {
    return (uint8_t)((at >> 12) * 7u + (at & 0xffu) + 1u);
}

/* A fresh aligned 2 MiB window, inside a mapping of three, filled. */
static volatile uint8_t *window(void) {
    long at = syscall6(SYS_MMAP, 0, 3 * MEGA, PROT_READ | PROT_WRITE, MAP_PRIVATE_ANON, -1, 0);
    uintptr_t w = ((uintptr_t)at + MEGA - 1u) & ~(MEGA - 1u);
    if (at < 0) {
        return 0;
    }
    for (uintptr_t i = 0; i < MEGA; i++) {
        ((volatile uint8_t *)w)[i] = want(i);
    }
    return (volatile uint8_t *)w;
}

/* Whether [from, to) of `w` still holds the pattern. */
static int intact(volatile uint8_t *w, uintptr_t from, uintptr_t to) {
    for (uintptr_t i = from; i < to; i++) {
        if (w[i] != want(i)) {
            return 0;
        }
    }
    return 1;
}

static uint32_t check_discard(void) {
    volatile uint8_t *w = window();
    int zero = 1;
    if (w == 0 || syscall6(SYS_MADVISE, (long)(w + MEGA / 2), PAGE, MADV_DONTNEED, 0, 0, 0) != 0) {
        return 0;
    }
    for (uintptr_t i = MEGA / 2; i < MEGA / 2 + PAGE; i++) {
        zero = zero && w[i] == 0;
    }
    return zero && intact(w, 0, MEGA / 2) && intact(w, MEGA / 2 + PAGE, MEGA) ? 1u : 0u;
}

static uint32_t check_protect_part(void) {
    volatile uint8_t *w = window();
    if (w == 0 || syscall6(SYS_MPROTECT, (long)(w + PAGE), PAGE, PROT_READ, 0, 0, 0) != 0) {
        return 0;
    }
    w[0] = want(0);
    w[2 * PAGE] = want(2 * PAGE);
    return intact(w, 0, MEGA) ? 2u : 0u;
}

static uint32_t check_unmap_part(void) {
    volatile uint8_t *w = window();
    if (w == 0 || syscall6(SYS_MUNMAP, (long)(w + 3 * PAGE), PAGE, 0, 0, 0, 0) != 0) {
        return 0;
    }
    return intact(w, 0, 3 * PAGE) && intact(w, 4 * PAGE, MEGA) ? 4u : 0u;
}

static uint32_t check_fork(void) {
    volatile uint8_t *w = window();
    int status = 0;
    long pid;
    if (w == 0) {
        return 0;
    }
    pid = syscall6(SYS_CLONE, SIGCHLD, 0, 0, 0, 0, 0);
    if (pid == 0) {
        int ok = intact(w, 0, MEGA);
        for (uintptr_t i = 0; i < MEGA; i += PAGE) {
            w[i] = 0xee;
        }
        (void)syscall6(SYS_EXIT_GROUP, ok ? 0 : 1, 0, 0, 0, 0, 0);
    }
    if (pid < 0 || syscall6(SYS_WAIT4, pid, (long)&status, 0, 0, 0, 0) != pid) {
        return 0;
    }
    return ((status >> 8) & 0xff) == 0 && intact(w, 0, MEGA) ? 8u : 0u;
}

static uint32_t check_move(void) {
    volatile uint8_t *w = window();
    long to;
    if (w == 0) {
        return 0;
    }
    to = syscall6(SYS_MREMAP, (long)w, MEGA, 3 * MEGA, MREMAP_MAYMOVE, 0, 0);
    return to > 0 && intact((volatile uint8_t *)to, 0, MEGA) ? 16u : 0u;
}

static uint32_t check_protect_whole(void) {
    volatile uint8_t *w = window();
    if (w == 0 || syscall6(SYS_MPROTECT, (long)w, MEGA, PROT_READ, 0, 0, 0) != 0 ||
        !intact(w, 0, MEGA) ||
        syscall6(SYS_MPROTECT, (long)w, MEGA, PROT_READ | PROT_WRITE, 0, 0, 0) != 0) {
        return 0;
    }
    w[5] = want(5);
    return intact(w, 0, MEGA) ? 32u : 0u;
}

static uint32_t check_page_out(void) {
    volatile uint8_t *w = window();
    if (w == 0 || syscall6(SYS_MADVISE, (long)(w + 9 * PAGE), PAGE, MADV_PAGEOUT, 0, 0, 0) != 0) {
        return 0;
    }
    return intact(w, 0, MEGA) ? 64u : 0u;
}

void user_main(void) {
    uint32_t passed = check_discard();
    passed |= check_protect_part();
    passed |= check_unmap_part();
    passed |= check_fork();
    passed |= check_move();
    passed |= check_protect_whole();
    passed |= check_page_out();
    (void)syscall6(SYS_EXIT_GROUP, passed, 0, 0, 0, 0, 0);
}
