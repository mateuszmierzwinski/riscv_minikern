/* selftest — deliberate faults, taken on purpose.
 *
 * `minikern.selftest=<what>` makes the kernel take a specific exception and
 * survive it, which is the only way to know the trap path works before
 * anything else depends on it. The ARM project's equivalent is what proved
 * its vector table, and the faults it could not take cleanly were how three
 * bring-up bugs were found.
 *
 * Each test installs an expected-fault handler, executes the instruction
 * that must fault, and checks that the cause and the faulting address are
 * the ones it asked for. A test that faults differently fails loudly; a
 * test whose instruction does **not** fault fails too, because that is the
 * more dangerous outcome.
 */
#ifndef MINIKERN_BOARD_JH7110_SELFTEST_H
#define MINIKERN_BOARD_JH7110_SELFTEST_H

#include <stdbool.h>

/* Runs the test the command line names, if any. Returns true when one ran. */
bool selftest_run(const char *cmdline);

#endif
