# Benchmark history

One row per publish. The point is not any single number — it is the **column**:
a regression that arrives gradually is invisible in a commit message and
obvious in a table.

**Real silicon only.** Every number in this table was read off a running
VisionFive 2. Emulator figures do not belong here at any strength — not as a
row, not as a comparison, not as a placeholder while a board run is pending —
because an emulator agrees with whatever you believed, which is this project's
most expensive recurring lesson (`CLAUDE.md` §5).

A number that was not measured for a row says **not measured**. Nothing is
carried forward from the row above, and nothing is derived by arithmetic: a
stale or computed number that looks like a reading is worse than a gap.

## The table

| Published | Monorepo commit | Go 8-unit speedup (median, range, n) | One busy thread: interrupts (busy hart / machine) over duration, net | NVMe KiB/s | SD KiB/s | missed pokes | lost deadlines |
|---|---|---|---|---|---|---|---|
| 2026-10-22 `3aa6326` | `9d47f2c25` | 3.3x (n=1) | not measured | ~326000 | ~8200 | n/a | n/a |
| 2026-10-22 `e093c4d` | `7d2012029` | not measured | not measured | not measured | not measured | n/a | n/a |
| *unpublished* | `efa74d7ef` | **3.5x** (1.9–3.6x, n=5) | **3 / 467 over 15.0 s, net off** | **332670** | **8043** | **0** | **0** |
| *unpublished* | `faa4b6253`+ | **3.7x** (2.6–3.7x, n=3) | not re-measured | not re-measured | not re-measured | **0** | **0** |

Notes per row, because a number without its circumstances is a rumour:

- **`3aa6326`** — the first board publish. The speedup is one run, so it has no
  range; treat it as an existence proof rather than a measurement. Interrupts
  were never counted at this commit: the periodic tick's rate could be worked
  out from 100 Hz and four harts, and that would be arithmetic rather than a
  reading, so the cell says what is true. `n/a` in the last two columns means
  the counters did not exist yet — dynamic ticks did not either. The storage
  figures carry **`~`** because they were recorded at the time in MB/s and
  converted here; the conversion is the kind of derivation this file otherwise
  forbids, so it is marked rather than presented as a reading.
- **`e093c4d`** — a publish of correctness work (fork with copy-on-write, two
  memory-corruption races, the VMA tree, XFS defaults). No board benchmark run
  was taken, and the row says so rather than repeating the one above.
- **`3e70c76e1`+** — dynamic ticks on by default. The speedup's low end (1.9x)
  is real run-to-run variance on four U74s, not a bad build; five runs is
  enough to see that the median moved and not enough to call the spread. The
  interrupt figures are the embedded spin program, **raw counts with the
  duration they were counted over**, against **1504 / 6028 over 15.0 s** for
  the same program with `minikern.periodic=1` on the same board.

  Storage *was* re-measured, and the reason is worth keeping: the honest
  objection to "storage was untouched, so it was not re-measured" is that SD
  at 8.2 MB/s might have been **capped by the 100 Hz tick** rather than by the
  card, and the in-kernel benchmark being immune to that is a claim too, not a
  fact. Two commands settled it — both figures are unchanged, so the tick was
  not the limit and the SD ceiling really is the word-at-a-time FIFO.

- **`faa4b6253`+** — the fair class (Phase 2): weighted virtual time, nice
  values, per-thread slices. The speedup column moved **up**, which was not
  the point of the change and is worth not over-reading at n=3; what matters
  is that proportional share cost nothing measurable. Its own measurement is
  not in this table because it is a *ratio* rather than a rate: two threads
  dividing one hart at nice -5 and +5 counted 1,354,000,000 against
  145,000,000, a ratio of **9.34** where the weight table predicts **9.32**.
  `PREEMPTION_MODEL.md` carries it.

## A column this table needs: the nice ratio

The fair class's promise is a *proportion*, and nothing above would notice it
breaking — a scheduler that ignored nice entirely would leave every column
here unchanged. `minikern.nice=1 minikern.smp=off` prints the two counts, and
their ratio against the weight table's prediction belongs in this history as
soon as there is a second data point to compare.

## What each column means, and how to get it

Stage the board image first — `minikern_board`, not `minikern`, which is the
QEMU one — and serve it where U-Boot fetches it:

```bash
bazel build //:minikern_board
cp "$(bazel cquery --output=files //:minikern_board)" ~/vf2/minikern.bin
```

