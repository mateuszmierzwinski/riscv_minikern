/* What the CPU does to a frame (ROADMAP §10, the baseline acceleration is
 * weighed against): clear and copy one 1080p XRGB frame, 8,294,400 bytes,
 * in anonymous memory, eight times each, with 64-bit stores — the loops a
 * software renderer's fill and blit come down to. Prints the best pass of
 * each in MB/s and microseconds; exits 0 when both ran.
 */
#include <stdint.h>

#define SYS_WRITE 64
#define SYS_EXIT_GROUP 94
#define SYS_CLOCK_GETTIME 113
#define SYS_MMAP 222
#define CLOCK_MONOTONIC 1L
#define PROT_RW 3L
#define MAP_PRIVATE_ANON 0x22L
#define FRAME (1920u * 1080u * 4u)
#define PASSES 8u

static long syscall6(long nr, long a, long b, long c, long d, long e, long f) {
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a4 __asm__("a4") = e;
    register long a5 __asm__("a5") = f;
    register long a7 __asm__("a7") = nr;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7) : "memory");
    return a0;
}

static uint64_t now_ns(void) {
    long ts[2];
    (void)syscall6(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)ts, 0, 0, 0, 0);
    return (uint64_t)ts[0] * 1000000000u + (uint64_t)ts[1];
}

static void fill(volatile uint64_t *dst, uint64_t value) {
    for (uint32_t i = 0; i < FRAME / 8u; i++) {
        dst[i] = value;
    }
}

static void copy(volatile uint64_t *dst, const volatile uint64_t *src) {
    for (uint32_t i = 0; i < FRAME / 8u; i++) {
        dst[i] = src[i];
    }
}

/* Appends the decimal of v to out at *n. */
static void put_number(char *out, uint32_t *n, uint64_t v) {
    char digits[20];
    uint32_t k = 0;
    do {
        digits[k++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0u);
    while (k > 0u) {
        out[(*n)++] = digits[--k];
    }
}

static void put_text(char *out, uint32_t *n, const char *s) {
    while (*s != '\0') {
        out[(*n)++] = *s++;
    }
}

/* "memspeed: <what> <MB/s> MB/s, <us> us a frame\n" for the best of PASSES. */
static void report(const char *what, uint64_t best_ns) {
    char line[96];
    uint32_t n = 0;
    put_text(line, &n, "memspeed: ");
    put_text(line, &n, what);
    put_text(line, &n, " ");
    put_number(line, &n, (uint64_t)FRAME * 1000u / (best_ns == 0u ? 1u : best_ns));
    put_text(line, &n, " MB/s, ");
    put_number(line, &n, best_ns / 1000u);
    put_text(line, &n, " us a frame\n");
    (void)syscall6(SYS_WRITE, 1, (long)line, n, 0, 0, 0);
}

void user_main(void) {
    long a = syscall6(SYS_MMAP, 0, FRAME, PROT_RW, MAP_PRIVATE_ANON, -1, 0);
    long b = syscall6(SYS_MMAP, 0, FRAME, PROT_RW, MAP_PRIVATE_ANON, -1, 0);
    uint64_t best_fill = ~0ull, best_copy = ~0ull;
    if ((a < 0 && a > -4096) || (b < 0 && b > -4096)) {
        (void)syscall6(SYS_EXIT_GROUP, 1, 0, 0, 0, 0, 0);
    }
    fill((uint64_t *)b, 0x00ff8000ff800000ull); /* touched once: the passes measure memory, not faults */
    fill((uint64_t *)a, 0);
    for (uint32_t p = 0; p < PASSES; p++) {
        uint64_t t0 = now_ns();
        fill((uint64_t *)a, p);
        uint64_t t1 = now_ns();
        copy((uint64_t *)a, (const uint64_t *)b);
        uint64_t t2 = now_ns();
        best_fill = t1 - t0 < best_fill ? t1 - t0 : best_fill;
        best_copy = t2 - t1 < best_copy ? t2 - t1 : best_copy;
    }
    report("fill", best_fill);
    report("copy", best_copy);
    (void)syscall6(SYS_EXIT_GROUP, 0, 0, 0, 0, 0, 0);
}
