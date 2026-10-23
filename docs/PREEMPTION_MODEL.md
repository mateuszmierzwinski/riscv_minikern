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
| **An IPI costs 1499 ns** to send | poking a hart costs about a thousandth of a slice; it must buy something |

A warning about a fourth number, because it was briefly treated as a fact
about preemption and is not one: `context_switch` itself measures **46 ns**,
and that is the register swap alone — not the trap frame, not the
floating-point state, not the kernel lock, not `satp`, not the scheduler's
arming, and not what the interrupted thread loses in cache. The full path has
not been measured in isolation. What has been measured is the **consequence**
of avoiding it: on the board, removing 2,567 of 3,111 context switches from a
Go workload took its parallel phase from 35 ms to 24 ms (2.4x to **3.6x**),
which puts a real preemption in the microseconds. Reason with that, not with
the 46 ns.

So the expensive things are talking to firmware, talking to other harts, and
**interrupting a thread that did not need interrupting**. The model follows
from that.

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

Dynamic ticks are **on by default**; `minikern.periodic` puts the old clock
back, which is worth keeping for one purpose — telling "this is broken" from
"this is broken without a tick nudging it". Both of the timer bugs below were
identified that way.

- One-shot deadlines per hart, through SBI (`timer_arm_at`, `timer_disarm`),
  with **one function** owning the pair of "armed" and "the comparator"
  (`set_hart_timer`). Every path that fires, arms or disarms goes through it.
- No re-arm when the deadline is unchanged; 100 µs rounding.
- The scheduler computing its own next deadline from the slice, the sleepers
  and the beat (`sched_arm_timer`), at every return to user mode and every
  scheduling decision — and **nothing at all** when the periodic clock still
  owns the comparator.
- Slice expiry measured against the `time` CSR.
- Software interrupts between harts, and poking only harts that are idle or
  running untimed.
- **Housekeeping as a beat with no owner** (`timer_add_housekeeping_hook`):
  work that needs intervals rather than edges — a polled network device, a
  protocol's retransmit timers — on one hart per beat, preferring a hart that
  has stopped anyway.
- Counters that make the model checkable rather than believable:
  `timer_interrupts_of`, `timer_housekeeping_beats`, `sched_missed_pokes`
  and `sched_lost_deadlines`.

### The invariant, which is the important part

Every lost wake-up in this design is a **hang with no output**: if no hart
holds a comparator at or before the soonest deadline anyone waits for,
nothing will interrupt any hart ever again. So it is checked rather than
reasoned about. Whenever a hart is about to stop looking — entering `wfi`, or
running a thread with no timer — `check_deadline_is_armed` asserts that the
soonest deadline in the system (a sleeper's *or* the beat's) is armed
somewhere, and `sched_lost_deadlines` counts the failures.

It is reported and counted rather than panicked, because a kernel that stops
on the first one cannot show how often it happens or whether a fix moved the
number. **Both counters must read zero**, and they do.

## Measured

On a real VisionFive 2, the same Go workload, periodic against dynamic:

| | periodic | dynamic |
|---|---|---|
| Context switches | 3,111 | **544** |
| 8 units in parallel | 35 ms | **24 ms** |
| Speedup on 8 units | 2.4x | **3.6x** |
| Missed pokes / lost deadlines | 0 / 0 | 0 / 0 |

And the test the model exists to pass — **one busy thread for 15 seconds on
the board**, an embedded program that makes no syscall until it exits, so
every interrupt is one the scheduler chose to take:

| | hart running the thread | whole machine | pointless switches |
|---|---|---|---|
| Periodic | **1,504** interrupts | 6,028 | **1,501** |
| Dynamic | **3** | 467 | **1** |

Three interrupts in fifteen seconds is the Phase 1 target met on silicon, and
the 1,501 context switches it removes were preemptions of a thread with no
competition — work the periodic clock was doing for nobody.

The machine's 467 is the idle harts' backstop, ten a second each. With a
network up it would be the housekeeping beat instead, which is the honest cost
of a polled device and why the beat has no owner: pinned to one hart,
whichever thread landed there would have paid the beat out of its own slice.
Interrupt-driven networking removes the beat's reason to exist and is on the
roadmap; `BENCHMARKS.md` carries these numbers per publish so that a
regression shows up as a column rather than as a surprise.

## The fair class (Phase 2)

**Weighted virtual time.** Each thread has a weight from its nice value and a
virtual clock that advances at a rate inversely proportional to that weight —
so a heavy thread's clock runs slow. The scheduler picks the **smallest**
clock, which shares the processor in proportion to weight and cannot starve
anybody: a thread that is passed over keeps its clock while the others
advance, so it becomes the smallest eventually and unconditionally.

