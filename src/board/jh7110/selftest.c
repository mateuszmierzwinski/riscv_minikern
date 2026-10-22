#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/riscv/mmu.h"
#include "arch/riscv/trap.h"
#include "board/jh7110/selftest.h"
#include "core/cmdline.h"
#include "core/console.h"

/* What the running test expects, and what it saw. A file-scope pair because
 * the handler is called from the trap path and cannot be passed anything. */
struct expectation {
    uint64_t want_cause;
    uint64_t want_stval;
    bool check_stval;
    bool fired;
    uint64_t saw_cause;
    uint64_t saw_stval;
};

static struct expectation expect;

/* How long the instruction at `pc` is.
 *
 * **An instruction here is 2 or 4 bytes**, because `rv64gc` includes the
 * compressed extension and the assembler uses it: the low two bits of the
 * first halfword are 0b11 for a 4-byte instruction and anything else for a
 * 2-byte one. That rule has to be read, not assumed — assuming 4 worked for
 * `csrr` and for `sd zero` (which cannot compress, since x0 is not one of
 * the registers `c.sd` can name) and resumed **inside** `c.ld` and
 * `c.ebreak`, which then raised an illegal instruction of their own. The
 * symptom was a second, unexplained fault; the cause was arithmetic.
 *
 * A fixed instruction width is a thing the ARM kernel this migrated from
 * could rely on in ARM state and could not in Thumb. Here there is no
 * mode — the widths are mixed in the same instruction stream. */
static uint32_t insn_length_at(uint64_t pc) {
    uint16_t first = *(const volatile uint16_t *)(uintptr_t)pc;
    return (first & 3u) == 3u ? 4u : 2u;
}

/* Steps over the instruction that faulted, which is what lets a deliberate
 * fault be survived rather than merely observed. */
static bool step_over(struct trapframe *tf) {
    expect.fired = true;
    expect.saw_cause = tf->scause;
    expect.saw_stval = tf->stval;
    if (tf->scause != expect.want_cause) {
        return false; /* not what this test asked for: let it be reported */
    }
    tf->sepc += insn_length_at(tf->sepc);
    return true;
}

static void arm_expectation(uint64_t cause, uint64_t stval, bool check_stval) {
    expect = (struct expectation){.want_cause = cause, .want_stval = stval, .check_stval = check_stval};
    trap_set_expected(step_over);
}

/* True when the fault arrived and was the one asked for. */
static bool expectation_met(const char *what) {
    trap_set_expected(NULL);
    if (!expect.fired) {
        kprintf("selftest: %s did NOT fault, which is worse than faulting\n", what);
        return false;
    }
    if (expect.saw_cause != expect.want_cause) {
        kprintf("selftest: %s gave cause %llu (%s), wanted %llu\n", what,
                (unsigned long long)expect.saw_cause, trap_cause_name(expect.saw_cause),
                (unsigned long long)expect.want_cause);
        return false;
    }
    if (expect.check_stval && expect.saw_stval != expect.want_stval) {
        kprintf("selftest: %s faulted at %016llx, wanted %016llx\n", what,
                (unsigned long long)expect.saw_stval, (unsigned long long)expect.want_stval);
        return false;
    }
    kprintf("selftest: %s faulted as expected: %s at %016llx\n", what,
            trap_cause_name(expect.saw_cause), (unsigned long long)expect.saw_stval);
    return true;
}

/* An M-mode CSR read. This is not an arbitrary illegal instruction: reading
 * `misa` from S-mode is exactly the mistake that killed this kernel's first
 * spike before it printed a character (CLAUDE.md §5), so the trap path is
 * proved against the real thing. */
static bool test_illegal_instruction(void) {
    uint64_t sink = 0;
    arm_expectation(EXC_ILLEGAL_INSN, 0, false);
    __asm__ volatile("csrr %0, 0x301" : "=r"(sink));
    (void)sink;
    return expectation_met("an M-mode CSR read from S-mode");
}

/* An unreachable address raises **different causes depending on whether
 * translation is on**, and the difference is a real diagnosis rather than a
 * detail: with paging off it is an *access* fault, meaning nothing on the bus
 * answered; with paging on it is a *page* fault, meaning the address is not
 * in the table. A kernel that conflates them sends its reader to the page
 * tables for a bus problem, or to the memory map for a missing mapping.
 *
 * So the test asks the MMU which world it is in rather than hard-coding one,
 * and this is how it was found: the expectation was written before paging
 * existed, and the first boot with Sv39 on reported a load page fault where
 * the test still wanted an access fault. */
static bool test_load_fault(void) {
    const uint64_t bad = 0x3000000000000000ull;
    uint64_t sink = 0;
    arm_expectation(mmu_enabled() ? EXC_LOAD_PAGE_FAULT : EXC_LOAD_FAULT, bad, true);
    __asm__ volatile("ld %0, 0(%1)" : "=r"(sink) : "r"(bad));
    (void)sink;
    return expectation_met("a load from unreachable memory");
}

static bool test_store_fault(void) {
    const uint64_t bad = 0x3000000000000008ull;
    arm_expectation(mmu_enabled() ? EXC_STORE_PAGE_FAULT : EXC_STORE_FAULT, bad, true);
    __asm__ volatile("sd zero, 0(%0)" : : "r"(bad));
    return expectation_met("a store to unreachable memory");
}

/* The kernel's own code must not be writable. This is the protection the
 * identity map sets up (text read-execute, everything else read-write), and
 * it is worth proving rather than assuming: a kernel that believes its text
 * is protected and is wrong has a weakness nobody will notice. */
static bool test_kernel_text_is_not_writable(void) {
    extern char __kernel_start[];
    uint64_t at = (uint64_t)(uintptr_t)__kernel_start;
    if (!mmu_enabled()) {
        kprintf("selftest: translation is off, so text protection does not apply\n");
        return true;
    }
    arm_expectation(EXC_STORE_PAGE_FAULT, at, true);
    __asm__ volatile("sd zero, 0(%0)" : : "r"(at));
    return expectation_met("a store into the kernel's own text");
}

/* An ebreak, which is how a debugger stops a program and how this kernel
 * proves it can take a trap that is neither an error nor an interrupt. */
static bool test_breakpoint(void) {
    arm_expectation(EXC_BREAKPOINT, 0, false);
    __asm__ volatile("ebreak");
    return expectation_met("a breakpoint");
}

/* One name per test, so a failing one can be run on its own. `all` runs the
 * lot, which is what the test suite asks for. */
struct selftest {
    const char *name;
    bool (*run)(void);
};

static const struct selftest tests[] = {
    {"illegal", test_illegal_instruction},
    {"load", test_load_fault},
    {"store", test_store_fault},
    {"break", test_breakpoint},
    {"rotext", test_kernel_text_is_not_writable},
};

bool selftest_run(const char *cmdline) {
    bool ok = true, ran = false;
    size_t len = 0;
    const char *want = cmdline_value(cmdline, "minikern.selftest", &len);
    if (want == NULL) {
        return false;
    }
    kprintf("selftest: taking deliberate faults\n");
    for (uint32_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        if (!cmdline_equals(cmdline, "minikern.selftest", "all") &&
            !cmdline_equals(cmdline, "minikern.selftest", tests[i].name)) {
            continue;
        }
        ran = true;
        ok = tests[i].run() && ok;
    }
    if (!ran) {
        kprintf("selftest: no test called \"%.*s\"\n", (int)len, want);
        return true;
    }
    kprintf("selftest: %s\n", ok ? "all faults taken and survived" : "FAILED");
    return true;
}
