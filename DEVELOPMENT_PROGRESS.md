# Development progress — minikern on RISC-V (JH7110 / VisionFive 2)

The plan and the record, in one file so that what was done and what is left
cannot drift apart. `[x]` means tested, `[~]` means partly done with the
remainder named, and **nothing is marked done because it was written** — only
because something proves it. What the kernel can do today is summarised in
`FEATURES.md`; the design and the measured hardware facts are in `GOALS.md`.

The order is the same discipline the ARM project used and it is the reason
that one got as far as it did: **each phase ends with something that boots and
is tested.** Nothing is "written now, wired up later".

---

## What this port inherits

`src/core/` and `src/lib/` carry over because they depend on no architecture:
9,300 lines across 61 files, with zero includes from `arch/` or `board/`. That
is the network stack, TCP, XFS, NBD, the buffer cache, the allocators, the
VMA tree, the ELF loader, the TAR reader, the device-tree reader, SHA-256/512,
Ed25519 and CRC-32C — and their host tests, which is why this repository has
real coverage from its first commit.

Three things in it are *not* architecture-neutral and were fixed on the way in:

- a physical address was `uint32_t`, which is correct on a 512 MiB board and
  overflows in the middle of an 8 GiB one (`core/types.h` now has `phys_t`);
- `fdt_prop_cells` **refused** a two-cell value whose high word was set, so an
  8 GiB VisionFive 2's own memory node would have been rejected;
- `bootinfo` understood two boot protocols and had to tell them apart. The
  ATAG half is deleted: on RISC-V the firmware always passes a device tree.

---

## P-opt — Built for the JH7110

- [x] **The core, not just the ISA (2026-10-22).** `-mcpu=sifive-u74` rather
      than a bare `-march=rv64gc`. Naming the core tells the compiler two
      things an ISA string cannot: the instruction set those cores implement
      (which is the same RV64GC) **and the pipeline**, which is what
      instruction scheduling depends on — the U74 is dual-issue in-order, so
      the order of independent instructions is worth something, where on an
      out-of-order core it would not be.
      The ISA is left exactly as the core defines it. The temptation is to add
      extensions — bit manipulation, vectors — but a JH7110 has neither, and
      an image built with them does not fail helpfully: it takes an illegal
      instruction in the middle of whatever used them.

## P0 — Boot

- [x] **P0.1 S-mode entry, SBI console, device tree, power-off (2026-10-22).**
      The firmware (OpenSBI, which QEMU bundles; U-Boot's SBI on the board)
      loads the ELF at `0x80200000`, enters it in S-mode with the hart id in
      `a0` and a device tree in `a1`. The kernel prints through SBI's console —
      a trap per character, which is right for a banner and wrong for a
      terminal — reads the tree, reports what the machine has, and asks the
      firmware to power off. `boot_qemu_test` passes when QEMU exits **by
      itself**, which it does.
      *Two facts measured rather than assumed, and both matter for the board:*
      **the boot hart is not hart 0.** QEMU's `virt` picks one by lottery (seen
      0, 1 and 2 across runs), and `sifive_u` — which models a monitor core
      beside its application cores, exactly as the JH7110 pairs an S7 with four
      U74s — boots on hart **1**. Anything that assumes it is hart 0, or that
      the others are `1..n`, is wrong on this SoC. And **SBI's power-off
      works**, where PSCI's on the ARM board was refused by U-Boot, so a boot
      test can wait for an exit instead of grepping for a line and killing the
      emulator.
- [x] **P0.2 The kernel's own UART, found in the device tree (2026-10-22).**
      It binds by **what the device says it is**, not by where it is:
      `fdt_find_compatible` scans for `snps,dw-apb-uart` (what the JH7110's
      tree says) or `ns16550a` (what an emulator says), reads the node's `reg`
      honouring two address and two size cells, maps the registers, and takes
      the console over from SBI. On `virt` it finds `/soc/serial@10000000`, and
      every line after that one is printed by the kernel's own driver.
      **This is the pattern every later driver here uses** — GPIO, the SD host
      controller and NVMe all begin with "find the node that says it is one of
      these" — so it is tested on the host in its own right: either compatible
      string finds the node, a *prefix* of one does not (or a driver binds to a
      part it does not understand), a path that does not fit is **refused
      rather than truncated** because half a path names a different node, and a
      device above 4 GiB is read correctly, which is where a PCIe window lives.
      Two deliberate choices: the console is **polled**, because a console that
      needs interrupts cannot report a fault in the interrupt controller; and
      the **baud rate is left exactly as the firmware set it**, because a
      kernel that recomputes a divisor from a clock it has guessed at goes
      silent at the moment it would have told you why.
      `mmu_map_device` maps registers in 4 KiB pages even where a larger one
      would fit: device registers sit next to other devices' registers, and a
      2 MiB mapping of a 4 KiB UART makes a megabyte of other peripherals
      reachable — which on some parts is a stray write away from reprogrammed.
