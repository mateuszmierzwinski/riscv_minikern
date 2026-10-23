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

## Measured facts, and what the board disproved

**`BENCHMARKS.md` owns the numbers** (real silicon only, a row per publish),
and `DEVELOPMENT_PROGRESS.md` owns the record of how each one was arrived at.

This section used to hold a table of its own, and deleting it is a correction
rather than a tidy-up. It was headed "Measured facts" and its contents were
measured under *emulation*; worse, it carried "the SD host controller is
Cadence SDHCI-compatible" from the documentation long after the board had
said otherwise — the JH7110's own device tree gives `starfive,jh7110-mmc`, a
Synopsys DesignWare controller with a different register set entirely. The
driver proved against the emulator was not a driver with a bug in it; it was
the wrong driver, and only hardware could say so.

That claim survived here because it had been written down twice: once where
it was corrected, and once where nobody looked again. Hence the rule this
file now follows — **it explains why, and owns no facts.**

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
