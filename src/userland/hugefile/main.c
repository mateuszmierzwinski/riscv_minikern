/* A file mapped in 2 MiB pages.
 *
 * A private file mapping whose window is 2 MiB aligned in both the address
 * and the file now gets one megapage, filled from the file, where it used to
 * get 512 small ones. The program writes a 4 MiB file of a known pattern,
 * maps it at an aligned address, and checks what a mapping must be:
 *
 *   1   every byte of the first window reads as the file holds it
 *   2   a write into the mapping is the mapping's own: read() of the file
 *       still returns the original
 *   4   mprotect of 4 KiB inside the window (which splits the megapage
 *       and the region) leaves every byte of it as it was
 *   8   the second window reads as the file holds it too
 *   16  munmap of 4 KiB inside a file mapping (it used to be refused)
 *       leaves the pages either side as the file holds them
 *   32  every piece unmapped, the file is still there to read: the cut
 *       regions each held their own reference, none gave back another's
 */
#include <stdint.h>

#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_READ 63
#define SYS_WRITE 64
#define SYS_EXIT_GROUP 94
#define SYS_MUNMAP 215
#define SYS_MPROTECT 226
#define SYS_MMAP 222
#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_WRONLY 01
#define O_CREAT 0100
#define O_TRUNC 01000
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_PRIVATE_ANON 0x22
#define MEGA 0x200000ul
#define PAGE 4096ul
#define FILE_BYTES (2u * MEGA)

static uint8_t chunk[65536];

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
    return (uint8_t)(at % 251u + (at >> 16));
}

/* The file, written in chunks; 1 when it all went. */
static int write_file(const char *path) {
    long fd = syscall6(SYS_OPENAT, AT_FDCWD, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0);
    long put = 0;
    for (uint32_t c = 0; fd >= 0 && c < FILE_BYTES / sizeof chunk; c++) {
        for (uint32_t i = 0; i < sizeof chunk; i++) {
            chunk[i] = want(c * sizeof chunk + i);
        }
        put += syscall6(SYS_WRITE, fd, (long)chunk, sizeof chunk, 0, 0, 0);
    }
    (void)syscall6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return fd >= 0 && put == (long)FILE_BYTES;
}

/* The file mapped at a 2 MiB-aligned address, or 0. */
static volatile uint8_t *map_file(const char *path) {
    long room = syscall6(SYS_MMAP, 0, 3 * MEGA, PROT_READ, MAP_PRIVATE_ANON, -1, 0);
    uintptr_t at = ((uintptr_t)room + MEGA - 1u) & ~(MEGA - 1u);
    long fd = syscall6(SYS_OPENAT, AT_FDCWD, (long)path, O_RDONLY, 0, 0, 0);
    long got;
    if (room < 0 || fd < 0) {
        return 0;
    }
    got = syscall6(SYS_MMAP, (long)at, FILE_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED,
                   fd, 0);
    (void)syscall6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return got == (long)at ? (volatile uint8_t *)at : 0;
}

static int holds(volatile uint8_t *m, uintptr_t from, uintptr_t to) {
    for (uintptr_t i = from; i < to; i++) {
        if (m[i] != want(i)) {
            return 0;
        }
    }
    return 1;
}

/* Whether the file's first bytes are still the pattern, read with read(). */
static int file_unchanged(const char *path) {
    long fd = syscall6(SYS_OPENAT, AT_FDCWD, (long)path, O_RDONLY, 0, 0, 0);
    long n = fd >= 0 ? syscall6(SYS_READ, fd, (long)chunk, 64, 0, 0, 0) : -1;
    int same = n == 64;
    for (uint32_t i = 0; same && i < 64u; i++) {
        same = chunk[i] == want(i);
    }
    (void)syscall6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return same;
}

void user_main(void) {
    volatile uint8_t *m = write_file("/hugefile") ? map_file("/hugefile") : 0;
    uint32_t passed = 0;
    if (m == 0) {
        (void)syscall6(SYS_EXIT_GROUP, 0, 0, 0, 0, 0, 0);
    }
    passed |= holds(m, 0, MEGA) ? 1u : 0u;
    m[0] = (uint8_t)(want(0) + 1u);
    passed |= m[0] == (uint8_t)(want(0) + 1u) && file_unchanged("/hugefile") ? 2u : 0u;
    m[0] = want(0);
    if (syscall6(SYS_MPROTECT, (long)(m + 5 * PAGE), PAGE, PROT_READ, 0, 0, 0) == 0) {
        m[6 * PAGE] = want(6 * PAGE); /* the neighbour is still writable */
        passed |= holds(m, 0, MEGA) ? 4u : 0u;
    }
    passed |= holds(m, MEGA, FILE_BYTES) ? 8u : 0u;
    if (syscall6(SYS_MUNMAP, (long)(m + MEGA + 3 * PAGE), PAGE, 0, 0, 0, 0) == 0) {
        passed |= holds(m, MEGA, MEGA + 3 * PAGE) && holds(m, MEGA + 4 * PAGE, FILE_BYTES) ? 16u
                                                                                          : 0u;
    }
    if (syscall6(SYS_MUNMAP, (long)m, FILE_BYTES, 0, 0, 0, 0) == 0 && file_unchanged("/hugefile")) {
        passed |= 32u;
    }
    (void)syscall6(SYS_EXIT_GROUP, passed, 0, 0, 0, 0, 0);
}
