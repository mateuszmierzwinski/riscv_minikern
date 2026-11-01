/* W^X: no user mapping is writable and executable at once.
 *
 * A page an attacker can write and the processor will run is the shortest
 * path from a memory-corruption bug to code execution, so the kernel refuses
 * to make one — at `mmap` and at `mprotect` — with `EACCES`, as Linux does
 * under SELinux's `execmem` denial. What it must still allow is the JIT's
 * two-step: write the code, then flip the page to read-and-execute.
 *
 * The exit status is a bitmask of the checks that **passed**:
 *   1   mmap(PROT_WRITE | PROT_EXEC) is refused with EACCES
 *   2   mmap(PROT_READ | PROT_WRITE) succeeds
 *   4   mprotect of that to PROT_WRITE | PROT_EXEC is refused with EACCES
 *   8   mprotect of it to PROT_READ | PROT_EXEC succeeds, and the code
 *       written before the flip runs
 *   16  mprotect back to PROT_READ | PROT_WRITE succeeds
 *   32  mmap(PROT_READ | PROT_EXEC) succeeds
 * 63 is all six. With `minikern.allowwx` (the control) 1 and 4 fail: 58.
 */
#include <stdint.h>

#define SYS_EXIT_GROUP 94
#define SYS_MMAP 222
#define SYS_MPROTECT 226
#define PROT_READ 1L
#define PROT_WRITE 2L
#define PROT_EXEC 4L
#define MAP_PRIVATE_ANON 0x22L
#define EACCES 13
#define PAGE 4096L
#define RET_INSN 0x8082u /* c.jr ra — `ret` */

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

static long map_anon(long prot) {
    return syscall6(SYS_MMAP, 0, PAGE, prot, MAP_PRIVATE_ANON, -1, 0);
}

static long protect(long at, long prot) {
    return syscall6(SYS_MPROTECT, at, PAGE, prot, 0, 0, 0);
}

/* Writes a `ret` at `at`, makes the page executable, and calls it. */
static uint32_t flip_and_run(long at) {
    *(volatile uint16_t *)at = RET_INSN;
    if (protect(at, PROT_READ | PROT_EXEC) != 0) {
        return 0;
    }
    __asm__ volatile("fence.i" ::: "memory");
    ((void (*)(void))at)();
    return 8u;
}

void user_main(void) {
    uint32_t passed = 0;
    long rw;
    passed |= map_anon(PROT_READ | PROT_WRITE | PROT_EXEC) == -EACCES ? 1u : 0u;
    rw = map_anon(PROT_READ | PROT_WRITE);
    if (rw > 0) {
        passed |= 2u;
        passed |= protect(rw, PROT_READ | PROT_WRITE | PROT_EXEC) == -EACCES ? 4u : 0u;
        passed |= flip_and_run(rw);
        passed |= protect(rw, PROT_READ | PROT_WRITE) == 0 ? 16u : 0u;
    }
    passed |= map_anon(PROT_READ | PROT_EXEC) > 0 ? 32u : 0u;
    syscall6(SYS_EXIT_GROUP, (long)passed, 0, 0, 0, 0, 0);
}
