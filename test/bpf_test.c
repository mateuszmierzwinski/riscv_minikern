/* The classic-BPF verifier and interpreter.
 *
 * **The verifier's refusals are the important half.** The interpreter runs
 * with no bounds checks at all, because every one of them has been made
 * unnecessary by the verifier — so a verifier that let a bad program
 * through would not produce a wrong answer, it would read kernel memory or
 * jump into it. Each refusal below is therefore a memory-safety test
 * wearing a validation test's clothes.
 */
#include "core/bpf.h"
#include "ktest.h"
#include <string.h>

/* The encoding, spelled out here rather than shared with the interpreter:
 * a test that built its instructions with the implementation's own macros
 * would agree with a wrong encoding. */
#define LD_W_ABS 0x20u  /* BPF_LD | BPF_W | BPF_ABS */
#define LD_W_IMM 0x00u
#define LD_W_MEM 0x60u
#define LDX_W_MEM 0x61u
#define ST_      0x02u
#define ALU_ADD_K 0x04u
#define ALU_DIV_K 0x34u
#define ALU_LSH_K 0x64u
#define ALU_NEG   0x84u
#define JMP_JEQ_K 0x15u
#define JMP_JA    0x05u
#define RET_K    0x06u
#define MISC_TAX 0x07u

static struct bpf_insn in(uint16_t code, uint8_t jt, uint8_t jf, uint32_t k) {
    struct bpf_insn i = {.code = code, .jt = jt, .jf = jf, .k = k};
    return i;
}

static void data_for(uint8_t *d, uint32_t nr, uint64_t arg0) {
    memset(d, 0, BPF_SECCOMP_DATA_BYTES);
    for (uint32_t i = 0; i < 4u; i++) {
        d[BPF_SECCOMP_OFF_NR + i] = (uint8_t)(nr >> (8u * i));
    }
    for (uint32_t i = 0; i < 8u; i++) {
        d[BPF_SECCOMP_OFF_ARGS + i] = (uint8_t)(arg0 >> (8u * i));
    }
}

static void test_a_filter_that_allows_one_call(void) {
    struct bpf_insn prog[] = {
        in(LD_W_ABS, 0, 0, BPF_SECCOMP_OFF_NR),
        in(JMP_JEQ_K, 0, 1, 63u), /* read? */
        in(RET_K, 0, 0, 0x7fff0000u),
        in(RET_K, 0, 0, 0u),
    };
    const char *why = "";
    uint8_t d[BPF_SECCOMP_DATA_BYTES];
    KTEST_CHECK(bpf_filter_valid(prog, 4u, &why));
    data_for(d, 63u, 0);
    KTEST_CHECK(bpf_filter_run(prog, 4u, d) == 0x7fff0000u);
    data_for(d, 64u, 0);
    KTEST_CHECK(bpf_filter_run(prog, 4u, d) == 0u);
}

static void test_an_argument_can_be_examined(void) {
    /* The point of a filter rather than strict mode: deciding on an
     * argument. This allows the call only when its first argument is 1. */
    struct bpf_insn prog[] = {
        in(LD_W_ABS, 0, 0, BPF_SECCOMP_OFF_ARGS),
        in(JMP_JEQ_K, 0, 1, 1u),
        in(RET_K, 0, 0, 0x7fff0000u),
        in(RET_K, 0, 0, 0x00050000u | 1u), /* ERRNO 1 */
    };
    const char *why = "";
    uint8_t d[BPF_SECCOMP_DATA_BYTES];
    KTEST_CHECK(bpf_filter_valid(prog, 4u, &why));
    data_for(d, 64u, 1u);
    KTEST_CHECK(bpf_filter_run(prog, 4u, d) == 0x7fff0000u);
    data_for(d, 64u, 2u);
    KTEST_CHECK(bpf_filter_run(prog, 4u, d) == (0x00050000u | 1u));
}