- [x] **P0.3 Trap entry and exceptions (2026-10-22).** One `stvec`, one
      `scause`, one `sepc`, one `stval`: a single vector, a frame saved on the
      stack already in use, and a dispatch on the cause — against ARM's eight
      vectors, banked registers per mode, and a different stack depending on
      which mode you came from. The frame's layout lives in one header shared
      by the assembly and the C, with static assertions that the two agree,
      because "the offsets drifted" is a bug with no symptom until it has a
      catastrophic one.
      `minikern.selftest=illegal|load|store|break|all` takes each fault on
      purpose and survives it, checking the **cause and the faulting address**
      against what it asked for — and failing if the instruction does *not*
      fault, which is the more dangerous outcome. The illegal-instruction case
      is the M-mode CSR read that killed this kernel's first spike, so the
      trap path is proved against the real mistake rather than a synthetic one.
      **The bug it found immediately:** stepping over a faulting instruction
      assumed 4 bytes. `rv64gc` includes the compressed extension, so an
      instruction is **2 or 4 bytes** — `csrr` and `sd zero` happen to be 4
      (x0 is not a register `c.sd` can name), while `c.ld` and `c.ebreak` are
      2, so resuming landed *inside* the next instruction and raised an
      illegal instruction of its own. The symptom was a second unexplained
      fault in a later test; the cause was arithmetic. The length is now read
      from the instruction itself, which is the architectural rule. Running
      each case in its own boot is what made it findable at all.
- [~] **P0.4 Sv39 paging — on, with the kernel identity-mapped (2026-10-22).**
      Three levels, 4 KiB pages, 2 MiB megapages and 1 GiB gigapages, written
      as **pure arithmetic that touches no CSR** and host-tested in ten cases
      before anything installed a table: canonical-address checking (bits
      63:39 must copy bit 38, and the 255 TiB hole between the halves is not
      an address), large mappings and their alignment rules, refusing to
      replace a live mapping, unmap reporting what it removed, and running out
      of table memory being reported rather than ignored. That separation is
      what the ARM project's page-table test earned its place by.
      `mmu.c` then builds the kernel's map and writes `satp`. The map is an
      **identity** map, because the instruction after `csrw satp` is fetched
      through the table just installed — if the kernel's own code is not at
      the address it is executing from, there is no handler to say so and the
      machine simply stops fetching. Text is mapped read-execute and
      everything else read-write, so the kernel's code cannot be written and
      its data cannot be executed, and `minikern.selftest=rotext` **proves the
      text protection by trying to write to it** rather than asserting it.
      A 1 GiB identity map costs **3 table pages**, because gigapage leaves
      need nothing below them.
      *The bug it surfaced immediately:* the fault self-tests expected *access*
      faults for unreachable addresses, which is right with translation off and
      wrong with it on — an unmapped address is a **page** fault. The
      distinction is a real diagnosis (nothing answered on the bus, versus
      nothing in the table), so the tests now ask the MMU which world they are
      in rather than hard-coding one.
      **What remains:** moving the kernel to the top half, which needs the
      image linked high and a jump, and per-process address spaces (P1.2).
- [ ] **P0.5 The real boot path on the board.** U-Boot on the VisionFive 2
      loads a flat image with `booti`; an SD card image with a `boot.scr`, and
      the documented flash procedure in `README.md`. The ARM project's
      `mksdcard.sh` is the template, minus `mkimage`.

## P1 — Memory and processes

- [x] **P1.1 Physical and kernel allocators, over the real memory map
      (2026-10-22).** Both carried over and already host-tested; what is new
      is 64-bit physical addresses and the map they are given.
      **Every frame starts out used** and the boot path releases what is
      really free, so a reservation nobody wrote loses memory rather than
      handing out memory in use — the only direction this mistake may fail in.
      Reserved: the kernel image, the page-table arena, and the device tree
      the firmware left in RAM, which the command line still points into.
      The bitmap lives in `.bss`, not in the memory it describes, so there is
      no moment where the allocator is allocating the storage it needs in
      order to exist. It is sized for **8 GiB** (2 Mi frames, 256 KiB of
      bitmap) so one image runs on every VisionFive 2.
      Proved on both configurations, and the big one is the point: with 8 GiB,
      2,096,535 of 2,097,152 frames come up free. That is the case the reader
      this kernel grew from **could not express at all** — it refused a
      two-cell value whose high word was set, so a board like this would have
      booted believing it had no memory. The frame allocator's own test now
      covers frames at 9 GiB, where a 32-bit address would wrap into an index
      that looks valid.
- [ ] **P1.2 User address spaces**, lazy faults, `mmap`/`munmap`/`mprotect`.
      The VMA tree carries over; the page-table backing is Sv39.
