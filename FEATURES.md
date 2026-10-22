# Features

What this kernel does today, and what it is going to do. Honest about the
difference: nothing is listed as working unless a test proves it, and the test
is named. The phase-by-phase record is `DEVELOPMENT_PROGRESS.md`.

## Proved on a real VisionFive 2

Everything below this section is proved by a test under QEMU. These are
proved on the board itself — a StarFive VisionFive 2 (`VF7110B1`, 8 GiB),
driven over its UART with its power switched by a relay. An emulator agreed
with every one of the mistakes they found.

| On the board | What it took |
|---|---|
| **The whole kernel boots on silicon**, through the board's own OpenSBI | the hart id, the device tree, the memory node and the timer frequency all read rather than assumed: this board's harts are 1–4, **hart 0 is a monitor core with no usable MMU**, its PLIC context is 8, and its timer is 4 MHz where QEMU's is 10 |
| **Four U74 cores running Go**, `NumCPU=4`, **3.3x** on 8 units of work | a lost-increment race in the hart-arrival counter: it was a plain `running++`, and that number reaches user space as `NumCPU`, so the board ran Go on two of its four cores with nothing looking wrong |
| **An SD card**, through the controller the board actually has | the documentation said Cadence SDHCI; the board's tree says `starfive,jh7110-mmc`, a DesignWare MSHC. Two transports now share one copy of the card protocol |
| **XFS mounted from that card, and a stock Go binary loaded off it and run** | `logstart` is an AG-encoded block number — this filesystem's *write* path converted it and its *read* path did not, so the two halves disagreed about where the log was |
| **An NVMe drive in the M.2 slot**: `TS128GMTE110S`, 122 GB | the board's PCIe is `starfive,jh7110-pcie`, read through the link the firmware trained; and a command's data pointer must be page-aligned, which `kzalloc` is not |
| **Its GPT read**, and the first partition presented as a disk | GPT and MBR, with a GPT disk's protective MBR recognised and skipped rather than followed |
| **The kernel's own UART**, taking the console over from the firmware | a 16550's registers are not always one byte apart: this one has `reg-shift = 2` and `reg-io-width = 4`, and getting it wrong is *garbling*, not silence |
| **A GPIO level crossing a physical wire**: driven on pin 60, read on pin 63 | the operator's jumper. It immediately showed what a loopback cannot: the other pin could not be driven at all, because this driver does not yet select a pad's function |
| **Both pin controllers clocked**, including the always-on one | a clock index belongs to a generator, not to a SoC — and the right numbers are in each node's own `clocks` property |

What is **not** done on the board: the pad function is not selected, so only a
pin the firmware already left as GPIO can be driven; there is no driver for
the board's own DesignWare GMAC; an XFS made with `mkfs.xfs`'s defaults stops
at `nrext64`, which changes the inode layout; and thermal and frequency
scaling are not started.

## Working now

