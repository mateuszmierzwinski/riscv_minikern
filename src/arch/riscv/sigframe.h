/* The signal frame, exactly as user space expects to find it.
 *
 * This is an **ABI fact**, not a design choice: a runtime reads these fields
 * at these offsets, so getting one wrong means it reads a register out of
 * the middle of another and does something inexplicable. The layout was
 * taken from what a Go runtime for `linux/riscv64` actually reads
 * (`runtime/defs_linux_riscv64.go`), and the static assertions below are
 * what keep this file honest.
 *
 * The register block is **`pc` followed by `x1`..`x31`** — not `x0`..`x31`.
 * `x0` is hard-wired to zero and carries no information, so the ABI spends
 * its slot on the program counter instead. A kernel that writes 32
 * registers starting at `x0` puts every one of them one slot too high.
 *
 * What a signal needs beyond the frame: **the kernel must supply the return
 * path**. A Go runtime sets the `SA_RESTORER` *flag* but leaves
 * `sa_restorer` zero on this architecture — only x86 gets a real one — so
 * there is no address in the request to return through. Linux points `ra` at
 * a trampoline it maps itself, and so does this kernel
 * (`SIGNAL_TRAMPOLINE`). The ARM project met the same problem in its own
 * form and paid a day for it; this is that lesson, measured rather than
 * assumed.
 */
#ifndef MINIKERN_ARCH_RISCV_SIGFRAME_H
#define MINIKERN_ARCH_RISCV_SIGFRAME_H

#include <stdint.h>

/* `siginfo_t`, of which a runtime reads the signal number, the code and the
 * address. 128 bytes, because that is what the ABI says it is — a shorter
 * one would have the frame's next field land in the middle of it. */
struct user_siginfo {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t pad;
    uint64_t si_addr;
    uint8_t rest[104];
};

struct user_stack {
    uint64_t ss_sp;
    int32_t ss_flags;
    int32_t pad;
    uint64_t ss_size;
};

/* `pc`, then `x1`..`x31`. */
struct user_regs {
    uint64_t pc;
    uint64_t x[31];
};

/* The floating-point half: thirty-two doubles then `fcsr`, and then the
 * space the ABI reserves for the wider extensions this kernel does not
 * implement.
 *
 * It is **filled**, not reserved. An earlier version left it zeroed and said
 * so, as a known limitation — and the limitation turned out to corrupt a Go
 * program about one run in twelve, because a handler is ordinary code that
 * uses these registers and the interrupted code was holding values in them.
 * The failures looked like anything but floating point: "WaitGroup misuse"
 * on correct code, and a panic while printing a panic. */
struct user_fpregs {
    uint64_t f[32];
    uint32_t fcsr;
    uint8_t reserved[528u - 32u * 8u - 4u];
};

struct user_sigcontext {
    struct user_regs sc_regs;
    struct user_fpregs sc_fpregs;
};

struct user_ucontext {
    uint64_t uc_flags;
    uint64_t uc_link;
    struct user_stack uc_stack;
    uint64_t uc_sigmask[16];  /* the ABI's 1024-bit set, of which 64 bits are used */
    uint64_t pad;
    struct user_sigcontext uc_mcontext;
};

/* What the kernel puts on the stack, and what `sp` points at when the
 * handler starts. */
struct rt_sigframe {
    struct user_siginfo info;
    struct user_ucontext uc;
};

_Static_assert(sizeof(struct user_siginfo) == 128u, "siginfo is 128 bytes");
_Static_assert(__builtin_offsetof(struct user_siginfo, si_addr) == 16u, "si_addr at 16");
_Static_assert(__builtin_offsetof(struct user_ucontext, uc_mcontext) == 176u,
               "uc_mcontext at 176 — a runtime reads every register through this offset");
_Static_assert(sizeof(struct user_regs) == 256u, "32 words: pc and x1..x31");
_Static_assert(__builtin_offsetof(struct rt_sigframe, uc) == 128u, "the context follows siginfo");

/* The signal codes a runtime distinguishes. SEGV_MAPERR is "nothing is
 * mapped there", which is what a nil dereference is, and a runtime turns
 * exactly that into a recoverable panic rather than a crash. */
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define BUS_ADRALN 1
#define BUS_ADRERR 2
#define ILL_ILLOPC 1

#endif