- [x] **P1.3a Interrupts and the tick (2026-10-22).** The PLIC, found in the
      tree, and SBI's timer — one absolute deadline per hart against the
      `time` CSR, firmware-mediated so it is identical on the emulator and the
      board, where the ARM kernel programmed a comparator itself per core and
      had to read `CNTFRQ` because the frequency differed between the two.
      **The part nobody can guess, and the reason this is written the way it
      is: a hart's PLIC *context* is a position in the controller's own
      `interrupts-extended` list, not its hart id.** On QEMU's `virt`, hart 0's
      supervisor context is **1**, because context 0 is its machine-mode
      context — the firmware's. A kernel that assumed they matched would
      enable another context's sources and wonder why nothing arrived. The
      context is resolved by walking that list and following each phandle to
      the hart it belongs to, which needed phandle resolution in the tree
      reader (one scan, shared with `fdt_find_compatible`).
      Claiming and completing are reads and writes of **one** register, so a
      handler cannot end an interrupt without also being ready for the next —
      which the GIC on the old architecture happily let you get wrong. A source
      with no handler is **disabled** after one complaint, because an
      unhandled interrupt re-asserts immediately and would otherwise starve
      the machine while printing about it.
      Proved by making the kernel wait for a known number of ticks and report
      the elapsed time *by the counter*: **50 ticks in 499 ms** at 100 Hz. An
      armed timer and a delivering one look identical in a boot log, and on the
      old architecture that difference was a real bug — a core whose controller
      had not been initialised ticked for ever without entering the kernel.
      The tick re-arms from **the deadline that passed, not from now**, so a
      slow handler costs latency rather than clock drift.
- [x] **P1.3b The scheduler (2026-10-22).** Threads, a run queue, and
      preemption on the tick. Written when it was, because a Go runtime is not
      optional about it: it starts a thread during initialisation and then
      waits on a futex for that thread to run, so a kernel with one thread
      does not run Go slowly — it deadlocks before `main`.
      **A kernel stack per thread**, which is the whole design. A thread
      blocked in the kernel has a kernel call stack that must survive while
      another thread runs on the same hart; one stack for all threads is the
      mistake this is built to avoid. Switching is `context_switch`: the
      callee-saved registers and the stack pointer, which is all a C call
      expects to survive, and nothing touches `satp` because threads share an
      address space.
      Two bugs worth recording. A new thread's first trap frame was laid out
      at the **base** of its kernel stack, and `uspace_enter_frame` records
      the trap stack as the frame's address plus its size — so the next trap
      had 288 bytes and then ran off the end into the previous allocation. It
      sits at the top now. And preemption did nothing at all for an hour: the
      tick hook was wired into `timer_on_tick` by a textual replacement that
      matched nothing (`timer.ticks++`, not `ticks++`), so the hook was set
      and never called, and one spinning thread starved every other one. A
      silent no-op is the worst kind of wiring error, and the only reason it
      was found is that the trace showed two threads alive and one running.

- [ ] **P1.4 Threads, context switch, and the kernel lock.** The ARM design
      holds: one lock around the kernel, taken on entry and released on exit,
      so that structures written for one core stay correct on five. The
      parallelism that matters is in user space.

## P2 — The syscall interface

