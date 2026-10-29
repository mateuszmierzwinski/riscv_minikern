# Development progress — minikern on RISC-V (JH7110 / VisionFive 2)

The phase plan. `[x]` is proved by a test, `[~]` is partly done, `[ ]` is open.
Nothing is marked done because it was written. `ROADMAP.md` holds the open
tickets in detail, `FEATURES.md` what works, `CLAUDE.md` §5 the pitfalls, and
git the history. **The board is the gate** (`hil_test`); the QEMU suite
(`qemu_tests`, `manual`) is repaired but sidelined, kept for bisecting.

Inherited from the ARM project: `src/core` + `src/lib` (~9,300 lines, no
`arch/` or `board/` includes) with their host tests. Fixed on the way in:
64-bit `phys_t`, two-cell FDT values, ATAG support deleted.

## Phases

| Phase | Goal | State | Where proven |
|---|---|---|---|
| P-opt | Build for the U74 | done | `-mcpu=sifive-u74`, checked with `-print-enabled-extensions` |
| P0 | Boot: S-mode, console, traps, Sv39 | done but for top-half step C and the SD boot image | `boot_`/`selftest_qemu_test`, `pgtable_test`, board boot |
| P1 | Memory, scheduler, kernel lock | done | `pmm_test`, `vma_test`, board (8 GiB, 4 harts) |
| P2 | Linux syscall ABI; Go unchanged | done | `user_`/`init_`/`go_qemu_test`, `elf_test`, `ustack_test`, board Go run |
| P3 | SMP via SBI HSM | done but for stopping idle harts | `smp_qemu_test`; board: 3.6x on 8 units |
| P4 | Storage: virtio-blk, SD, NVMe, XFS | done | `disk_`/`xfs_qemu_test`, `xfs_test`; board SD + NVMe |
| P5 | Network: IPv4 stack, NICs, TLB shootdown | done but for netboot | `net_`/`tcp_`/`tcp_hostile_test`, `net_qemu_test`; board GMAC + DHCP |
| P6 | Board hardware: clocks, GPIO, thermal | partly done | `crg_jh7110_test`, `gpio_jh7110_test`, `cpufreq_test`; board jumpers |
| P7 | Sockets and Go `net`; finishing | sockets done, finishing open | `netgo_qemu_test`; board DNS/netlink programs |
| P8 | ARM tree removed; ARM parity | done but for netboot and thermal | ARM tree at `e7c6425d5`, `arm_minikern` v1.0.0 |
| Sched | Dynamic ticks, fair/RT classes, affinity | done; see `PREEMPTION_MODEL.md` | board: `minikern.spin`, `.nice`, `.rt`, `.pin`, `.isolate` |

## P-opt — built for the JH7110
- [x] `-mcpu=sifive-u74`: exactly RV64GC plus the U74 pipeline model; no extensions the chip lacks.

## P0 — Boot
- [x] P0.1 S-mode entry, SBI console, device tree read, SBI power-off; boot hart is not assumed to be 0.
- [x] P0.2 Own UART found by `compatible`, polled, firmware's baud kept; `reg-shift`/`reg-io-width` from the tree.
- [x] P0.3 One trap vector; `minikern.selftest` takes each fault on purpose; instruction length read (2 or 4 bytes).
- [~] P0.4 Sv39 paging (host-tested arithmetic, W^X kernel map, `rotext` self-test). The kernel now runs only in the top half (linked at `0xffffffc000200000`; `start.S` and secondaries jump high in asm). Remaining: step C — see ROADMAP §7.
- [ ] P0.5 Real SD-card boot image and flash procedure — see ROADMAP §7.

## P1 — Memory and processes
- [x] P1.1 Frame allocator over the real memory map, sized for 8 GiB; every frame starts used, firmware memory reserved.
- [x] P1.2 User address spaces, demand paging, `mmap`/`munmap`/`mprotect`/`mremap` (ROADMAP §2 Done).
- [x] P1.3a PLIC (context from `interrupts-extended`, not hart id) and SBI timer; 50 ticks in 499 ms.
- [x] P1.3b Threads with a kernel stack each, run queue, tick preemption; now dynamic ticks (`PREEMPTION_MODEL.md`).
- [x] P1.4 Context switch and one ticket lock around the kernel (delivered by P1.3b and P3.2).

