# TASKS — minikern on RISC-V (StarFive JH7110 / VisionFive 2)

The plan and the record. Newest work is marked as it lands; `[~]` means
partly done with the remainder named, and nothing is marked `[x]` until it is
tested.

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
- [ ] **P0.2 The kernel's own UART.** The JH7110's UART0 is 16550-compatible,
      so the ARM board's `uart.c` is the one driver that ports nearly
      unchanged — but the base address and clock come from the device tree
      here rather than from a header. SBI's console stays as the fallback for
      the window before the tree is read, and for a board whose UART this
      kernel has not been told about.
- [ ] **P0.3 Trap entry and exceptions.** One `stvec`, one `scause`, one
      `sepc`, one `stval`: a single vector and a dispatch on the cause, where
      ARM had eight vectors and banked registers per mode. The frame is saved
      on the current stack. Deliberate faults (illegal instruction, load
      fault, store fault) as a self-test, the way `minikern.selftest=` proved
      the ARM trap path.
- [ ] **P0.4 Sv39 paging.** Three levels, 4 KiB pages, with the kernel in the
      top half (sign-extended, so `0xFFFF_FFC0...`) and user space in the
      bottom. The page-table code is pure and host-tested before it is ever
      installed, which is how the ARM equivalent caught a descriptor-type bug
      that would otherwise have been a silent fault storm.
- [ ] **P0.5 The real boot path on the board.** U-Boot on the VisionFive 2
      loads a flat image with `booti`; an SD card image with a `boot.scr`, and
      the documented flash procedure in `README.md`. The ARM project's
      `mksdcard.sh` is the template, minus `mkimage`.

## P1 — Memory and processes

- [ ] **P1.1 Physical and kernel allocators**, which are carried over: the
      page allocator and the heap are pure and already host-tested. What is
      new is the memory map they are given, read from the device tree.
- [ ] **P1.2 User address spaces**, lazy faults, `mmap`/`munmap`/`mprotect`.
      The VMA tree carries over; the page-table backing is Sv39.
- [ ] **P1.3 The timer and the scheduler.** SBI's timer extension sets one
      absolute deadline per hart against the `time` CSR — simpler than the
      ARM generic timer's per-core comparator, and firmware-mediated so it is
      the same on the emulator and the board.
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

- [ ] **P4.1 virtio-blk**, so the block layer, the buffer cache and XFS are
      exercised under the emulator before any hardware driver exists. The
      block device port and the cache carry over; this is a new adapter under
      them.
- [ ] **P4.2 The SD card (`mmc`).** The JH7110 has a Cadence SDHCI-compatible
      host controller. Read-only first — enough to mount a root filesystem —
      then writes.
- [ ] **P4.3 NVMe over PCIe.** The VisionFive 2 has an M.2 slot, and this is
      the storage path that makes the board worth using: a PCIe root-complex
      walk to find the controller, then the admin and I/O queues. Verifiable
      under QEMU first (`virt` offers a PCIe host bridge and an emulated NVMe
      controller), which means the queue handling can be right before the
      board's root complex is ever touched.
- [ ] **P4.4 XFS on real storage.** The implementation carries over whole —
      mount, read, logged writes, create, remove, grow — and `xfs_repair -n`
      staying clean after every test remains the rule.

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

- [ ] **P6.1 GPIO.** The JH7110's pin controller, as a `gpio_pin` port — the
      interface already exists and is host-tested. The lesson from the ARM
      port is recorded in advance: **a pin controller is not one block.** On
      that SoC the pin everything cared about lived in a *second* controller
      with its own clock gate, and the name parser rejected it as a typo, so
      the feature was unusable on every board that had it. Enumerate the
      controllers from the device tree; do not assume one.
- [ ] **P6.2 Clocks and resets.** Every peripheral here needs its gate
      ungated and its reset released before its registers mean anything —
      which is exactly the bug that left the ARM thermal sensor dead: a
      complete driver that had never been switched on. A peripheral's clocks
      and resets are part of its contract.
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
- [ ] **P7.2 The CVE suites**, which are the largest test surface in the ARM
      project and carry over: hostile syscalls from a Go program, malformed
      and attacking packets under ASan and UBSan, and the server's own
      adversarial tests.
- [ ] **P7.3 The hardware report.** `minikern.hwreport=1` prints every value
      taken from a datasheet beside what it should be, so powering a board up
      produces one checkable artefact instead of a vague instruction. It was
      the most useful thing the ARM project built late; here it belongs early.