static void test_a_load_outside_the_data_is_refused(void) {
    const char *why = "";
    struct bpf_insn past[] = {in(LD_W_ABS, 0, 0, BPF_SECCOMP_DATA_BYTES), in(RET_K, 0, 0, 0)};
    struct bpf_insn edge[] = {in(LD_W_ABS, 0, 0, BPF_SECCOMP_DATA_BYTES - 4u),
                              in(RET_K, 0, 0, 0)};
    struct bpf_insn odd[] = {in(LD_W_ABS, 0, 0, 2u), in(RET_K, 0, 0, 0)};
    struct bpf_insn huge[] = {in(LD_W_ABS, 0, 0, 0xfffffffcu), in(RET_K, 0, 0, 0)};
    KTEST_CHECK(!bpf_filter_valid(past, 2u, &why));
    /* The last whole word is in range — an off-by-one the other way would
     * reject a filter that reads the sixth argument, which is legal. */
    KTEST_CHECK(bpf_filter_valid(edge, 2u, &why));
    /* **Unaligned is refused as well as out of range.** A load at offset 2
     * would straddle `nr` and `arch` and return a number that is neither,
     * which a filter author would never be able to account for. */
    KTEST_CHECK(!bpf_filter_valid(odd, 2u, &why));
    /* And an offset chosen to overflow the bound arithmetic. */
    KTEST_CHECK(!bpf_filter_valid(huge, 2u, &why));
}

static void test_a_jump_past_the_end_is_refused(void) {
    const char *why = "";
    struct bpf_insn far[] = {in(JMP_JA, 0, 0, 100u), in(RET_K, 0, 0, 0)};
    struct bpf_insn branch[] = {in(LD_W_ABS, 0, 0, 0), in(JMP_JEQ_K, 90, 0, 1u),
                                in(RET_K, 0, 0, 0)};
    /* **Overflow, not merely excess.** `at + 1 + k` is computed in 64 bits
     * precisely so that a huge `k` cannot wrap into a target that looks
     * like it lands inside the program. */
    struct bpf_insn wrap[] = {in(JMP_JA, 0, 0, 0xffffffffu), in(RET_K, 0, 0, 0)};
    KTEST_CHECK(!bpf_filter_valid(far, 2u, &why));
    KTEST_CHECK(!bpf_filter_valid(branch, 3u, &why));
    KTEST_CHECK(!bpf_filter_valid(wrap, 2u, &why));
}

static void test_a_filter_must_end_in_a_return(void) {
    const char *why = "";
    struct bpf_insn runs_off[] = {in(LD_W_ABS, 0, 0, 0)};
    struct bpf_insn empty[1] = {in(RET_K, 0, 0, 0)};
    KTEST_CHECK(!bpf_filter_valid(runs_off, 1u, &why));
    /* Refused at install time rather than guessed at run time: allowing
     * would turn a malformed filter into no filter, and killing would
     * punish a program for a mistake that could have been reported. */
    KTEST_CHECK(!bpf_filter_valid(empty, 0u, &why));
    KTEST_CHECK(!bpf_filter_valid(NULL, 1u, &why));
}

static void test_packet_addressing_is_refused(void) {
    const char *why = "";
    struct bpf_insn ind[] = {in(0x40u, 0, 0, 0), in(RET_K, 0, 0, 0)};  /* BPF_LD|BPF_IND */
    struct bpf_insn len[] = {in(0x80u, 0, 0, 0), in(RET_K, 0, 0, 0)};  /* BPF_LD|BPF_LEN */
    struct bpf_insn msh[] = {in(0xa1u, 0, 0, 0), in(RET_K, 0, 0, 0)};  /* BPF_LDX|BPF_MSH */
    struct bpf_insn halfword[] = {in(0x28u, 0, 0, 0), in(RET_K, 0, 0, 0)}; /* LD|H|ABS */
    KTEST_CHECK(!bpf_filter_valid(ind, 2u, &why));
    KTEST_CHECK(!bpf_filter_valid(len, 2u, &why));
    KTEST_CHECK(!bpf_filter_valid(msh, 2u, &why));
    /* Whole words only: the ABI's fields are words, and a half-word load
     * of `nr` would read half a syscall number. */
    KTEST_CHECK(!bpf_filter_valid(halfword, 2u, &why));
}

