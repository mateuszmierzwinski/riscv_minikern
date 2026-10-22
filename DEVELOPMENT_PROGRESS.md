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
- [ ] **P1.3b The scheduler** over that tick: threads, a run queue,
      preemption.
- [ ] **P1.4 Threads, context switch, and the kernel lock.** The ARM design
      holds: one lock around the kernel, taken on entry and released on exit,
      so that structures written for one core stay correct on five. The
      parallelism that matters is in user space.

## P2 — The syscall interface

- [ ] **P2.1 The generic syscall table.** RISC-V uses `asm-generic/unistd`,
      which is the modern set: `mmap` with byte offsets rather than `mmap2`
      with page offsets, one `lseek`, one `fcntl`, one `stat`, one `futex`.
      Eight legacy ABI shapes the ARM kernel implemented simply do not exist
      here. ~104 syscalls to renumber, and the handlers read their arguments
      from a RISC-V frame (`a0..a5`, number in `a7`).
- [ ] **P2.2 ELF64.** The loader carries over; the class, machine and header
      sizes change.
- [ ] **P2.3 The process start state** — `argc`, `argv`, `envp`, `auxv`. No
      `AT_HWCAP` bit for TLS to advertise, because `tp` is in the ABI; no
      sigpage, because RISC-V's signal return is its own.
- [ ] **P2.4 Signals**, including what Go's runtime actually needs. This is
      the one item with a known unknown: on ARM, Go set `SA_RESTORER` with a
      zero restorer and needed a kernel-provided trampoline page, which cost a
      day to discover. RISC-V does it differently and the difference has to be
      measured, not assumed.
- [ ] **P2.5 Go runs unchanged.** `GOARCH=riscv64`, which is 64-bit and
      first-class, so none of the 32-bit heap-reservation gymnastics applies.
      Threads, futexes, `epoll` timers, preemption, the garbage collector.

## P3 — SMP, the JH7110's way

- [ ] **P3.1 Harts up through SBI HSM.** `hart_start` cannot start a core in
      the wrong privilege mode — the specification says S-mode — which
      removes the entire class of bug that made the ARM bring-up hard.
      **The hart numbering is the thing to get right:** this SoC's harts are
      an S7 at 0 and four U74s at 1–4, so the kernel enumerates the `cpus`
      node of the device tree and starts what it finds, rather than counting
      from zero.
- [ ] **P3.2 Per-hart state**, with the lesson from the ARM port written down
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