**Go 8-unit speedup** — the stock Go binary on the SD card prints it. Run it at
least five times: the parallel phase varies by more than any change short of a
regression.

```bash
python3 run.py "console=sbi minikern.uart=sbi minikern.disk=sd minikern.init=1" 110 go.log
grep -aoE '8 units serially [0-9]+ms, in parallel [0-9]+ms, speedup [0-9.]+x' go.log
```

Report the **median and the range**, with *n*. A single run is not a
measurement — the five behind the current row were 1.9x, 3.4x, 3.5x, 3.5x and
3.6x, and any one of them alone would have been misleading.

**Timer interrupts for one busy thread** — the embedded spin program, which
makes no syscalls at all until it exits, so every interrupt is one the
scheduler chose. Both figures come from `smp: switches/ticks by hart`: the hart
that ran the thread, and the sum. Scale to 10 s from the reported duration.

```bash
python3 run.py "console=sbi minikern.uart=sbi minikern.spin=1" 110 spin.log
grep -aE 'spin: [0-9]+ ms|switches/ticks|missed pokes' spin.log
```

The baseline to compare against is the same run with `minikern.periodic=1`,
which puts the old 100 Hz clock back.

**NVMe and SD throughput** — the in-kernel benchmark, which exists because both
drivers were slow for reasons invisible in the code. Quote the large-block
figure and say which block size:

```bash
python3 run.py "console=sbi minikern.uart=sbi minikern.disk=nvme minikern.diskbench=64:16" 110 nvme.log
python3 run.py "console=sbi minikern.uart=sbi minikern.disk=sd   minikern.diskbench=16:16" 110 sd.log
```

**missed pokes / lost deadlines** — `sched: N missed pokes, M lost deadlines`,
printed at the end of any run that ran a program. These are not performance
numbers and they are in the table because they are the ones that must never
move:

- a **missed poke** is a wake-up that a timeout rescued rather than
  correctness — the idle backstop fired with work already waiting;
- a **lost deadline** is a wake-up nothing would have rescued — no hart held a
  comparator at or before the soonest deadline anyone was waiting for, which
  is a hang with no output.

**Both must read zero.** A row with a non-zero value in either column is a bug
report, not a benchmark, and the commit that produced it should not have been
published. They are deliberately in the same table as the speedups, because the
temptation a dynamic-tick scheduler creates is to buy a number in one column
with a silent failure in another.

## The rule for adding a row

A row is added from a **board run of the published commit**, not from an
emulator run, not from memory, and not from the previous row. If a column was
not measured, it says so. If a number got worse, the row still goes in and the
note says what changed — the history is only worth keeping if it records the
regressions as readily as the wins.

**Raw readings, in the units the kernel prints.** No scaling, no unit
conversion, no per-second normalisation: a cell holds what was on the console,
and where a count needs a duration to mean anything, the duration is in the
cell beside it. An earlier version of this file scaled a 15-second interrupt
count down to 10 seconds, which is exactly the arithmetic the rule above
forbids — and a reader who spots the document breaking its own rule is right
to distrust the rest of it. Where a figure is unavoidably derived (the first
row's storage numbers, converted from MB/s), it is marked `~` and the note
says so.

**Never compare rows across configurations.** The interrupt column is
meaningless without knowing whether a network was up: with no network a busy
thread costs 3 interrupts over 15 s, and with one the housekeeping beat runs
at 10 ms and the machine pays around 1,200 over 10 s. Those are different
rows of reality, not a 400x regression, so the cell names the configuration.

## Columns this table should have and does not

Stated rather than quietly missing, because the gap is the point of a history:

- **A syscall-heavy benchmark.** Nothing here exercises the big kernel lock,
  which is one hart in the kernel at a time and the ceiling on anything
  syscall-bound (`ROADMAP.md` §8). The speedup column uses compute-bound work
  and would not notice that ceiling moving.
- **`time.Sleep(50 ms)` accuracy.** The kernel already reports how late each
  wake-up was (`sched: N sleeps, woken X us late on average, Y us at worst`),
  which is the number a dynamic-tick scheduler is most likely to make worse
  while improving every other column. It belongs here.
- **A Linux baseline on the same board.** Every number above is this kernel
  against itself, so none of them says whether 8043 KiB/s off an SD card or
  3.5x on eight units is good. A Linux row would. It needs the operator, since
  it means a bootable Linux on the card.
