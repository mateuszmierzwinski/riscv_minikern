# Phase 0 — what this machine is

Measured on a real VisionFive 2 (JH7110, 8 GiB), `minikern.probe=1`. Every
number below came off the board; nothing here is from a datasheet, and the
two things that cannot be measured yet say so.

## The table

| Fact | Measured | Why it matters |
|---|---|---|
| Harts | **5 in the tree, 4 schedulable** | hart 0 is the S7 and has no usable MMU. Harts 1–4 are the U74s. A scheduler must take its hart set from the tree, not from a count |
| **Sstc** (`stimecmp`) | **absent** | there is no per-hart timer CSR. **Every timer arm is an SBI call** |
| `sbi_set_timer` | **335 ns** | the price of arming. Re-arming on every scheduling decision would cost more than the decision |
| **`satp` ASID bits** | **0** | the hardware implements no address-space ids, so **every** switch between address spaces flushes the whole TLB |
| `sfence.vma` (all) | **32 ns** | the fence itself is cheap — it is not what a flush costs |
| TLB refill, 64 pages | **296 ns** | this is what a flush costs: ten times the fence, paid by the next thread as it faults its working set back in |
| **Context switch, same address space** | **46 ns** | registers and a stack pointer, nothing more |
| SBI IPI, send | **1499 ns** | **four and a half times a timer arm, and thirty times a context switch** |
| `time` CSR | **4 MHz**, 249 ns a tick | the clock everything is measured against. Coarse: a 100 µs rounding is 400 ticks |
| Operating points | **375 / 500 / 750 MHz at 0.80 V, 1500 MHz at 1.04 V** | read from the board's own `/opp-table-0`. Three of the four share a voltage |

## What it sets

**Switching address spaces costs about 330 ns more than switching threads**
(46 + 32 + 296). That is the whole no-ASID penalty, and it is small: against
a 1.5 ms slice it is 0.02%. The plan's fallback of raising `min_gran` to 3 ms
when there is no ASID is **not needed on this chip** — the measurement says
1.5 ms costs two parts in ten thousand. Keeping the slice short is worth more
than avoiding a flush.

**An IPI is expensive — 1.5 µs, thirty context switches.** "Send a wakeup IPI
only if the target is in `wfi` or should be preempted" is therefore not an
optimisation but a requirement: a scheduler that pokes a hart on every wakeup
would spend more time sending interrupts than running threads.

**Arming is expensive too — 335 ns, seven context switches.** With no Sstc
there is no cheap arm, so "do not re-arm if the deadline has not changed" is
load-bearing. A single runnable task must end up with **no** timer at all,
which is what makes the Phase 1 test (`~0 timer interrupts in 10 s`) a real
check rather than a nicety.

**The `time` CSR's 249 ns resolution** is comfortable against a 100 µs
deadline rounding (400 ticks) and hopeless for measuring anything smaller —
which is why the costs above are averaged over two thousand repetitions
rather than timed once.

Derived starting values, to be tuned by the Phase 2 tests:

| Setting | Value | From |
|---|---|---|
| `target_latency` | 12 ms | the plan's starting point; nothing measured contradicts it |
| `min_gran` | **1.5 ms** | kept rather than raised: the no-ASID penalty is 0.02% of it |
| Timer rounding | 100 µs | 400 ticks of the 4 MHz clock |
| DVFS rate limit | **unknown** | needs the PLL switch and PMIC ramp times, which need drivers that do not exist |

## What could not be measured, and why

- **PMIC voltage ramp** — the AXP15060 is on I²C and there is no I²C driver.
- **PLL relock time** — there is no JH7110 clock driver to switch with.
- **Whether the CPU clock is cluster-wide** — the tree points all four U74s
  at one clock, which says it is; confirming it by changing the frequency
  and watching the others needs that same clock driver.
- **Whether the `time` CSR is independent of the CPU clock** — the same.
  It is read from `timebase-frequency` and is 4 MHz while the cores are at
  1.5 GHz, which is consistent with independence but does not prove it.
- **IPI *latency*** — only the cost of *sending* is reported. Measuring
  arrival needs a software-interrupt handler on the far hart, which this
  kernel does not have yet; the scheduler will add one.