| Feature | Proved by |
|---|---|
| Boots as an SBI payload in S-mode, on QEMU's `virt` under real OpenSBI | `boot_qemu_test` |
| Takes its hart id and device tree from the firmware, and **never assumes it is hart 0** | `boot_qemu_test` (the hart id is in the banner) |
| Probes which SBI extensions the firmware implements rather than assuming them | `boot_qemu_test` |
| Reads the device tree: memory, the kernel command line | `boot_qemu_test`, `bootinfo_test`, `fdt_test` |
| Memory nodes of **any size**, including an 8 GiB board's two-cell values | `bootinfo_test`, `fdt_test` |
| Powers the machine off through SBI, so a test ends by the machine exiting | `boot_qemu_test` |
| One trap vector: exceptions and interrupts, with a frame whose layout the assembly and the C are asserted to agree on | `selftest_qemu_test` |
| Deliberate faults taken and survived — illegal instruction, load, store, breakpoint — each checked for the **right cause and the right faulting address** | `selftest_qemu_test` |
| **Sv39 paging**, with the kernel identity-mapped: text read-execute, everything else read-write | `pgtable_test` (10 cases, on the host), `selftest_qemu_test` |
| The kernel's own text is **not writable**, and that is proved by trying | `selftest_qemu_test` (`rotext`) |
| Large pages where they fit: a 1 GiB identity map costs 3 table pages | `pgtable_test` |
| **Finds devices in the device tree by `compatible` string** and maps their registers — the pattern every driver uses | `fdt_test`, `boot_qemu_test` |
| Its own **16550 UART**, found that way, taking the console over from SBI | `boot_qemu_test` |
| **A frame allocator and a kernel heap** over the memory the tree describes, with the kernel, the tree and the page tables reserved | `pmm_test`, `boot_qemu_test` |
| **8 GiB of RAM**, read and tracked: 2 Mi frames | `boot_qemu_test` (an `-m 8G` boot), `pmm_test` (frames at 9 GiB) |
| **Interrupts** through the PLIC, with each hart's *context* resolved from the tree rather than assumed to be its hart id | `boot_qemu_test` |
| **A tick** through SBI's timer, keeping time: 50 ticks in 499 ms at 100 Hz | `boot_qemu_test` |
| **Go's `net` package, unchanged** — UDP round trip, TCP connect/accept/half-close, a 64 KiB bulk transfer byte for byte, with zero resets | `netgo_qemu_test` |
| **Descriptors**: a real table with per-object operations, and a **real `epoll`** that reports readiness rather than nothing | `netgo_qemu_test` |
| **Checked user copies**: a syscall validates a user pointer instead of dereferencing it, so a bad one is `EFAULT` rather than a kernel fault | `netgo_qemu_test` |
| **Signals**: `rt_sigaction`, masks and a per-thread alternate stack, delivery with a real frame, a kernel-supplied return trampoline — so a nil pointer dereference is a **recoverable Go panic** | `go_qemu_test`, `smp_qemu_test` |
| **Floating-point state** saved per thread and per signal frame | `go_qemu_test` |
| **Networking**: Ethernet, ARP, IPv4, ICMP, UDP, DHCP and TCP over virtio-net — a lease, a gateway that answers, and a datagram echoed back through the adapter | `net_qemu_test`, `net_test`, `tcp_test`, `tcp_hostile_test` |
| **TLB maintenance across harts** — the firmware's cross-hart fence on every mapping *removed*, local-only on every mapping added, with a spurious fault retried | `go_qemu_test` under load |
| **Built for the JH7110**: `-mcpu=sifive-u74` (exactly `rv64gc` + the U74 scheduling model, verified), and per-hart data padded to the U74's 64-byte cache line | unverifiable under an emulator that models no caches |
| **Four harts, all running user threads** — enumerated from the device tree (ids are not positions), started through the firmware's hart-state interface, scheduled behind one kernel lock; `GOMAXPROCS=4`, 1.6x on 8 units of work | `smp_qemu_test` |
| **A stock gc Go binary runs unchanged** — `go1.25.5` for `linux/riscv64`, off a filesystem: goroutines, a contended mutex, channels, the garbage collector, sleeps, panic/recover | `go_qemu_test` |
| **Threads**: `clone`, a run queue, preemption on the tick, a kernel stack each | `go_qemu_test` |
| **Futexes**: wait and wake, with timeouts | `go_qemu_test` |
| **Memory syscalls**: `brk`, `mmap` with unbacked `PROT_NONE` reservations and honoured hints, `munmap`, `mprotect` | `go_qemu_test` |
| **A real program off the filesystem**: `/init` read from XFS on NVMe, ELF64-loaded, started with `argc`/`argv`/`envp`/`auxv`, run to completion | `init_qemu_test` |
| **An ELF64 loader** that refuses what it cannot run — 32-bit, wrong architecture, dynamically linked, headers that contradict the file | `elf_test` |
| **User mode**: a process in its own address space, making syscalls, resumed after each — and a user fault that ends the process while the kernel carries on | `user_qemu_test` |
| **XFS on a real device** — mounted off NVMe, directory listed, file contents read; and every feature it does not understand refused | `xfs_qemu_test`, `xfs_test` |
| **A block-size adapter**: a filesystem's 4 KiB blocks over a controller's 512-byte sectors | `blockscale_test` |
| Built with **`-mcpu=sifive-u74`**: the JH7110's own core and pipeline, not a generic RV64GC | the whole suite runs on it |
| **GPIO** on both JH7110 pin controllers: found, mapped, **clocked and brought out of reset** (both confirmed by reading back), with the pin-to-register arithmetic proved — a *byte* per pin, not a bit | `gpio_jh7110_test` |
| **Clocks and resets** through the JH7110's generators, found by `compatible`, with every enable and release confirmed rather than assumed | `crg_jh7110_test` |
| **An SD card** over two host controllers — SDHCI and the JH7110's DesignWare MSHC — sharing one copy of the card protocol: powered up, addressed, sized, read and written | `disk_qemu_test`, `sdcard_test`, and a real card on the board |
| **NVMe over PCIe** — the M.2 slot: the bus walked, the controller identified, read and written | `disk_qemu_test`, and a real drive on the board |
| **A PCIe bus walker** that follows bridges rather than sweeping bus numbers, and keeps a BAR the firmware placed | `disk_qemu_test`, and two real controllers on the board |
| **Partition tables**, GPT and MBR, with a GPT disk's protective MBR skipped rather than followed | `partition_test`, and a real GPT drive on the board |
| **A real block device** over virtio, with the carried-over buffer cache on top — written, read back, and the image byte-for-byte unchanged | `disk_qemu_test`, `bcache_test` |
| A printf-style formatter, command-line parsing, CRC-32C, SHA-256, SHA-512, Ed25519 verification | host tests, carried over |