The arithmetic is pure and lives in `src/core/fair.c`, tested on the host,
because every mistake in it is a silent wrong answer that reads as "the
scheduler is unfair" rather than as a failure. Three were worth testing for:

- a virtual clock that **overflows** makes the heaviest thread the lightest
  (`ran_ns * 88761` overflows 64 bits after about two hours at the heaviest
  weight, so the multiplication is split across the division);
- a slice that **rounds to zero** spends the processor on switching, so the
  period stretches at a floor instead;
- a waking thread given its old clock **monopolises** the processor catching
  up, and one given the plain minimum gets a fresh claim every time it
  sleeps — so waking is worth a bounded credit and no more.

The nice-to-weight table is Linux's numbers as an **interface fact**, like
errno values: a program asking for nice -5 is asking for a share of the
processor, and a kernel with its own table would answer a different question.

Where each piece lives, because the three are easy to put in the wrong place:

| | |
|---|---|
| **The pick** | smallest virtual clock, with the old round-robin cursor kept as the tie-break — equal clocks are the common case, and breaking the tie by position rather than by table index keeps the previous behaviour instead of favouring low slots |
| **The charge** | on the way *out* of the switch, because how long a thread ran is only known once it has stopped, and that is the one point every exit path passes through — yielded, blocked, preempted or ended |
| **The slice** | the thread's share of a 12 ms period by weight, floored at the 1.5 ms Phase 0 settled; zero when nothing else is runnable, which is what dynamic ticks need |

`setpriority` and `getpriority` are implemented for `PRIO_PROCESS` and self
only. Another process's priority needs a permission model this kernel does
not have. `setpriority` **clamps rather than refuses**, as POSIX requires:
an error a caller probably does not check would leave it running at a
priority it did not ask for and does not know about. `getpriority` returns
the nice value rather than Linux's `20 - nice` — that bias exists so a libc
wrapper can tell -1 from an error, and it is the wrapper's job.

### Measured on the board

Two threads dividing **one hart** (`minikern.smp=off`), one at nice +5 and one
at nice -5, counting for three seconds each:

| | count | share |
|---|---|---|
| nice -5 (weight 3121) | 1,354,000,000 | 90.3% |
| nice +5 (weight 335) | 145,000,000 | 9.7% |

A ratio of **9.34**, where the weight table predicts **9.32** — 0.2% out. One
hart is not a limitation of the test but the point of it: with two runnable
threads on four harts both simply run, and no weighting is observable.

Getting that measurement cost two bugs that only the board showed, both in
the test rather than the kernel, and both worth keeping as lessons:

- **Two threads writing a label and then a number as separate calls
  interleave.** The board printed `light heavy 1354000000` and lost a count.
  The console is shared state and a line is the unit that must be atomic.
- **`exit` ends a thread; `exit_group` ends the process.** The light thread
  exited, the heavy one spun on, and the kernel waited for a program that
  would never finish — both counts printed and nothing followed them. The
  two syscall numbers are one apart.

## Affinity and isolation (Phase 4)

Two different things, and keeping them apart is most of the design:

- **Affinity** is a thread's own: which harts may run it, inherited at
  `clone`, set with `sched_setaffinity`. A thread asking to be somewhere.
- **Isolation** is the machine's: harts the scheduler places no work on at
  all, set once from the command line (`minikern.isolate=<mask>`). The
  operator reserving something.

Both are masks of **indices into the hart table, not hart ids** — the ids are
the firmware's and start at 1 on this board, so a mask of ids would be off by
one against every per-hart array in the kernel.

What a thread may actually be placed on is computed rather than stored (what
it asked for, minus what the machine reserved), so isolating a hart takes
effect on threads that already exist rather than only on new ones.

Three decisions worth stating:

- **`sched_setaffinity` refuses a mask that leaves nowhere to run**, as Linux
  does. Accepting it would produce a thread that is simply never chosen
  again — a hang that reads as the scheduler losing a thread rather than as a
  bad argument.
- **The thread moves immediately**, not at its next slice: if the calling
  hart is no longer allowed, the call yields. Otherwise a thread can pin
  itself away from the hart it is on and carry on running there, which makes
  the call look as though it did nothing.
- **An isolated hart still idles inside the scheduler** rather than being
  parked, so it can be given work again without being restarted. It
  therefore still shows timer ticks in the report (its idle backstop) while
  showing no context switches, which is exactly the distinction to look for.

