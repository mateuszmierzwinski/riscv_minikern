# Features

What this kernel does today, and what it is going to do. Honest about the
difference: nothing is listed as working unless a test proves it, and the test
is named. The phase-by-phase record is `DEVELOPMENT_PROGRESS.md`.

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
| **An SD card** through a standard host controller — found, powered up, addressed, sized correctly, read and written | `disk_qemu_test` |
| **NVMe over PCIe** — the M.2 slot: the bus walked, BARs assigned, bus mastering enabled, the controller identified and read/written | `disk_qemu_test` |
| **A PCIe bus walker** that assigns memory windows itself, because no firmware here has | `disk_qemu_test` |
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
| **Processes** | user address spaces, the timer through SBI, threads |
| **The syscall interface** | the generic table, ELF64, the process start state, signals — then Go binaries unchanged |
| **SMP** | harts started through SBI's hart-state extension, enumerated **from the device tree** because this SoC's application cores are 1–4 |
| **Storage** | XFS over the block devices that work now |
| **Network** | virtio-net first, then the DesignWare GMAC |
| **The board's hardware** | GPIO, clocks and resets, the temperature sensor and frequency scaling |
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