## Carried over, not yet wired up

These came from the ARM kernel and depend on no architecture, so they are
already in the tree and already have host tests. They are listed separately
because *present* is not *working*: each needs the kernel services beneath it.

IPv4 with ARP, ICMP, UDP and TCP · DHCP · the MKNB netboot protocol with
Ed25519-verified offers · XFS (mount, read, logged writes, create, remove,
grow) · a network block device client · a buffer cache · the VMA tree · an
ELF loader · a ustar reader.

## Planned

In the order it will be built, because each step needs the one before it:

| | |
|---|---|
| **The pad function** | GPIO writes reach a pad only if the pin controller's mux selects GPIO for it. Without that, a pin muxed to a peripheral ignores them in silence — which is what the board showed |
| **The board's Ethernet** | the DesignWare GMAC. The cable is in the second of the two, which is the one that negotiates |
| **Modern XFS** | `nrext64` changes the inode core's layout, and sparse inodes change the inode b-tree's records. Until both are read, a filesystem made with `mkfs.xfs`'s defaults is refused rather than guessed at |
| **`fork` and `exec`** | so `os/exec` works. The ARM kernel had them; this one does not yet |
| **The top half** | the kernel still lives in the bottom half of the address space |
| **Thermal and frequency scaling** | the temperature sensor, then the clock tree |
| **The real boot path** | an SD-card image with U-Boot, and the documented flash procedure |

## Deliberately not planned

- **A second architecture.** This kernel targets the JH7110. The ARM work is
  archived at [arm_minikern](https://github.com/mateuszmierzwinski/arm_minikern),
  and keeping both would mean an abstraction layer whose only user is history.
- **SACK**, in TCP. It improves on reassembly rather than replacing it, and
  with the reassembly queue a loss already costs one round trip.
- **XFS log replay.** A filesystem whose log is not clean is refused at mount,
  so a crash is `xfs_repair`'s to deal with rather than something this kernel
  guesses at.