`sched_getaffinity` reports the thread's own mask. It used to report "every
CPU the machine has", which was true before affinity existed and a lie
afterwards: a runtime that sets an affinity has to read back what it set, and
one that never sets it has to see the harts it can actually be placed on.

### Measured on the board

Isolating hart index 3 (`minikern.isolate=0x8`) while running the Go
program:

| | before | after |
|---|---|---|
| Hart 3 context switches | 131 | **0** |
| Hart 3 timer ticks | 161 | 98 (its idle backstop) |
| `NumCPU` the runtime sees | 4 | **3** |
| Concurrency results | correct | correct |

`NumCPU` dropping is the honest `getaffinity` showing through — the Go
runtime stopped claiming a hart it could not have.

Pinning a thread to hart index 2 (`minikern.pin=1`):

| hart index | 1 | 2 | 3 | 4 |
|---|---|---|---|---|
| context switches | 0 | **1** | 0 | 0 |

One switch, on the one hart asked for, and none anywhere else — with no
preemption, because a pinned thread with no competition is still a thread
with no competition.

## The real-time class (Phase 5)

**Priority order, absolutely.** A real-time thread runs in preference to
every fair thread and to every lower-priority real-time one, for as long as
it wants to. That is the whole promise of the class, and a kernel that mixed
it with proportional share would be promising something weaker.

One field carries both the class and the priority: `rt_priority` of 0 *is*
the fair class, 1..99 are real-time. A separate enum would make
"`SCHED_FIFO` at priority 0" expressible, and it is not a thing.

| | |
|---|---|
| `SCHED_FIFO` | no slice at all. A slice would be the proportional share the class exists to escape |
| `SCHED_RR` | a 10 ms quantum, but **only against another thread of its own priority** — being preempted for a lower-priority one would invert the order the class promises |
| Leaving the class | the virtual clock is **placed**, not restored: a thread that has been real-time has not been accumulating a clock anybody can compare against, so returning its old one would make it immortal or instantly starved depending on how long it was away |

**A waking real-time thread evicts a lower-priority one immediately**, which
is what makes the class real-time rather than merely preferred: the latency
that matters is from the wake to the first instruction, and waiting for the
running thread's slice would put a fair thread's 1.5 ms in front of it. Every
hart running something it outranks is marked and poked — marked rather than
switched, because a thread cannot be taken off a hart from another hart, so
the preemption happens where it always does, on the way back to user mode.

`sched_setscheduler`, `sched_getscheduler`, `sched_get_priority_max` and
`sched_get_priority_min` are implemented for self only, with the 1..99 range
POSIX defines — an interface fact, not this kernel's choice. `SCHED_OTHER`
must carry priority 0, which is what distinguishes "put me back in the fair
class" from "put me in the fair class at priority 40", a request with no
meaning.

### Measured on the board

A `SCHED_FIFO` thread at priority 50 against a fair thread, both doing the
same fixed work on **one hart** (`minikern.smp=off`), each printing one byte
when it finishes. The console read **`rf`**: real-time first, fair second.

The first attempt printed **`r` and then nothing at all**, and that is worth
keeping rather than tidying away. After printing, the real-time thread went
on spinning — at real-time priority — and the fair thread was never going to
be chosen again. It was not a bug in the scheduler; it was the scheduler
working, and the hazard the class carries. A runaway real-time thread starves
everything else, which is why setting the class is privileged in every system
that has one. The test now hands the processor back before waiting, which
also exercises the way out of the class.

## Frequency scaling (Phase 3)

This was recorded as blocked — "needs an I²C driver and a clock driver that
do not exist" — and the board's own device tree says otherwise. Its four
operating points are:

| | |
|---|---|
| 375 MHz | 800 mV |
| 500 MHz | 800 mV |
| 750 MHz | 800 mV |
| 1500 MHz | 1040 mV |

**Three of the four share a voltage**, and the four frequencies are
1500 ÷ {1, 2, 3, 4}. The system clock generator's word at index 1 is that
divider. So the frequency half needs one register and no PMIC at all, and the
"blocked" was a conclusion drawn before looking.

**Why no voltage control is needed to do this safely.** The unsafe order is
raising frequency before voltage; this only ever divides a clock whose
voltage was set by the firmware for the fastest point. Running at 375 MHz on
1.04 V is wasteful and never unsafe, and returning to 1500 MHz is safe
because the voltage was never lowered. That is also precisely the limit:
this delivers the **latency and thermal** half of DVFS and not the power
half, because most of the saving at a low point comes from dropping to
800 mV — which needs the AXP15060 over I²C, and neither driver exists.
Stated rather than implied, in `ROADMAP.md` too.

