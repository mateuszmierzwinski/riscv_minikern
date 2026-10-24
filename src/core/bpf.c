#include "core/bpf.h"

#include <stddef.h>

/* The instruction encoding, as classic BPF defines it: a class in the low
 * three bits, then a size or an operation, then a source. */
#define BPF_CLASS(c) ((c) & 0x07u)
#define BPF_LD   0x00u
#define BPF_LDX  0x01u
#define BPF_ST   0x02u
#define BPF_STX  0x03u
#define BPF_ALU  0x04u
#define BPF_JMP  0x05u
#define BPF_RET  0x06u
#define BPF_MISC 0x07u

#define BPF_SIZE(c) ((c) & 0x18u)
#define BPF_W 0x00u
#define BPF_H 0x08u
#define BPF_B 0x10u

#define BPF_MODE(c) ((c) & 0xe0u)
#define BPF_IMM 0x00u
#define BPF_ABS 0x20u
#define BPF_IND 0x40u
#define BPF_MEM 0x60u
#define BPF_LEN 0x80u
#define BPF_MSH 0xa0u

#define BPF_OP(c) ((c) & 0xf0u)
#define BPF_ADD 0x00u
#define BPF_SUB 0x10u
#define BPF_MUL 0x20u
#define BPF_DIV 0x30u
#define BPF_OR  0x40u
#define BPF_AND 0x50u
#define BPF_LSH 0x60u
#define BPF_RSH 0x70u
#define BPF_NEG 0x80u
#define BPF_MOD 0x90u
#define BPF_XOR 0xa0u

#define BPF_JA   0x00u
#define BPF_JEQ  0x10u
#define BPF_JGT  0x20u
#define BPF_JGE  0x30u
#define BPF_JSET 0x40u

#define BPF_SRC(c) ((c) & 0x08u)
#define BPF_K 0x00u
#define BPF_X 0x08u

#define BPF_TAX 0x00u
#define BPF_TXA 0x80u

/* The scratch memory a filter may use. Sixteen words, as classic BPF
 * defines, and an index outside it is rejected by the verifier rather than
 * wrapped — a filter writing past its scratch would be writing into the
 * kernel's stack. */
#define BPF_MEMWORDS 16u

/* A word loaded from the input, big-endian-independent: the structure is in
 * the host's byte order because the kernel filled it, so this is a plain
 * aligned read rather than a conversion. */
static uint32_t load_word(const uint8_t *data, uint32_t off) {
    return (uint32_t)data[off] | ((uint32_t)data[off + 1u] << 8) |
           ((uint32_t)data[off + 2u] << 16) | ((uint32_t)data[off + 3u] << 24);
}

/* Whether one instruction is one we will run, and whether its jumps and
 * offsets are inside their bounds. */
static bool insn_valid(const struct bpf_insn *in, uint32_t at, uint32_t count, const char **why) {
    uint16_t code = in->code;
    switch (BPF_CLASS(code)) {
    case BPF_LD:
    case BPF_LDX:
        /* **Only immediate, absolute and scratch loads.** `BPF_IND`,
         * `BPF_LEN` and `BPF_MSH` address a packet, and the input here is a
         * fixed structure — an indirect load would read from an offset that
         * means nothing. */
        if (BPF_MODE(code) == BPF_ABS) {
            if (BPF_SIZE(code) != BPF_W) {
                *why = "a seccomp filter may only load whole words";
                return false;
            }
            /* Word-aligned and inside the input. The alignment matters as
             * much as the bound: an unaligned load would straddle two
             * fields and return a number that is neither. */
            if ((in->k & 3u) != 0u || in->k > BPF_SECCOMP_DATA_BYTES - 4u) {
                *why = "a load outside the syscall data, or unaligned";
                return false;
            }
            return true;
        }
        if (BPF_MODE(code) == BPF_MEM) {
            if (in->k >= BPF_MEMWORDS) {
                *why = "a scratch index outside the sixteen words";
                return false;
            }
            return true;
        }
        if (BPF_MODE(code) == BPF_IMM) {
            return true;
        }
        *why = "packet-relative addressing has no meaning for a syscall";
        return false;
    case BPF_ST:
    case BPF_STX:
        if (in->k >= BPF_MEMWORDS) {
            *why = "a scratch index outside the sixteen words";
            return false;
        }
        return true;
    case BPF_ALU:
        switch (BPF_OP(code)) {
        case BPF_ADD:
        case BPF_SUB:
        case BPF_MUL:
        case BPF_DIV:
        case BPF_OR:
        case BPF_AND:
        case BPF_LSH:
        case BPF_RSH:
        case BPF_NEG:
        case BPF_MOD:
        case BPF_XOR:
            return true;
        default:
            *why = "an arithmetic operation this interpreter does not have";
            return false;
        }
    case BPF_JMP: {
        /* **Forwards only, and inside the program.** Classic BPF has no
         * backward jump, which is the whole reason a filter is known to
         * terminate — so termination needs no instruction budget, only this
         * check. `at + 1 + off` is computed in 64 bits because the sum of
         * two 32-bit values would wrap into a target that looks valid. */
        uint64_t next = (uint64_t)at + 1u;
        uint64_t ja = next + in->k;
        uint64_t jt = next + in->jt;
        uint64_t jf = next + in->jf;
        if (BPF_OP(code) == BPF_JA) {
            if (ja >= count) {
                *why = "a jump past the end of the filter";
                return false;
            }
            return true;
        }
        if (BPF_OP(code) != BPF_JEQ && BPF_OP(code) != BPF_JGT && BPF_OP(code) != BPF_JGE &&
            BPF_OP(code) != BPF_JSET) {
            *why = "a comparison this interpreter does not have";
            return false;
        }
        if (jt >= count || jf >= count) {
            *why = "a branch past the end of the filter";
            return false;
        }
        return true;
    }
    case BPF_RET:
        if (BPF_SRC(code) != BPF_K && BPF_SRC(code) != BPF_X) {
            *why = "a return from neither a constant nor X";
            return false;
        }
        return true;
    case BPF_MISC:
        if ((code & 0xf8u) != BPF_TAX && (code & 0xf8u) != BPF_TXA) {
            *why = "a miscellaneous instruction this interpreter does not have";
            return false;
        }
        return true;
    default:
        *why = "an instruction class this interpreter does not have";
        return false;
    }
}