static void test_scratch_is_bounded(void) {
    const char *why = "";
    struct bpf_insn store[] = {in(ST_, 0, 0, 16u), in(RET_K, 0, 0, 0)};
    struct bpf_insn load[] = {in(LD_W_MEM, 0, 0, 99u), in(RET_K, 0, 0, 0)};
    struct bpf_insn ok[] = {in(ST_, 0, 0, 15u), in(LD_W_MEM, 0, 0, 15u), in(RET_K, 0, 0, 0)};
    /* Sixteen words, so 16 is one past the end — and the interpreter
     * indexes with no check, so this refusal is what keeps a filter from
     * writing into the kernel's stack. */
    KTEST_CHECK(!bpf_filter_valid(store, 2u, &why));
    KTEST_CHECK(!bpf_filter_valid(load, 2u, &why));
    KTEST_CHECK(bpf_filter_valid(ok, 3u, &why));
}

static void test_the_arithmetic_that_would_trap_does_not(void) {
    const char *why = "";
    uint8_t d[BPF_SECCOMP_DATA_BYTES];
    struct bpf_insn div0[] = {in(LD_W_IMM, 0, 0, 7u), in(ALU_DIV_K, 0, 0, 0u),
                              in(RET_K + 0x08u, 0, 0, 0u)}; /* RET | BPF_X ... */
    struct bpf_insn divret[] = {in(LD_W_IMM, 0, 0, 7u), in(ALU_DIV_K, 0, 0, 0u),
                                in(MISC_TAX, 0, 0, 0u), in(RET_K + 0x08u, 0, 0, 0u)};
    struct bpf_insn shift[] = {in(LD_W_IMM, 0, 0, 1u), in(ALU_LSH_K, 0, 0, 64u),
                               in(MISC_TAX, 0, 0, 0u), in(RET_K + 0x08u, 0, 0, 0u)};
    (void)div0;
    data_for(d, 1u, 0);
    /* **Zero rather than a trap**, which is what Linux's interpreter does:
     * a filter that divides by zero should get a wrong answer, not take
     * the kernel down with it. */
    KTEST_CHECK(bpf_filter_valid(divret, 4u, &why));
    KTEST_CHECK(bpf_filter_run(divret, 4u, d) == 0u);
    /* A shift of 64 is undefined in C, so it is defined as zero here —
     * the answer the hardware gives and the one an author expects. */
    KTEST_CHECK(bpf_filter_valid(shift, 4u, &why));
    KTEST_CHECK(bpf_filter_run(shift, 4u, d) == 0u);
}

static void test_a_long_filter_is_refused(void) {
    const char *why = "";
    static struct bpf_insn big[BPF_MAX_INSNS + 1u];
    for (uint32_t i = 0; i < BPF_MAX_INSNS + 1u; i++) {
        big[i] = in(RET_K, 0, 0, 0u);
    }
    /* The filter runs on every syscall, so its length is a real bound
     * rather than a courtesy. */
    KTEST_CHECK(!bpf_filter_valid(big, BPF_MAX_INSNS + 1u, &why));
    KTEST_CHECK(bpf_filter_valid(big, BPF_MAX_INSNS, &why));
}

int main(void) {
    KTEST_RUN(test_a_filter_that_allows_one_call);
    KTEST_RUN(test_an_argument_can_be_examined);
    KTEST_RUN(test_a_load_outside_the_data_is_refused);
    KTEST_RUN(test_a_jump_past_the_end_is_refused);
    KTEST_RUN(test_a_filter_must_end_in_a_return);
    KTEST_RUN(test_packet_addressing_is_refused);
    KTEST_RUN(test_scratch_is_bounded);
    KTEST_RUN(test_the_arithmetic_that_would_trap_does_not);
    KTEST_RUN(test_a_long_filter_is_refused);
    return ktest_failures();
}