### Measured on the board

Each point set through the driver, with a loop timed against the fixed 4 MHz
reference — so the ratio is the frequency ratio, and a point that reported
the right number without changing anything would show up as an unchanged
rate:

| asked | set | divider | loop rate | of the top |
|---|---|---|---|---|
| 1500000 kHz | 1500000 | 1 | 165,803,108/s | 100% |
| 750000 kHz | 750000 | 2 | 82,902,413/s | **50%** |
| 500000 kHz | 500000 | 3 | 55,268,466/s | **33%** |
| 375000 kHz | 375000 | 4 | 41,451,421/s | **25%** |

Confirmed first as raw register writes before any driver existed, one board
run per divider: ratios of **1.99, 2.99 and 3.99**. The reference clock does
not move, which is what makes those numbers evidence rather than agreement.

### What Phase 3 found in this tree

`src/core/cpufreq.c` — the governor policy, carried over from the ARM
project — **was in no build target at all.** It was compiled nowhere and
tested nowhere, so the policy this kernel claimed to have did not exist as
far as the build was concerned. It is built and host-tested now, and it
turned out to be correct: ten groups covering the table's validity, the
critical and throttle steps, the load steps, the ends of the table, the
hysteresis that stops it oscillating, and an impossible sensor reading being
treated as the worst case. That last one is the ARM project's own lesson as a
test — a governor that believes a reading of -264 °C climbs to the top of the
table with nothing watching the temperature.

Four other carried-over files are in the same position and are **not** fixed
here: `caldate.c`, `gpio.c`, `nbd.c` and `tar.c` are in the tree and in no
target. Recorded rather than quietly tidied, because "we have this" and "this
compiles" are different claims and the gap between them is where that
governor sat.

## What is not built

- **Lazy floating point.** State is saved and restored unconditionally
  rather than only when `sstatus.FS` says it is dirty. This is now the
  largest known item on the preemption path.
- **Skipping `satp` on a same-address-space switch.** The scheduler checks
  whether the address space changed before adopting one, so the common case
  already avoids the write; the switch path itself does not distinguish them.
- **Targeted TLB shootdown.** A removal still broadcasts to every hart
  rather than to the harts that have run that address space.
- **`SIGSTOP` and `SIGCONT`.** Neither is implemented, so a process cannot
  be stopped and resumed.
- **A measurement of the full preemption path in isolation** — see the
  warning above.

## The two bugs turning this on found

**A disarm that skipped the mask.** Disarming is spelled as clearing the
timer's enable bit rather than setting the comparator far away, because an
out-of-range comparator value is at the implementation's discretion and a
timer interrupt that stays pending is a hart that takes it again the instant
it returns. The implementation then had an early-out — "already disarmed,
nothing to do" — which is wrong in exactly the case that matters: **a timer
interrupt clears that record itself**, so the disarm that followed one
returned without masking, the comparator kept a time that had passed, and
the hart took the same interrupt for ever.

It took longer than it should have because of a coincidence worth
remembering: a supervisor timer interrupt and a load access fault **both
report cause 5**, and only the interrupt bit tells them apart. A trace that
printed `scause & ~INTERRUPT` showed "5" and sent the investigation looking
for a faulting load that did not exist.

The fix is not the mask — it is that `set_hart_timer` is now the only place
that changes either record, unconditionally. The question "is it already in
that state?" is the question that got this wrong.

**Arming while something else owned the comparator**, twice, in both
directions. First `sched_arm_timer` ran on every return to user mode even
with dynticks off, overwriting the periodic schedule's comparator and masking
the interrupt that drove it — a networking test hung every time. Then the
beat armed from the periodic path, which would have done the same thing in
reverse. One owner at a time, and a mechanism that is switched off must be
*entirely* off.

## What the beat cost to get right

Two designs were wrong first, and both are recorded in `timer.c` because the
second one is the kind of mistake that looks like a design:

- **Armed from the scheduler.** `sched_arm_timer` runs on returns to user
  mode and at scheduling decisions, so a kernel doing its own network work
  with *no user process at all* never armed it. The kernel's final idle loop
  is not the scheduler, and once the scheduler had taken the comparator,
  nothing armed that hart again. The symptom was precise: a DHCP lease was
  obtained — that path polls in a loop of its own — and then a ping got no
  answer.
- **Pinned to one hart.** A hart that owns the beat and then picks up a
  compute-bound thread charges that thread a hundred interrupts a second and
  runs the polling inside them, on its slice, attributed to nobody. Measured
  above: 1,001 interrupts against 11.
