/* boardspec — describing a board on the command line, for one whose device
 * tree is missing a fact or has it wrong.
 *
 * Three facts are enough to come up and say what is wrong with the rest:
 * where memory is, where the console is, and how fast the timer counts.
 *
 *   minikern.mem=<base>:<size>           size may end in K, M or G
 *   earlycon=uart8250,<mmio|mmio32>,<addr>   Linux's own syntax: mmio is
 *                                        one-byte registers, mmio32 four
 *                                        bytes each, four apart
 *   minikern.timebase=<Hz>
 *
 * Each one given replaces what the tree says; anything not given comes
 * from the tree as before. Pure, so the parsing is host-tested.
 */
#ifndef MINIKERN_CORE_BOARDSPEC_H
#define MINIKERN_CORE_BOARDSPEC_H

#include <stdbool.h>
#include <stdint.h>

struct board_spec {
    bool has_mem;
    uint64_t mem_base;
    uint64_t mem_size;
    bool has_uart;
    uint64_t uart_base;
    uint32_t uart_shift;  /* log2 of the register spacing */
    uint32_t uart_width;  /* bytes per access */
    uint64_t timebase;    /* 0: from the tree */
};

/* Reads every override the command line gives. A malformed one is ignored
 * (the tree's value stands), never half-applied. */
void board_spec_parse(const char *cmdline, struct board_spec *out);

#endif