- [x] **P2.0 User mode (2026-10-22).** A process with its own Sv39 address
      space, entered with `sret` — one instruction, where the architecture
      this migrated from needed a mode switch, a banked register set and a
      stack per mode. Proved with a page of hand-written instructions rather
      than an ELF, deliberately: a loader on top of a mode switch that has
      never been entered is two untested things at once, and on the ARM side
      the first bug found this way was in the process *start state*, which a
      loader has nothing to do with.
      The trap path now serves both origins through one vector: `sscratch`
      holds the kernel stack while user code runs and zero while the kernel
      runs, so a single swap at the top both finds the right stack and says
      where the trap came from. It is zeroed on entry, because a fault
      *inside* the handler must take the kernel path and not swap to a stack
      already in use.
      Getting back out needed a pair of functions, and the shape is worth
      naming: `sret` does not return, so `uspace_enter` saves the callee-saved
      registers and goes, and the exit syscall calls `uspace_leave`, which
      restores them so control reappears at the caller. The process did not
      return; the kernel resumed.
      **The bug, and it is a good one.** A trap does not change translation —
      the *process's* `satp` is still loaded when the kernel runs — so the
      handler's code, its stack **and the console's registers** must all be
      addressable from the process's address space. Sharing the kernel's
      top-level entries only where the process had none silently dropped the
      console: the console's registers are at `0x10000000` and a process's
      text at `0x10000`, which collide in root entry 0. The first `write`
      syscall then faulted inside the kernel *while printing*, and the fault
      handler faulted *while reporting that*, until the stack ran off the
      end — a hang with **no output at all**, diagnosed by asking the monitor
      for `scause`, `stval` and `satp`. Collisions are resolved one level
      down now, and one deeper than that is refused rather than papered over.
      All of which **disappears when the kernel moves to the top half**
      (P0.4's remainder): above the sign-extension hole it cannot share a
      root entry with user space at all.
      Tested for three things a weaker test would miss: the program **comes
      back** (it writes twice, with the kernel resuming it in between — a
      kernel that reaches user mode once and never returns looks identical in
      a log), the exit status *and* the syscall count both arrive, and a fault
      in user code **ends the process and not the kernel** — the second
      program reads the kernel's own text, which every address space maps and
      no user page marks reachable, and the machine carries on to power itself
      off.
      The syscall numbers are the generic set RISC-V uses, which is the modern
      one: `write` is 64 and `exit` 93, with no `mmap2`, no `_llseek`, no
      `stat64` and no `*_time64` duplicate of anything — the ARM kernel
      implemented all of those as separate entry points.

- [x] **P2.1 The generic syscall table (2026-10-22).** RISC-V uses
      `asm-generic/unistd`, the modern set: `mmap` with byte offsets rather
      than `mmap2` with page offsets, one `lseek`, one `fcntl`, one `stat`,
      one `futex`. Eight legacy ABI shapes the ARM kernel implemented simply
      do not exist here.
      **A table, not a `switch`** — and the reason is the log it makes: an
      unimplemented call is reported once, by number and by name, and that
      line is how the next thing a runtime needs gets found. The whole of
      the Go bring-up below was done that way, one line at a time, which is
      faster than reading a runtime's source and does not go out of date.
      Thirty-six calls so far: the console, memory (`brk`, `mmap`, `munmap`,
      `mprotect`, `madvise`), time, randomness, threads (`clone`, `futex`,
      `sched_yield`, `tgkill`), limits, the architecture's own
      `riscv_hwprobe`, and the netpoller's `epoll`/`eventfd` pair.
      Memory is where the care went. A reservation (`PROT_NONE`) is
      **recorded and not backed**, because a Go runtime reserves its arenas
      before using any of them and backing hundreds of gigabytes of address
      space with page tables would exhaust memory for nothing — the pages
      appear when the range is mapped again with real permissions. A hint is
      honoured when it is free and **ignored rather than refused** when it is
      not, which is what a runtime placing its heap by hint expects.
      `USER_TOP` is Sv39's whole user half, 256 GiB, because that is the
      limit the hardware imposes: a kernel offering only the bottom gigabyte
      makes every one of those hints fail.
- [x] **P2.2 ELF64 (2026-10-22).** A **rewrite**, not a widening: ELF64 is
      not ELF32 with bigger fields. The header's layout differs, and a program
      header's **flags move from the end to the front** — in ELF32 they follow
      the sizes, in ELF64 they sit immediately after the type. Patching
      offsets would have read a segment's permissions out of its alignment.
      What it refuses is the point, and is what the host test spends its time
      on: a 32-bit binary, one for another architecture, an object file, a
      shared library (there is no loader here to help it), a segment that runs
      past the end of the file, one that reaches into the kernel's half, an
      entry point outside user space, and an ELF32-sized program header. Each
      of those, trusted, is a fault somewhere later with no obvious cause.
- [x] **P2.3 The process start state (2026-10-22).** A Linux process does
      not begin with empty registers: it begins with a stack already holding
      `argc`, `argv`, the environment and an auxiliary vector, and a C runtime
      reads all of it before `main`. Every word is 64-bit here, which is most
      of why this is a separate implementation rather than a widening — a
      pointer-sized word is the unit the whole structure is counted in.
      No `AT_HWCAP` bit for thread-local storage, unlike the architecture this
      came from, where a C runtime checked `HWCAP_TLS` to decide whether to
      read the thread pointer from a register or from a helper page the kernel
      had to map. Here `tp` is in the ABI: nothing to advertise, no page to
      provide.
      **Tested from the outside**, which is the only way this can be tested
      honestly: `src/userland/init.c` is an ordinary static ELF64 built with
      the same toolchain, depending on nothing from the kernel, which walks
      the whole block and *reports what it found* — and the test asserts the
      program's account, not the kernel's. A loader can place every segment
      correctly and still start a program wrongly; on the ARM side that was
      the first bug found in user mode, and it was invisible until something
      actually read `argv`.
      **The bug it caught immediately: `AT_PHDR` was 0.** The user link script
      did not leave room for the ELF header and the program headers at the
      start of the first segment, so the header table was in the *file* but in
      no segment — there was no address at which those headers existed in
      memory, so there was nothing to report. A Go runtime reads `AT_PHDR` to
      find its own segments, so zero there is not a cosmetic gap.
      `SIZEOF_HEADERS` fixed it, and the test now asserts the real address.
      Also asserted: a 16-byte-aligned stack pointer at entry, which the ABI
      requires and a runtime relies on when it spills a 128-bit value; and
      that `AT_RANDOM`'s sixteen bytes are not all zero, which would be a
      kernel supplying the tag without the bytes.
- [x] **P2.3a A program off the filesystem (2026-10-22).** The whole machine
      in one test: firmware, S-mode, paging, an NVMe controller, a block-size
      adapter, a buffer cache, XFS, the ELF64 loader, a process address space
      and the start state — reading `/init` off the disk and running it to
      completion. This is the path a Go binary takes unchanged.
- [x] **P2.4 Go runs unchanged (2026-10-22).** **The project's goal, reached
      on RISC-V.** A stock gc Go binary — `go1.25.5`, built by the ordinary
      Go rules for `linux/riscv64`, with no build tags and no knowledge that
      this kernel exists — read off an XFS filesystem on an NVMe drive and
      run to completion:

      ```
      go: go1.25.5 on linux/riscv64
      go: GOMAXPROCS=1 NumCPU=1
      go: allocated 7 MiB total, 3 collections, 32 blocks kept
      go: sum of primes below 200000 is 1709600813
      go: 8 goroutines finished, first=21171191 last=37583560
      go: contended counter reached 16000
      go: channel values [1 4 9 16 25 36]
      go: slept for 51ms (at least 50ms: true)
      go: recovered from assignment to entry in nil map
      go: 1 goroutines at the end (1 at the start), total time 56ms
      ```

      The test asserts all of it, because "hello" would prove only a loader.
      The garbage collector means `mmap`/`munmap` over a heap that grows and
      shrinks; eight goroutines at once means `clone`; a mutex contended by
      all of them means futex waiting **and** waking; the 50 ms sleep means
      `nanosleep` and `clock_gettime` agree with each other and the hardware;
      the recovered panic means the runtime can walk its own stack. The test
      also fails on **any** unimplemented syscall, even a survivable one,
      because a test that lets one through hides the next.
      **The bug worth remembering.** `futex`'s wake count is the *third*
      argument — the same slot `FUTEX_WAIT` uses for the value it expects.
      Read from the wrong slot it finds 0, wakes nobody, and **reports
      success**: the runtime's `notewakeup` returned happily while the thread
      it meant to start slept for ever. The program deadlocked after printing
      nothing, and the only thing that could show it was a trace with a wake
      and a wait on the same address and no wake-up in between. Three other
      refusals had to be met first, each found from one log line: `fcntl` on
      the standard descriptors (a runtime checks all three before anything
      else and aborts with "cannot open standard fds"), `prlimit64` for the
      stack limit, and the netpoller's `epoll_create1`/`eventfd2` — which a
      kernel with no network stack still has to answer, because the first
      `time.Sleep` initialises the poller.
      Its `epoll_pwait` **honours the timeout** rather than returning "no
      events" instantly: a runtime whose idle threads are answered
      immediately spins a core at the speed of the syscall, and on one hart
      that starves every other thread.
- [ ] **P2.5 Signals**, including what Go's runtime actually needs. Not yet
      required: everything above runs without them, because `tgkill` is
      answered and a missed preemption signal is a slower runtime rather than
      a broken one. This is the one item with a known unknown: on ARM, Go set
      `SA_RESTORER` with a zero restorer and needed a kernel-provided
      trampoline page, which cost a day to discover. RISC-V does it
      differently and the difference has to be measured, not assumed.

## P3 — SMP, the JH7110's way

- [x] **P3.1 Harts up through SBI HSM (2026-10-22).** One firmware call per
      hart: `sbi_hart_start(hart, address, opaque)` enters it **in S-mode**,
      with translation off, at a physical address. The specification says
      S-mode, so the entire class of bug that made the ARM bring-up hard —
      PSCI starting a secondary in the mode the *primary* was entered in, so
      a core that skipped the HYP→SVC drop wrote the hypervisor's MMU
      registers and died on its next instruction fetch while PSCI reported
      success — cannot happen here.
      **The harts come from the device tree**, found by the generic `riscv`
      entry of their `compatible` list, with each one's `mmu-type` read and a
      hart that cannot run Sv39 skipped rather than started and lost. The ids
      are not positions: measured across runs, QEMU's `virt` booted on hart
      0, 1, 2 and 3 on different runs of the same image, and the JH7110 pairs
      an S7 at hart 0 with four U74s at 1-4.
      That the entry address is physical is only convenient because the
      kernel is still identity-mapped. When it moves to the top half (P0.4)
      this entry must stay reachable physically, which is the one piece of
      that move that cannot be deferred.
- [x] **P3.2 Threads on every hart (2026-10-22).** One lock around the
      kernel (`kernel/klock.h`), which preserves exactly the property the
      single-hart design relied on — one hart inside the kernel at a time —
      without auditing every structure first. The parallelism it buys is in
      **user space**, which is where a program spends its time. It is a
      ticket lock, because a test-and-set lock lets the hart with the warmest
      cache win repeatedly and starves the others.
      The scheduler's state became **per hart**: `current` was one variable,
      and two harts would each have believed they owned the same thread. Each
      hart runs the same loop — claim a runnable thread, run it, repeat — and
      a thread gives the hart back by switching to the loop rather than
      picking its own successor, which it could otherwise pick from another
      hart's. The first thread stopped being special: it used to be the stack
      `uspace_run` was called on, so it was the only thread another hart
      could not run.
      Per-hart registers had to follow: `satp` and `sstatus.SUM` are set by
      every hart that runs a thread of the process, and the timer comparator
      is per-hart too — a shared deadline had whichever hart ticked first
      advance it for everyone, so the others armed a deadline already in the
      past and took interrupts continuously.
      **Measured**: `GOMAXPROCS=4 NumCPU=4` from `sched_getaffinity`, every
      hart running threads, and 8 units of work taking 1.6x less time in
      parallel than serially — the same figure the ARM project got. The
      emulator is the limit there: without `-accel tcg,thread=multi` QEMU
      round-robins every vCPU onto one host thread and the ratio is 1.1x.
      Four bugs, each worth keeping:
      **A trap's "did this come from user mode?" test used a register that by
      then held the interrupted stack pointer**, which is non-zero either
      way — so the kernel reloaded `tp` (the hart id) from a frame slot that
      kernel traps never write. The hart then indexed every per-hart array at
      0: its ticks were counted in another hart's slot and its timer rearmed
      through that slot, so it stopped ticking after one interrupt. Invisible
      whenever the boot hart happened to be hart 0, which is most runs, so it
      presented as a test failing one time in three. The test is the saved
      `sstatus`'s SPP bit now, the same bit the exit path uses.
      **Destroying a process freed the kernel's own page tables.** Sharing
      the kernel's mappings copies its level-1 *entries* into a process's own
      table wherever the roots collide, and the teardown cleared only the
      root-level sharing — so the walk descended through those copies and
      freed the kernel's second level. For one process nothing happened: the
      pages were still mapped and still intact. The SMP work added a 16 KiB
      kernel stack per thread, that allocation landed on them, and the
      kernel's mappings changed underneath it — so loading the next process's
      `satp` faulted at an address with no console behind it and the machine
      stopped silently. The teardown is now the exact mirror of the sharing,
      at both levels, and all of it disappears when the kernel moves to the
      top half.
      **A port installed late is a port that was not installed.** The kernel
      lock was wired into the trap path after the first program had already
      run, so every syscall took the "no lock configured" path while the
      scheduler still released one — and the ticket lock's two counters
      drifted apart until the next hart to ask waited for a ticket that had
      already been served. It is wired in the composition root now, before
      anything can trap, and the counters are printed between processes when
      tracing, because a lock whose numbers have diverged will stop the
      machine later and at a place that says nothing about why.
      **A sleep that spins is a sleep that starves.** `nanosleep` and an idle
      runtime's `epoll_pwait` both looped on the clock calling
      `sched_yield_now`, which on four harts produced four and a half million
      thread switches for a program that ran 56 milliseconds — correct, and
      almost all of it the scheduler talking to itself. A blocked thread with
      a deadline costs one switch; the same run now does 37.


- [ ] **P3.9 Per-hart state**, with the lesson from the ARM port written down
      first: anything a core caches *about its own registers* must be
      invalidated by whatever sets those registers from nothing, or a core
      that stops and restarts runs with a stale idea of what it has installed.
- [ ] **P3.3 Idle harts stop through SBI.** `hart_stop`/`hart_start` is the
      same power saving the ARM kernel got from PSCI `CPU_OFF`, and the same
      invariant applies: the boot hart never stops, so a wake that races a
      stop is always retried by something still ticking.

## P4 — Storage, which is what this board is for

- [x] **P4.1 virtio-blk: a real disk (2026-10-22).** The modern MMIO
      transport (version 2) and a block device over it, presenting the same
      `blockdev` port every later storage adapter will. Legacy version 1 is
      **refused and said so**, rather than half-supported: its queue layout
      differs, and a driver that half-supports what it cannot test is worse
      than one that says no.
      The kernel writes a pattern to the last sector, reads it back and
      restores the original — the last, because a filesystem lives at the
      start and a test that scribbles on sector 0 destroys what the next test
      reads. The test checks the image's digest from **outside**, which is the
      check the kernel's own opinion of itself cannot fool.
      **Three bugs, and the first one is the serious one.**
      *The frame allocator was handing out the firmware's memory.* Everything
      below the kernel's load address belongs to whoever loaded it: OpenSBI
      sits at the base of RAM with the kernel 2 MiB above, and it is still
      running — every SBI call, including the one that prints, executes there.
      The virtio queue landed on top of it and the store was refused by the
      physical memory protection OpenSBI sets up to defend itself. **That
      refusal was luck**: on a machine without it the write would have landed
      and the failure would have appeared later, elsewhere, for no visible
      reason. The rule is now positional and needs no agreement with the
      firmware — the kernel knows where it was loaded.
      *A descriptor cursor that never gave anything back*, which worked for
      exactly one request and then timed out, because the second chain was
      built from descriptors the device still owned. This driver is
      synchronous, so the same descriptors are reused every time and there is
      nothing to reclaim; a cursor is right for a driver with several requests
      in flight, and that driver would need a free list too.
      *A `NULL` passed for an out-parameter.* `cmdline_value(…, NULL)` for
      "is this key present" stored to address zero, because the length is
      written unconditionally. There is a `cmdline_has` now: a function that
      cannot be called wrongly beats a comment telling callers not to.
      And one diagnostic that earned its place: refusing a legacy transport
      now prints why. Without it, an emulator full of legacy transports looked
      exactly like a machine with no devices at all, and the silence was the
      bug.
- [x] **P4.2 The SD card (2026-10-22).** The board's controller is a Cadence
      part implementing the **SD Host Controller Interface**, a published
      standard rather than a vendor interface — so the driver is written
      against the standard and binds to the emulator's controller as well.
      The card protocol is identical either way and only the registers' address
      differs, which is what makes an SD driver testable without the board.
      Two protocols kept apart: the host controller (issue a command, clock the
      bus, move a block through the data port) and the card (CMD0, CMD8, ACMD41
      until it powers up, CMD2 and CMD3 for an address, CMD9 for its capacity).
      Polled, not DMA: the data port is correct, identical on every
      implementation, and needs no memory the card can reach. Speed is worth
      having once something waits on it.
      **The bug worth recording: every field of the capacity register arrives
      8 bits from where the specification says it is.** The card sends 136 bits
      — start, transmission, reserved, the 128-bit register, a CRC — and the
      controller presents only the middle, having dropped the CRC byte. So
      specification bit 127 arrives as response bit 119, and reading the
      documented positions gave plausible nonsense: a **64 MiB card reported
      itself as 30 GiB**, and the write that followed was addressed past its
      end. A test that only checked "a card was found" would have passed.
      And the other half of the same question: a high-capacity card addresses
      **blocks** while an older one addresses **bytes**, so getting that wrong
      reaches the first 1/512th of a modern card.
      One more find: an SD host controller reports its PCI class as
      `08:05:01` for "SDHCI 1.0 compliant" or `08:05:02` for a DMA-only
      variant, so the class match needed a **mask** — an exact match on
      `08:05:00` found nothing at all. There is a `minikern.pciscan` bus
      listing now, because "no controller" and "a controller whose class I did
      not recognise" are different problems that look identical without one.
- [x] **P4.3 NVMe over PCIe — the M.2 slot (2026-10-22).** Worked on the
      first run, which is what a specification written once and implemented
      everywhere buys you.
      **The PCIe walker assigns the memory windows itself.** No firmware here
      has: on a machine that boots Linux, U-Boot or the kernel writes the base
      addresses into each device's registers, and nothing on this path does —
      so a freshly enumerated device has BARs full of zeroes, answers
      configuration reads perfectly, and ignores every access to its own
      registers. The size is discovered the way the specification prescribes
      (write all ones, read back, the bits that stayed zero are not decoded)
      and the base is taken from the 32-bit window the bridge publishes in its
      `ranges`. **Bus mastering** is enabled in the same breath, because a
      controller that cannot master initialises perfectly, accepts commands,
      and never completes one — it cannot read the queues the driver built.
      Then NVMe's own handshake: reset, admin queue pair, enable, identify the
      controller, identify namespace 1, create the I/O queue pair. The model
      string comes from the drive's own identify data (`QEMU NVMe Ctrl`),
      which is what proves the admin queue works rather than merely that a
      device was found; the geometry comes from the namespace's current
      format, which reports its block size as a log2 in a table.
      The **phase bit** is the detail worth knowing: completions are never
      cleared, so "is this entry new?" is answered by comparing its phase with
      what this pass expects, and that expectation inverts when the ring wraps.
      A driver that forgets to invert it works for 32 commands and hangs on
      the 33rd — long after a quick test would have passed.
      Tested: enumerated, identified, written, read back, and the image's
      digest unchanged from outside. And **with both a virtio disk and an NVMe
      drive present, the NVMe drive is chosen**, because on a board with the
      M.2 slot filled that is the storage anyone would want.
- [x] **P4.4 XFS on real storage (2026-10-22).** The whole stack at once: a
      controller, a block-size adapter, a buffer cache, and the filesystem —
      reading an image `mkfs.xfs` made and `xfs_repair -n` calls clean, off an
      **NVMe drive**. The root directory lists every entry, and
      `/hello.txt` reads back as 43 bytes of *"hello from a real XFS
      filesystem on RISC-V"*. Populated with `mkfs.xfs -p`, a proto file,
      because mounting an image to copy files in needs root and this does not.
      **What it refuses is tested as carefully as what it reads**, because a
      filesystem whose layout you have guessed at is how data is lost quietly:
      reflink and rmapbt are turned away, so are sparse inodes, and the kernel
      **defers to the filesystem's own diagnosis** instead of paraphrasing it —
      an earlier version of the boot path guessed a cause from the error code
      and printed "a dirty log" over the top of "unsupported read-only
      features".
      **Two real bugs, both in the layer between the filesystem and the
      device.**
      *A filesystem's block size and a device's sector size are different
      facts.* XFS asks its cache in **filesystem blocks**, and every
      controller here presents 512-byte sectors, so each metadata read
      returned an eighth of a block: the filesystem **mounted** — a superblock
      fits in one sector — and then the first directory read failed with an
      I/O error and nothing else visibly wrong. The ARM kernel never needed a
      bridge, because its only device was a network one whose block size the
      caller chose. `core/blockscale.c` is that bridge, pure and host-tested,
      and it refuses a ratio that is not a whole number rather than rounding
      one into place.
      *And an NVMe PRP entry addresses from its offset to the end of its page,
      no further.* A page-aligned buffer hides this completely; hand the driver
      one eight bytes into a page — which is exactly what a kernel heap
      returns, since it puts a header in front — and the first entry covers
      4088 bytes while the last eight go nowhere. The symptom was precisely
      that: **byte 4088 of a 4 KiB block read back as zero.** `prp2` is set
      whenever a transfer runs past the first page, and anything that would
      need a PRP *list* is refused rather than silently truncated.

## P5 — Network

- [ ] **P5.1 virtio-net**, for the same reason as virtio-blk: the whole IPv4
      and TCP stack carries over and can be proven on the emulator.
- [ ] **P5.2 The DesignWare GMAC**, which is the JH7110's Ethernet. The
      largest single new driver in the port, and a standard IP block, so its
      descriptor format and reset sequence can be checked against independent
      implementations rather than guessed.
- [ ] **P5.3 Netboot.** The MKNB protocol, its server and the Ed25519
      verification all carry over; only the driver under them is new.

## P6 — The board's own hardware

- [~] **P6.1 GPIO — written, with the half that can be proved proved
      (2026-10-22).** **Both** controllers are enumerated from the device
      tree: the `sys` one with 64 pins and the always-on `aon` one with four.
      That is the ARM lesson applied in advance rather than relearned — there,
      the one pin anybody cared about lived in a second controller the pin
      parser did not know existed, so the feature was unusable on every board
      that had it, silently.
      **The register layout is unusual, and that is where the risk is:** a
      pin's output enable and output value are each a **byte** in a table of
      64, not a bit in a word; inputs are the conventional one-bit-per-pin
      across two words. Writing a bit where a byte belongs lands on a
      *neighbouring pin*, which on a board wired to real hardware is how a
      GPIO driver damages something — and no boot log would show it. So the
      mapping from pin to register is pure and **host-tested**: consecutive
      pins must be consecutive bytes, the two output tables must not overlap,
      every pin must map to a distinct input bit, and the input words must not
      land inside the output tables. The offsets are cross-checked against an
      implementation that runs on this silicon.
      The value is written **before** the enable, because a pin switched to
      output while its value byte still holds the last thing written drives
      that for an instruction — and on a board that glitch is on a wire.
      **What is not done, and is named rather than glossed:** the block's
      **clocks and resets** are not handled. The device tree declares them,
      and the ARM thermal sensor is the warning — a complete driver that was
      never switched on reads nothing and looks like it works. In practice the
      firmware enables this block because it uses GPIO itself; that is an
      assumption, and it is written down here as one. It needs a clock driver
      (P6.2), and until then no pin is driven: the kernel reports the
      controllers it found and stops there, because poking at pins on a board
      whose schematic it has not seen is how a kernel shorts something.
      No emulator models this controller, so the register *behaviour* has
      never run — the same honest position the ARM project ended in, reached
      deliberately this time and with the testable half tested.
      **Update (2026-10-22): the clock and reset gap is closed** (P6.2). The
      controller's clock is enabled and its reset released, both confirmed by
      reading back, and a controller that fails either is not marked up.
- [x] **P6.2 Clocks and resets (2026-10-22).** Written *because* of the ARM
      thermal sensor: a complete, correct driver that had never been switched
      on, so on silicon every write would have been ignored and every reading
      refused while the governor above it reported temperatures and looked
      healthy. A peripheral's clocks and resets are part of its contract, and
      every JH7110 driver in this tree now goes through this first.
      The layout is regular: a **clock** is one word at `base + 4 * index`
      with bit 31 enabling it; a **reset** is one bit in a bank, where
      *setting* the bit holds the block in reset and clearing it releases, and
      a separate status bank reads back 1 for a released line.
      **Both are confirmed by reading back, not assumed.** An enable bit that
      will not stay set means the *parent* clock is off — a different
      diagnosis from "I wrote it" — and a block still in reset accepts every
      write and ignores all of them. A controller whose clock will not enable
      or whose reset will not release is **not marked up**, so nothing above
      can drive a pin through a block that is not listening.
      The generators are found by `compatible`, never by address, and there is
      one per domain: the system peripherals' and the always-on domain's,
      with *different* reset-bank offsets — which is the "a pin controller is
      not one block" lesson one level further down.
      The addressing is pure and host-tested: a word per clock, resets banked
      32 to a word with the boundaries checked, and every id in the system
      generator's range proved to map to its own distinct bit. A reset id that
      resolves to the wrong bank releases a *different* peripheral, which
      takes a board down in a way nothing explains.
      GPIO now switches its block on through this, so **P6.1's named gap is
      closed** — and on a machine with no generator (an emulator) the kernel
      says so and touches nothing.
- [ ] **P6.3 Thermal and frequency.** The JH7110 has a temperature sensor
      (`sfctemp`) and CPU frequency control through its clock tree. The policy
      carries over from the ARM project — temperature overrules load,
      hysteresis against flapping, and a reading that **cannot be believed is
      treated as the worst case**, which is a fix that port only made after
      the governor had trusted an impossible −264 °C.

## P7 — Finishing

- [ ] **P7.1 Remove the ARM project from the monorepo** once this one reaches
      parity on the things that matter (Go unchanged, SMP, storage, network).
      It is published and tagged, so removal loses nothing — but it happens
      *after* the replacement works, not before.
- [ ] **P7.4 The documents a reader wants.** `GOALS.md`, `FEATURES.md` and
      this file are the published record; the agent ruleset (`CLAUDE.md`) and
      session state (`MEMORY.md`) are working documents and stay out of the
      repository, because a public tree carrying an instruction file addressed
      to a tool is confusing rather than transparent.
- [ ] **P7.2 The CVE suites**, which are the largest test surface in the ARM
      project and carry over: hostile syscalls from a Go program, malformed
      and attacking packets under ASan and UBSan, and the server's own
      adversarial tests.
- [ ] **P7.3 The hardware report.** `minikern.hwreport=1` prints every value
      taken from a datasheet beside what it should be, so powering a board up
      produces one checkable artefact instead of a vague instruction. It was
      the most useful thing the ARM project built late; here it belongs early.
