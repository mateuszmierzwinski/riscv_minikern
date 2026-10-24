/* A classic-BPF interpreter, for `seccomp` filters and nothing else.
 *
 * **Why an interpreter rather than a mode switch.** `seccomp` has two
 * modes: strict, which permits four syscalls, and filter, which runs a
 * program the process supplied. Strict mode alone is a handful of lines and
 * is almost never what anybody wants — a sandbox needs to allow `read` on
 * some descriptors, or `openat` with particular flags, which is a decision
 * about *arguments*. That decision is the filter, so a kernel without one
 * does not have `seccomp` in any useful sense.
 *
 * Classic BPF is the format the ABI fixes: a program is an array of
 * `{code, jt, jf, k}` and `libseccomp` emits exactly that. Inventing a
 * simpler filter language would mean no existing program could use it.
 *
 * **What is deliberately absent.** Packet-relative addressing (`BPF_IND`,
 * `BPF_MSH`, `BPF_LEN`) has no meaning here: the input is a fixed 64-byte
 * structure, not a packet, so an indirect load is a load from an offset
 * that does not exist. `seccomp` forbids those instructions for the same
 * reason, and the verifier rejects them rather than the interpreter
 * guessing.
 *
 * Division and modulo by zero are defined to yield zero rather than
 * trapping, which is what Linux's interpreter does — a filter that divides
 * by zero gets a wrong answer rather than taking the kernel down with it.
 */
#ifndef CORE_BPF_H
#define CORE_BPF_H

#include <stdbool.h>
#include <stdint.h>

/* One classic-BPF instruction, laid out as the ABI has it. */
struct bpf_insn {
    uint16_t code;
    uint8_t jt;
    uint8_t jf;
    uint32_t k;
};

/* The longest filter accepted. Linux's limit is 4096 instructions; this is
 * the same, and it is a real bound rather than a courtesy — the filter runs
 * on **every** syscall, so a program that installed a million instructions
 * would have made its own kernel slow on purpose. */
#define BPF_MAX_INSNS 4096u

/* The input a `seccomp` filter reads, by the offsets the ABI fixes. A
 * filter loads from these and nothing else. */
#define BPF_SECCOMP_DATA_BYTES 64u
#define BPF_SECCOMP_OFF_NR     0u  /* int32: the syscall number */
#define BPF_SECCOMP_OFF_ARCH   4u  /* uint32 */
#define BPF_SECCOMP_OFF_IP     8u  /* uint64 */
#define BPF_SECCOMP_OFF_ARGS   16u /* six uint64 */

/* Whether a program is one this interpreter will run.
 *
 * Checked **once, when the filter is installed**, so that running it needs
 * no bounds tests in the inner loop — which matters because the loop is on
 * the syscall path. Every jump is verified to land inside the program and
 * forwards (classic BPF has no backward jumps, which is what makes
 * termination free rather than a step budget), every load is verified to be
 * an aligned word inside the input, and the last instruction is verified to
 * be a return.
 *
 * `why` receives a short reason, for the complaint a refusal owes its
 * caller — "filter rejected" alone sends somebody to read this file. */
bool bpf_filter_valid(const struct bpf_insn *prog, uint32_t count, const char **why);

/* Runs a verified program over a 64-byte input and returns its value.
 *
 * Only ever called with a program `bpf_filter_valid` accepted, which is why
 * it can be a plain loop with no error path: the verifier has already made
 * every instruction safe to execute. */
uint32_t bpf_filter_run(const struct bpf_insn *prog, uint32_t count, const uint8_t *data);

#endif /* CORE_BPF_H */
