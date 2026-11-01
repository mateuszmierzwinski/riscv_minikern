/* The libFuzzer entry point every harness here defines.
 *
 * Each harness is built twice (BUILD, "Fuzzing"): once against libFuzzer,
 * for the timed open-ended run `gen_fuzz_c.sh` drives, and once against
 * `replay.c`, which feeds it every committed seed and crasher on an
 * ordinary `bazel test`. The harness cannot tell which, and must not. */
#ifndef RISCV_MINIKERN_TEST_FUZZ_H
#define RISCV_MINIKERN_TEST_FUZZ_H

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#endif