## P2 — The syscall interface
- [x] P2.0 User mode via `sret`; `sscratch` tells the trap path the origin; user faults end the process, not the kernel.
- [x] P2.1 Generic `asm-generic/unistd` table; unimplemented calls logged once by name; `PROT_NONE` reserved, not backed.
- [x] P2.2 ELF64 loader, rewritten (not widened) from ELF32; hostile headers refused, CVE-tested.
- [x] P2.3 Process start state (argv, envp, auxv, `AT_PHDR`, 16-byte-aligned stack), asserted by the program itself.
- [x] P2.3a `/init` loaded off XFS on NVMe and run to completion.
- [x] P2.4 Stock gc Go (`go1.25.5`, `linux/riscv64`) runs unchanged: GC, goroutines, futexes, sleep, recover.
- [x] P2.5 Signals: per-thread mask and altstack, FP state saved, nested frames stacked, fault signals forced.
- [x] P2.6 `fork` (COW), `execve`, `wait4`, process groups, tty, uids, rlimits (ROADMAP §1 Done).

## P3 — SMP
- [x] P3.1 Harts from the tree started through SBI HSM; hart 0 (S7, no MMU) skipped.
- [x] P3.2 Per-hart scheduler state, `satp`, `SUM` and timer; `GOMAXPROCS=4`; board 3.6x on 8 units.
- [x] P3.4 Idle harts release to the kernel page table; `uspace_destroy` calls `sched_forget_address_space` first.
- [ ] P3.3 Stop idle harts through SBI `hart_stop`/`hart_start`, with the boot hart never stopping so a racing wake is always retried.
- [ ] P3.9 When harts can stop (P3.3), anything a hart caches about its own registers must be invalidated on restart.

## P4 — Storage
- [x] P4.1 virtio-blk (modern MMIO only; legacy refused with a reason).
- [x] P4.2 SD: SDHCI for QEMU, DesignWare `dwmmc.c` for the JH7110, shared card protocol; board 8.2 MB/s.
- [x] P4.3 NVMe over PCIe on the firmware's trained link; board drive identified, 334 MB/s.
- [x] P4.4 XFS on real storage, v4 and v5, `mkfs.xfs` defaults; GPT; automount by label (ROADMAP §3).
- Open storage and XFS work: see ROADMAP §3 and §8.

## P5 — Network
- [x] P5.1 IPv4/ARP/ICMP/UDP/DHCP/TCP stack carried over unchanged; three host suites green.
- [x] P5.2 virtio-net: separate RX/TX queues, 12-byte header both ways; DHCP lease and UDP echo.
- [x] P5.3 Cross-hart TLB maintenance via SBI RFENCE; removals broadcast, additions local with retry.
- [x] P5.4 DesignWare GMAC 5.20 + YT8531 PHY on the board: DHCP, TCP, DNS (ROADMAP §4/§5).
- [ ] P5.5 Netboot (MKNB + Ed25519) — see ROADMAP §7 "Own netboot".

## P6 — The board's own hardware
- [x] P6.1 GPIO on both pin controllers; byte fields written as 32-bit read-modify-write; all four jumper directions pass.
- [x] P6.2 Clocks and resets by `compatible`, confirmed by read-back; ungated inverter clocks recognised.
- [x] P6.3a Frequency: four operating points through one divider, measured 100/50/33/25%.
- [~] P6.3b Thermal: sensor and host-tested governor exist, not connected — see ROADMAP §4 "The thermal half".
- [ ] P6.3c Voltage scaling — see ROADMAP §4.

## P7 — Sockets and finishing
- [x] P7.1 Descriptor table with per-object read/write/poll/release.
- [x] P7.2 Real `epoll`; `struct epoll_event` is 16 bytes (packed only on x86-64).
- [x] P7.3 Checked user copies (`uspace_copy_in`/`_out` walk the page table).
- [x] P7.4 Sockets, LP64 `msghdr`; Go `net` UDP/TCP round trips, 64 KiB byte for byte; graceful close detaches.
- [ ] P7.5 `http.Client` against `http.Server` in one guest program, to exercise sockets under a real protocol.
- [ ] P7.6 `minikern.hwreport=1`: print each datasheet value beside what was read, as one checkable artefact (not yet in the tree).
- [ ] P7.7 Publish `GOALS.md`, `FEATURES.md` and this file as the record; keep `CLAUDE.md` out of any public tree.

## P8 — ARM tree and parity
- [x] P8.1 ARM tree removed; recoverable at `e7c6425d5` and `github.com/mateuszmierzwinski/arm_minikern` v1.0.0.
- [x] P8.2 `fork`/`exec` (ROADMAP §1); `give_every_mapped_page` on by default, `minikern.noforkleaves` the control.
- [x] P8.3 Board Ethernet (= P5.4).
- [ ] P8.4 Netboot — see ROADMAP §7.
- [ ] P8.5 Thermal control — see ROADMAP §4.

## Cross-cutting open items
- [~] Sleeper hang believed fixed by the top-half move (two-address bug); board confirmation pending — see ROADMAP §0.
- Everything else open is in `ROADMAP.md` §0–§9.
