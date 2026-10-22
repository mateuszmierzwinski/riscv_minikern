# Goals, design and measured facts

What this kernel is for, how it is built, and what has actually been measured
rather than read in a datasheet. The running record of work is
`DEVELOPMENT_PROGRESS.md`; what works today is `FEATURES.md`.

## The goal

**A clean, from-scratch kernel that copies no Linux code but implements
Linux's syscall interface, so that ordinary Go binaries run on it unchanged —
on the StarFive JH7110 (SiFive VisionFive 2).**

"Linux-like" here means **ABI-compatible**: the syscall numbers (the generic
`asm-generic/unistd` set), the calling convention, the structure layouts user
space sees, the process start state, and the errno values. Those are interface
facts, and using them copies no code. Nothing from the Linux source tree is
copied or vendored into this repository.

Where a datasheet and another implementation disagree, **the datasheet and a
measurement decide**. Where this project's own comments have claimed a
provenance they could not show, the claim is withdrawn rather than repeated —
that has already happened once, to a pair of thermal-sensor constants in the
kernel this migrated from.

## Why RISC-V, and why this board

This is the continuation of
[arm_minikern](https://github.com/mateuszmierzwinski/arm_minikern), which
reached the same goal on a 32-bit Allwinner H3. The move is away from the
architecture, not away from the design: what that kernel had to carry is in
the table in `README.md`, and none of it exists here.

The board matters too. That project ended one item short, and the item could
not be closed: three of the H3's registers — the Ethernet PHY select, the
thermal sensor and the pin controller — are modelled by no emulator, so they
had to be taken on faith until the hardware was in hand. QEMU's `virt` machine
models everything *this* kernel needs, so the bring-up is verifiable without a
board at all. The JH7110's own peripherals still need the board, but it is a
better-documented part: upstream Linux support, a published technical
reference manual, and standard IP blocks with independent implementations to
check against.

## Architecture

```
src/
├── lib/        freestanding helpers, no kernel knowledge (host-tested)
├── core/       the hexagon: pure, portable C — it includes nothing from
│               arch/ or board/, which is why it carried over from the ARM
│               kernel untouched
├── arch/riscv/ RV64: entry, traps, Sv39 translation, SBI
└── board/jh7110/ the machine: its console, and main.c — the composition root
```

Rules that keep it that way:

- **Dependencies point inward.** `core` never includes `arch/` or `board/`.
  Only `board/<board>/main.c` names concrete adapters.
- **A port is a struct of function pointers plus a `self` pointer**, so a
  test fakes one with a stack structure and no framework.
- **Anything that parses input a kernel does not control takes a bound,**
  rejects what runs past it, and has a hostile test.
- **Nothing large goes on a kernel stack** (`-Wframe-larger-than=1024`).
- Functions stay under 25 lines of code; the documentation lives on the
  declaration, not in the body.

## Measured facts

QEMU 10.1.5 `qemu-system-riscv64`, measured 2026-10-22:

| Fact | Value | Consequence |
|---|---|---|
| Boot protocol | OpenSBI loads an **ELF payload at `0x80200000`** and enters it in **S-mode** | no privilege-mode drop to negotiate, and no `objcopy` or `mkimage` in the build |
| Entry registers | `a0` = hart id, `a1` = a device tree's physical address (magic confirmed) | the device-tree reader carried over unchanged; there is no second boot format |
| **The boot hart is not hart 0** | `virt` picks one by lottery (0, 1 and 2 all seen); **`sifive_u` boots on hart 1**, because hart 0 is a monitor core | the JH7110 pairs an **S7 at hart 0 with four U74s at 1–4**, so anything assuming "I am hart 0", or that the application cores are `0..n-1`, is wrong on the target board |
| SBI extensions on `virt` | timer, hart-state, reset and debug-console all present | timers, core bring-up and power-off are firmware-mediated and identical on the emulator and the board |
| SBI power-off | **works** | a boot test can wait for the machine to exit, where the ARM project's PSCI power-off was refused by U-Boot and its test had to grep a line and kill the emulator |
| An M-mode CSR read from S-mode | **illegal instruction** | it killed the first spike before its first character. What a supervisor may know about its ISA comes from the device tree |
| Instruction width | **2 or 4 bytes** (`rv64gc` includes compressed instructions) | "skip the faulting instruction" must read the instruction, not assume a width — see the progress record for what assuming cost |
| Sv39 identity map of 1 GiB | **3 table pages** | gigapage leaves make the kernel's own map nearly free |
| **A hart's PLIC context is not its hart id** | on `virt`, hart 0's supervisor context is **1** — context 0 is its machine-mode context, the firmware's | the context is a position in the controller's own `interrupts-extended` list, so it is resolved by walking that list and following each phandle to the hart it names. Assuming they match enables another context's sources |
| Counter frequency on `virt` | 10 MHz, from `/cpus/timebase-frequency` | the rate is the machine's and the ISA does not state it, so it comes from the tree — the same lesson as the ARM timer that ran at 62.5 MHz under one boot path and 24 MHz under another |
| QEMU `virt`'s UART | an `ns16550a` at `0x10000000`, in `/soc/serial@10000000` | the same driver binds to it and to the JH7110's `snps,dw-apb-uart`, because it asks the tree rather than being compiled for a board |

### From the JH7110 documentation — each to be verified when first used

- UART0 is 16550-compatible; its address and clock come from the device tree.
- The SD host controller is Cadence SDHCI-compatible.
- Ethernet is a DesignWare GMAC — a standard block, so its descriptor format
  can be checked against independent implementations.
- There is an M.2 slot behind a PCIe root complex: NVMe.
- The temperature sensor is `sfctemp`; CPU frequency goes through the clock
  tree.

## Pitfalls already paid for

Met here:

- **An M-mode CSR read from S-mode is an illegal instruction** — a privileged
  register that exists at a privilege level you are not at.
- **The boot hart is not hart 0**, and on the target SoC hart 0 is not even an
  application core.
- **A two-cell device-tree value is 64 bits.** The reader carried over from
  ARM *refused* one whose high word was set, which was safe on a 512 MiB board
  and rejects the memory node of an 8 GiB VisionFive 2 outright.
- **A physical address is not 32 bits here.** DRAM starts at `0x4000_0000` and
  a board can carry 8 GiB, so the top of its own memory is past
  `0x2_0000_0000`.
- **An unreachable address faults differently depending on whether
  translation is on:** an access fault means nothing answered on the bus, a
  page fault means the address is not in the table. Conflating them sends the
  reader to the wrong place entirely.

Inherited from the ARM project, and worth not relearning:

- **A peripheral's clocks and resets are part of its contract.** A complete
  driver that was never switched on reads nothing and looks like it works.
- **A pin controller is not one block.** The pin that mattered there lived in
  a second controller with its own clock gate, and the name parser rejected it
  as a typo — so the feature was unusable on every board that had it.
- **A fake proves the policy and nothing about the registers.** A frequency
  governor was green against a fake clock while three of five operating points
  failed with `EINVAL`.
- **A reading that cannot be believed must be treated as the worst case**, not
  acted on.
- **A test that encodes an assumption protects the assumption, not the user.**

## Building

```bash
bazel test  //...            # host tests: no board, no emulator
bazel test  //:qemu_tests    # under QEMU (tagged manual)
bazel build //:minikern      # the image SBI loads
```

The toolchain is **clang** with `ld.lld`: no riscv64 cross-gcc is installable
without root on the development machine, and clang cross-compiles by
construction. GNU `ar` and `nm` are used and measured to handle RV64 objects;
`objcopy`, `objdump` and `strip` are deliberately pointed at a failing command,
because Fedora's binutils cannot read a RISC-V object and nothing in this build
asks it to — SBI loads the ELF.