bool bpf_filter_valid(const struct bpf_insn *prog, uint32_t count, const char **why) {
    *why = "";
    if (prog == NULL || count == 0u || count > BPF_MAX_INSNS) {
        *why = count == 0u ? "an empty filter" : "a filter longer than 4096 instructions";
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (!insn_valid(&prog[i], i, count, why)) {
            return false;
        }
    }
    /* **The last instruction must return.** Otherwise execution runs off
     * the end, and the only answers available there are "allow", which
     * turns a malformed filter into no filter, and "kill", which punishes a
     * program for a mistake the kernel could have told it about at install
     * time. Refusing the filter is the one answer that is neither. */
    if (BPF_CLASS(prog[count - 1u].code) != BPF_RET) {
        *why = "a filter whose last instruction is not a return";
        return false;
    }
    return true;
}

uint32_t bpf_filter_run(const struct bpf_insn *prog, uint32_t count, const uint8_t *data) {
    uint32_t mem[BPF_MEMWORDS] = {0};
    uint32_t a = 0, x = 0;
    uint32_t pc = 0;
    while (pc < count) {
        const struct bpf_insn *in = &prog[pc];
        uint16_t code = in->code;
        uint32_t src = BPF_SRC(code) == BPF_X ? x : in->k;
        pc++;
        switch (BPF_CLASS(code)) {
        case BPF_LD:
            a = BPF_MODE(code) == BPF_ABS ? load_word(data, in->k)
                                          : (BPF_MODE(code) == BPF_MEM ? mem[in->k] : in->k);
            break;
        case BPF_LDX:
            x = BPF_MODE(code) == BPF_ABS ? load_word(data, in->k)
                                          : (BPF_MODE(code) == BPF_MEM ? mem[in->k] : in->k);
            break;
        case BPF_ST:
            mem[in->k] = a;
            break;
        case BPF_STX:
            mem[in->k] = x;
            break;
        case BPF_ALU:
            switch (BPF_OP(code)) {
            case BPF_ADD: a += src; break;
            case BPF_SUB: a -= src; break;
            case BPF_MUL: a *= src; break;
            /* **Zero yields zero rather than trapping**, which is what
             * Linux's interpreter does: a filter that divides by zero
             * should get a wrong answer, not take the kernel with it. */
            case BPF_DIV: a = src == 0u ? 0u : a / src; break;
            case BPF_MOD: a = src == 0u ? 0u : a % src; break;
            case BPF_OR: a |= src; break;
            case BPF_AND: a &= src; break;
            /* A shift of 32 or more is undefined in C, so it is defined
             * here as zero — the answer the hardware would give and the one
             * a filter author expects. */
            case BPF_LSH: a = src >= 32u ? 0u : a << src; break;
            case BPF_RSH: a = src >= 32u ? 0u : a >> src; break;
            case BPF_XOR: a ^= src; break;
            default: a = (uint32_t)(-(int32_t)a); break; /* BPF_NEG */
            }
            break;
        case BPF_JMP: {
            bool take;
            if (BPF_OP(code) == BPF_JA) {
                pc += in->k;
                break;
            }
            take = BPF_OP(code) == BPF_JEQ
                       ? a == src
                       : (BPF_OP(code) == BPF_JGT ? a > src
                                                  : (BPF_OP(code) == BPF_JGE ? a >= src
                                                                             : (a & src) != 0u));
            pc += take ? in->jt : in->jf;
            break;
        }
        case BPF_RET:
            return BPF_SRC(code) == BPF_X ? x : in->k;
        default: /* BPF_MISC */
            if ((code & 0xf8u) == BPF_TAX) {
                x = a;
            } else {
                a = x;
            }
            break;
        }
    }
    /* Unreachable for a verified program, whose last instruction returns.
     * Killing rather than allowing, because arriving here means the
     * verifier and the interpreter disagree — and the safe reading of that
     * disagreement is that the filter is not being applied. */
    return 0u;
}
