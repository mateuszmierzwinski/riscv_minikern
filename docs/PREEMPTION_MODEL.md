# Preemption model

When the scheduler interrupts a thread: what is built, the board
measurements, what is not built, the knobs. Per-publish figures: `BENCHMARKS.md`.

## The hardware costs it is built on

| Measured (`minikern.probe=1`) | Value | Consequence |
|---|---|---|
| `Sstc` (`stimecmp`) | absent | every timer arm is an SBI call |
| `sbi_set_timer` | 335 ns | never re-arm for an unchanged deadline |
| `satp` ASID bits | 0 | every address-space switch flushes the TLB |
| `sfence.vma` + 64-page refill | 32 + 296 ns | ~330 ns per address-space switch, 0.02% of a 1.5 ms slice |
| SBI IPI send | 1499 ns | poke only when it buys something |
| `time` CSR | 4 MHz (249 ns/tick) | 100 µs rounding = 400 ticks |
| `context_switch` alone | 46 ns | register swap only, **not** the cost of a preemption |

The full preemption path is unmeasured in isolation; removing 2,567
preemptions saved ~12 ms, which puts it in the microseconds.

## The model

- **A hart is interrupted only when something is due.** No periodic tick. The
  scheduler arms for the soonest of: (1) the running thread's slice, only if
  something else is runnable; (2) the soonest deadline a blocked thread waits
  for (sleep, futex/wait timeout); (3) the housekeeping beat. None due → no timer.
- **Deadlines round up to 100 µs** (`TIMER_GRANULARITY_NS`), so near ones coincide.
- **No re-arm when unchanged** (no firmware call), so `sched_arm_timer` simply
  recomputes at every return to user mode and every scheduling decision.
- **One owner of the timer state.** `set_hart_timer` is the only function that
  changes "armed" and the comparator, unconditionally. Disarm clears the
  enable bit rather than parking the comparator far away. When the periodic
  clock owns the comparator, `sched_arm_timer` does nothing.
- **Pokes, not clocks.** An IPI goes only to a hart idle in `wfi`, or running
  an untimed thread that should now be preempted (else a newly runnable thread
  waits for ever behind a syscall-free loop). Busy, timed harts get nothing.
- **Slices are measured** against the `time` CSR, not counted in ticks.
- **Housekeeping beat with no owner** (`timer_add_housekeeping_hook`): interval
  work (polled NIC, retransmit timers) on one hart per beat, preferring an idle
  one. Pinned to one hart, it cost that hart's thread 1,001 interrupts vs 11.
- **Idle harts release to the kernel page table**; `uspace_destroy` calls
  `sched_forget_address_space` before freeing a process's table.

## Invariants (both must read zero, and do)

- **`sched_lost_deadlines`** — before a hart stops looking (`wfi` or running
  untimed), `check_deadline_is_armed` checks the system's soonest deadline is
  armed on some hart. A miss would be a silent hang; it is counted, not panicked.
- **`sched_missed_pokes`** — an idle hart's backstop timer found work that
  should have woken it: a lost wakeup rescued by a timeout.
- Also: `timer_interrupts_of`, `timer_housekeeping_beats`, and
  `sched_overdue_wakes` (blocked 1 s past its deadline → reported and woken).

## Results (board)

Same Go workload, periodic vs dynamic:

| | periodic | dynamic |
|---|---|---|
| Context switches | 3,111 | **544** |
| 8 units in parallel | 35 ms | **24 ms** |
| Speedup on 8 units | 2.4x | **3.6x** |
| Missed pokes / lost deadlines | 0 / 0 | 0 / 0 |

One busy thread for 15 s, no syscalls (`minikern.spin=1`):

| | hart running it | whole machine | pointless switches |
|---|---|---|---|
| Periodic | 1,504 interrupts | 6,028 | 1,501 |
| Dynamic | **3** | 467 | **1** |

The machine's 467 is the idle harts' 10/s backstop; with a network up, the beat.

## Scheduling classes

- **Fair** (`src/core/fair.c`, `fair_test`): weighted virtual time (Linux's
  nice→weight table); smallest clock wins, round-robin breaks ties; charged on
  switch-out; slice = weighted share of 12 ms, floor 1.5 ms, zero if alone.
  `setpriority`/`getpriority` for `PRIO_PROCESS`, self only; `setpriority`
  clamps. Board, one hart: nice −5 vs +5 got 90.3% / 9.7%, ratio **9.34**
  against a predicted **9.32**.
- **Real-time**: `rt_priority` 0 = fair, 1..99 = RT. `SCHED_FIFO` has no
  slice; `SCHED_RR` has a 10 ms quantum against equal priority only. A waking
  RT thread marks and pokes every hart running something it outranks.
  Leaving the class places a fresh virtual clock. Self only. Board: `rf`.
- **Affinity and isolation**: masks of hart *indices*, not hart ids.
  Affinity is per thread (inherited at `clone`), isolation the machine's;
  the effective mask is computed, so isolation reaches existing threads.
  `sched_setaffinity` refuses an empty result and moves the thread at once;
  `sched_getaffinity` reports the thread's own mask. Board: isolating index 3
  took its switches 131→0 and `NumCPU` 4→3; pinning to index 2 gave one switch.

**Frequency**: 1500 MHz ÷ {1–4} through one divider (measured 100/50/33/25%);
safe because voltage stays at the top point's 1040 mV, so no power saving yet (ROADMAP §4).

## Not built

- Lazy floating point — FP state is saved and restored unconditionally; the
  largest known item on the preemption path.
- Skipping the `satp` write in the switch path itself on a same-address-space
  switch (the scheduler already avoids adopting an unchanged one).
- Targeted TLB shootdown — removals still broadcast to every hart.
- An isolated measurement of the full preemption path.
- Tickless network — the polled NIC keeps a 10 ms beat (ROADMAP §4).
- `sched_arm_timer` can arm for an instant already past (ROADMAP §0).

## Knobs

| Command-line option | Effect |
|---|---|
| `minikern.periodic` | put the old periodic clock back (dynamic ticks are default); the "broken without a tick?" control |
| `minikern.spin` | one busy thread, 15 s, no syscalls; the interrupt-count measurement |
| `minikern.nice` | two threads at nice ±5; run with `minikern.smp=off` |
| `minikern.rt` | `SCHED_FIFO` vs fair thread; run with `minikern.smp=off` |
| `minikern.pin` | pin a thread to hart index 2 |
| `minikern.isolate=<mask>` | keep work off the harts in the mask (hex accepted) |
| `minikern.smp=off` | one hart only |
| `minikern.probe=1` | print the hardware cost table above |
| `minikern.nopreempt` | refuse `tgkill`, i.e. Go async preemption off |
| `minikern.trace` | scheduler trace |
