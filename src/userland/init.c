/* A program that reads the state it was started in, and says what it found.
 *
 * It exists to test the kernel from the outside. A loader can place every
 * segment correctly and still start a program wrongly — the ARM kernel this
 * migrated from had exactly that bug, and it was invisible until something
 * actually *read* `argv` and the auxiliary vector. So this walks the whole
 * block the kernel built and reports it, and the test checks the report.
 *
 * Freestanding, static, no libc: the syscalls are written out by hand
 * because that is the interface under test.
 */
#include <stdint.h>

#define SYS_WRITE 64
#define SYS_EXIT 93

#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_ENTRY 9
#define AT_RANDOM 25
#define AT_EXECFN 31

static long syscall3(long nr, long a, long b, long c) {
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a7 __asm__("a7") = nr;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static uint32_t length_of(const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void say(const char *s) {
    (void)syscall3(SYS_WRITE, 1, (long)(uintptr_t)s, length_of(s));
}

/* Decimal, because a program that cannot print a number cannot report what
 * it found. */
static void say_number(uint64_t v) {
    char buf[24];
    uint32_t at = sizeof buf;
    buf[--at] = '\0';
    if (v == 0) {
        buf[--at] = '0';
    }
    while (v != 0) {
        buf[--at] = (char)('0' + v % 10u);
        v /= 10u;
    }
    say(buf + at);
}

static void say_hex(uint64_t v) {
    char buf[20];
    uint32_t at = 0;
    buf[at++] = '0';
    buf[at++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4) {
        buf[at++] = "0123456789abcdef"[(v >> shift) & 0xf];
    }
    buf[at] = '\0';
    say(buf);
}

/* Walks the auxiliary vector for one tag. Returns 0 when it is absent,
 * which is distinguishable from a zero value only by the caller knowing
 * which tags cannot be zero — AT_PAGESZ and AT_RANDOM among them. */
static uint64_t auxv_find(const uint64_t *auxv, uint64_t tag) {
    for (uint32_t i = 0; auxv[i] != AT_NULL; i += 2u) {
        if (auxv[i] == tag) {
            return auxv[i + 1u];
        }
    }
    return 0;
}

void user_main(const uint64_t *sp) {
    uint64_t argc = sp[0];
    const char *const *argv = (const char *const *)&sp[1];
    const char *const *envp = argv + argc + 1u;
    const uint64_t *auxv;
    uint32_t envc = 0;
    const uint8_t *random;
    say("init: argc=");
    say_number(argc);
    for (uint64_t i = 0; i < argc; i++) {
        say(" argv[");
        say_number(i);
        say("]=");
        say(argv[i]);
    }
    while (envp[envc] != 0) {
        envc++;
    }
    say("\ninit: envc=");
    say_number(envc);
    for (uint32_t i = 0; i < envc; i++) {
        say(" ");
        say(envp[i]);
    }
    auxv = (const uint64_t *)(envp + envc + 1u);
    say("\ninit: pagesz=");
    say_number(auxv_find(auxv, AT_PAGESZ));
    say(" phnum=");
    say_number(auxv_find(auxv, AT_PHNUM));
    say(" phent=");
    say_number(auxv_find(auxv, AT_PHENT));
    say(" phdr=");
    say_hex(auxv_find(auxv, AT_PHDR));
    say(" entry=");
    say_hex(auxv_find(auxv, AT_ENTRY));
    say("\ninit: execfn=");
    say((const char *)(uintptr_t)auxv_find(auxv, AT_EXECFN));
    /* AT_RANDOM must point at sixteen bytes that are not all zero: a
     * runtime seeds its hash tables from them, and all-zero would be a
     * kernel that supplied the tag without supplying the bytes. */
    random = (const uint8_t *)(uintptr_t)auxv_find(auxv, AT_RANDOM);
    say("\ninit: random=");
    if (random == 0) {
        say("absent");
    } else {
        uint32_t nonzero = 0;
        for (uint32_t i = 0; i < 16u; i++) {
            nonzero += random[i] != 0 ? 1u : 0u;
        }
        say_number(nonzero);
        say(" of 16 bytes are non-zero");
    }
    /* The stack pointer must be 16-byte aligned, which the ABI requires and
     * a runtime relies on when it spills. */
    say("\ninit: sp=");
    say_hex((uint64_t)(uintptr_t)sp);
    say(((uint64_t)(uintptr_t)sp % 16u) == 0 ? " (16-byte aligned)" : " (MISALIGNED)");
    say("\ninit: done\n");
    (void)syscall3(SYS_EXIT, 42, 0, 0);
}
