# Preemption model

What this kernel does about deciding when to interrupt a thread, why it does
it that way, and what is built versus still owed. Written after
implementation, so it describes code rather than intentions — where something
is not built, it says so.

The numbers it rests on were measured on a real VisionFive 2 and are in
`docs/scheduler-phase0.md`. Three of them decide almost everything here:

| Measured | Consequence |
|---|---|
| **No `Sstc`** — there is no supervisor timer CSR | every arm of the timer is a call into firmware, **335 ns** |
| **No ASID** — `satp` implements zero address-space id bits | every switch between address spaces flushes the whole TLB, costing **~330 ns** in fence plus refill |
| **An IPI costs 1499 ns** to send | poking a hart is thirty context switches; it must buy something |

A context switch itself is **46 ns**. So the expensive things are talking to
firmware and talking to other harts, and the cheap thing is switching
threads. The model follows from that.

---

## The model

**A hart is interrupted only when something is due.** There is no periodic
tick in the design. A clock that fires a hundred times a second spends 335 ns
re-arming itself each time to discover, usually, that nothing has changed.

Three things can be due, and the scheduler arms for whichever is soonest:

1. **The running thread's slice.** Only when something else is runnable — a
   thread with no competition is not taking a turn from anybody, and
   interrupting it buys nothing.
2. **The soonest deadline a blocked thread is waiting for** — a sleep, a
   futex with a timeout, a wait with a timeout.
3. **Housekeeping**, which only one hart does. (Field present, nothing uses
   it yet.)

If none of them applies, the hart wants no timer at all.

**Deadlines are rounded up to 100 µs** (`TIMER_GRANULARITY_NS`). That is 400
ticks of this board's 4 MHz clock — coarse enough that two threads whose
slices end microseconds apart ask for the *same* deadline, fine enough that a
1.5 ms slice is not visibly lengthened. The rounding exists to make the next
rule fire often.

**The timer is not re-armed for a deadline it already holds.** Each hart
remembers what it last wrote to the comparator; an arm for the same value
returns without a firmware call. This is the rule that makes arming on every
trap affordable, and it is why the scheduler can simply recompute its answer
at every return to user mode rather than tracking when the answer might have
changed.

**A hart that must look again is poked, not waited for.** Two kinds need it:

- an **idle** hart, asleep in `wfi`, which has no clock of its own; and
- a hart **running a thread with no timer**, which disarmed because nothing
  else wanted the hart and now something does.

The second is easy to miss and was: without it, a thread that became runnable
while another hart ran a tight loop would wait for ever, because the looping
hart had nothing to interrupt it and the loop made no syscalls. It is the
"**or should preempt**" half of the rule, and it is not an optimisation.

Harts that are neither get nothing, because an IPI costs thirty context
switches and tells a busy hart something it will find out at its next trap
anyway.

**Slices are measured, not counted.** Without a periodic clock there is
nothing to decrement, so each hart records when the running thread's slice
began and compares against the `time` CSR when it is asked. One slice length
for now, 1.5 ms; the fair class will compute it per thread.

---

## Why 1.5 ms, with no ASID

The plan allowed for raising the minimum granularity to ~3 ms on a machine
with no address-space ids, on the grounds that every switch flushes the TLB.
**The measurement says not to.** The flush is 32 ns and the refill of a
64-page working set is 296 ns, so an address-space switch costs about 330 ns
more than a thread switch. Against a 1.5 ms slice that is **0.02%**.

Doubling the slice to save two parts in ten thousand would cost latency
everywhere for nothing. This is exactly what Phase 0 was for: the fallback
was reasonable and the hardware did not need it.

---

## What is built

- One-shot deadlines per hart, through SBI (`timer_arm_at`, `timer_disarm`).
- No re-arm when the deadline is unchanged.
- 100 µs rounding.
- The scheduler computing its own next deadline from slice, sleepers and
  housekeeping (`sched_arm_timer`), called at every return to user mode and
  at every scheduling decision.
- Slice expiry measured against the `time` CSR.
- Software interrupts between harts: `sbi_send_ipi`, a handler that clears
  the pending bit, and the enable bit set per hart.
- Poking only harts that are idle or running untimed.
- Per-hart state cache-line aligned (it already was).
- Counters that make the model checkable rather than believable:
  `timer_interrupts_of`, `sched_missed_pokes`.

## What is not built

- **Lazy floating point.** State is saved and restored unconditionally
  rather than only when `sstatus.FS` says it is dirty.
- **Skipping `satp` on a same-address-space switch.** The scheduler checks
  whether the address space has changed before adopting one, so the common
  case already avoids the write; the switch path itself does not distinguish
  them.
- **Housekeeping on one hart.** The field exists; nothing schedules work
  into it, and the network still polls from a tick hook.
- **Targeted TLB shootdown.** A removal still broadcasts to every hart
  rather than to the harts that have run that address space.
- **`SIGSTOP` and `SIGCONT`.** Neither is implemented, so a process cannot
  be stopped and resumed.

## What is held back, and why

**Dynamic ticks are off by default.** `minikern.dynticks` turns them on, and
`sched_arm_timer` does nothing at all while they are off — one owner of the
comparator at a time, or the two schedules fight.

### The two bugs turning them on has already found

**A disarm that skipped the mask.** Disarming is spelled as clearing the
timer's enable bit rather than setting the comparator far away, because an
out-of-range comparator value is at the implementation's discretion and a
timer interrupt that stays pending is a hart that takes it again the instant
it returns. The implementation then had an early-out — "`armed` is already
zero, nothing to do" — which is wrong in exactly the case that matters:
**a timer interrupt clears `armed` itself**, so the disarm that follows it
returned without masking, the comparator kept a time that had passed, and
the hart took the same interrupt for ever.

It took a while to see because of a coincidence worth remembering: a
supervisor timer interrupt and a load access fault **both report cause 5**,
and only the interrupt bit tells them apart. A trace that printed
`scause & ~INTERRUPT` showed "5" and sent the investigation looking for a
faulting load that did not exist.

**Arming while the periodic clock still owned the comparator.** With
dynticks off, `sched_arm_timer` was still running on every return to user
mode, overwriting the periodic schedule's comparator and masking the
interrupt that drove it. A networking test hung every time. Hence the
early return above: a mechanism that is switched off must be *entirely* off.

### What is still wrong

With dynticks on, a Go program now gets through its startup, its goroutines,
its garbage collector and its contended-counter and channel checks — and
then **stops at a `time.Sleep`**. The sleeping thread's deadline is computed
and armed correctly (traced), so what is wrong is in the waking rather than
the arming. It is not yet understood.

So the default remains the periodic tick, `minikern.dynticks` is how the
remaining bug will be found, and `sched_missed_pokes` counts the times an
idle hart's backstop fired with work already runnable — a lost wakeup saved
by a timeout rather than by being right. **That number must be zero before
the clock can go.**

This is plausibly the same defect as the open four-hart race recorded in
`CLAUDE.md`: both are a kernel that makes progress because something keeps
nudging it, and both are invisible while it does.
