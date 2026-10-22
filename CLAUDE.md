# CLAUDE.md — minikern

> The design record: the architecture and the reasoning behind it, the hardware
> facts as measured rather than as documented, and every pitfall this project —
> or the ARM kernel it migrated from — has already paid for. The plan and the
> record of the migration is `TASKS.md`.
> When the code and this file disagree, fix this file in the same change.
>
> minikern is developed inside a private monorepo and published from it, so §2
> is written against that monorepo's master ruleset, which is not part of this
> repository. It is kept as it stands because what is interesting there is the
> deviations and their reasons, whether or not the rule being deviated from is
> in front of you.

---

## 0. Goal

**A clean, from-scratch kernel that copies no Linux code but implements
Linux's syscall interface, so Go binaries run unchanged — on the StarFive
JH7110 (SiFive VisionFive 2).**

This is the migration of `arm_minikern`, which reached that goal on a 32-bit
Allwinner H3. The reason for moving is that the friction left in that project
was the architecture, not the design: a 32-bit legacy ABI (`mmap2`, `_llseek`,
`stat64`, `futex_time64`, the ARM-private `set_tls`), banked registers, a
HYP→SVC drop on every core, and a PSCI implementation that started secondaries
in the wrong mode and refused to power the machine off. None of that exists
here.

"Linux-like" means **ABI-compatible**: the syscall numbers (the generic
`asm-generic/unistd` set), the calling convention, the `struct` layouts user
space sees, the process start state, and the errno values. These are interface
facts; using them copies no code. Nothing from the Linux source tree is copied
or vendored. Where a datasheet and another implementation disagree, **the
datasheet and a measurement decide** — and where this project's own comments
claim a provenance they cannot show, the claim is withdrawn rather than
repeated.

---

## 1. Status

| Phase | State |
|---|---|
| P0 Boot | **S-mode entry proved (2026-10-22).** OpenSBI loads the ELF at `0x80200000`, enters S-mode with the hart id in `a0` and a device tree in `a1`; the kernel prints through SBI, reads the tree, and powers the machine off. 4 host tests + 1 boot test green. |
| P1 Memory/processes | Not started; the allocators and the VMA tree carry over. |
| P2 Syscalls | Not started. |
| P3 SMP | Not started. The hart numbering is the thing to get right (§3). |
| P4 Storage | Not started: virtio-blk, then SD, then NVMe. |
| P5 Network | Not started: virtio-net, then the GMAC. |
| P6 Board hardware | Not started: GPIO, clocks/resets, thermal. |

---

## 2. Deviations from the master ruleset

| Master rule | Here |
|---|---|
| Hexagonal architecture | **Applies.** `src/core` is the hexagon and includes nothing from `arch/` or `board/` — which is why it carried over from the ARM project untouched. `src/board/jh7110/main.c` is the composition root. |
| `interfaces.go` (7.4) | `core/ports.h` holds every port: a struct of function pointers plus `self`. |
| `errors.go` (8.3) | `core/errno.h`, with Linux's errno values, because they go back to user space as `-errno`. |
| Function length (11) | Applies in full: 25 code lines, 27 tolerated, 30 only with the named test in the doc comment. |
| Mocking with gomock (17) | N/A for C. A port is a struct of function pointers, so a test passes a fake struct. The harness is `test/ktest.h`. |
| Sonar (24) | Skipped: no `go.mod` anywhere, so the master's decision logic says skip. |
| CVE suite (25) | **Applies**, and carries over from the ARM project as the largest test surface. |
| Toolchain | **clang, not gcc** — a deviation worth stating. There is no riscv64 cross-gcc installable without root here, and clang cross-compiles by construction. See §4. |
| Security posture (29) | Applies once there is Go code in the tree. For C, `-Wall -Wextra -Werror` is the floor. |

**No libc, no third-party code.** Everything the kernel needs is written here.
Unlike the ARM project, there is no hand-written 64-bit division: the ISA has
it.

---

## 3. The hardware — measured facts

QEMU 10.1.5 `qemu-system-riscv64`, measured 2026-10-22:

| Fact | Value | Consequence |
|---|---|---|
| Boot protocol | OpenSBI (bundled with QEMU) loads an **ELF payload at `0x80200000`** and enters it in **S-mode** | no mode drop to negotiate, and no `objcopy`/`mkimage` in the build |
| Entry registers | `a0` = hart id, `a1` = physical address of a device tree (magic `0xd00dfeed` confirmed) | `core/fdt.c` carries over unchanged; there is no ATAG alternative |
| **The boot hart is not 0** | `virt` picks one by lottery (0, 1 and 2 all seen across runs); **`sifive_u` boots on hart 1**, because hart 0 is a monitor core | the JH7110 pairs an **S7 at hart 0 with four U74s at 1–4**, so a kernel that assumes it is hart 0, or that the application cores are `0..n-1`, is wrong on the target board. Enumerate the `cpus` node. |
| SBI extensions on `virt` | TIME, HSM, SRST and DBCN all present | the timer, hart bring-up, power-off and a debug console are all firmware-mediated and identical on the emulator and the board |
| SBI power-off | **works** (`SRST`, QEMU exits 0) | a boot test can wait for an exit, where the ARM project's PSCI power-off was refused by U-Boot and its test had to grep a line and kill the emulator |
| An M-mode CSR from S-mode | `csrr misa` is an **illegal instruction** | the RISC-V counterpart of ARM's HYP-vs-SVC trap: it killed the first spike before its first character. What a supervisor may know about its ISA comes from the device tree. |
| `virt` peripherals | PLIC, CLINT, virtio-net, virtio-blk, a PCIe host bridge | unlike the H3, **everything this kernel needs is modelled**, so the bring-up is verifiable without a board |

**From the JH7110 documentation — verify each when first used:**

- UART0 is 16550-compatible; its base and clock come from the device tree.
- The SD host controller is Cadence SDHCI-compatible.
- Ethernet is a DesignWare GMAC — a standard IP block, so its descriptor
  format can be checked against independent implementations.
- There is an M.2 slot behind a PCIe root complex: NVMe.
- The temperature sensor is `sfctemp`, and CPU frequency goes through the
  clock tree.

---

## 4. Toolchain and build

| Tool | Why |
|---|---|
| **clang 21** (`--target=riscv64-unknown-elf -march=rv64gc -mabi=lp64d`) | Fedora 43 ships no riscv64 cross gcc this account can install (packages need root), and clang cross-compiles by construction. Measured: it builds a working freestanding S-mode image with no root at all. |
| **`ld.lld-19`** | the linker, via `--ld-path`. |
| **GNU `ar` and `nm`** | measured to archive and read RV64 objects, even though Fedora's BFD is x86-only. |
| **`objcopy`/`objdump`/`strip`: unavailable** | Fedora's binutils cannot recognise a RISC-V object, and nothing here asks it to — SBI loads the ELF. They are pointed at a failing command so that a step which starts needing one fails loudly. |
| `qemu-system-riscv64` 10.1.5 | `-M virt` with `-bios default` (OpenSBI). |

```bash
bazel test //...          # host tests
bazel test //:qemu_tests   # the emulator (manual)
bazel build //:minikern    # the image
```

Kernel command line: `minikern.halt=poweroff` (power off when done, which is
what makes a boot test a test).

---

## 5. Pitfalls already met

- **An M-mode CSR read from S-mode is an illegal instruction.** `csrr misa`
  killed the first spike before it printed anything. The analogue of the ARM
  project's HYP-vs-SVC trap: a privileged register that exists at a privilege
  level you are not at.
- **The boot hart is not hart 0**, and on the target SoC hart 0 is not even an
  application core (§3).
- **A two-cell device-tree value is 64 bits.** The reader carried over from
  ARM *refused* one whose high word was set, which was safe on a 512 MiB board
  and rejects the memory node of an 8 GiB VisionFive 2 outright.
- **A physical address is not 32 bits here.** DRAM starts at `0x4000_0000` and
  a board can carry 8 GiB, so the top of its own memory is past
  `0x2_0000_0000` (`core/types.h`).
- **An anchored pattern must account for the carriage return** the console
  emits, which cost a test failure in the ARM project twice and once here.

### Inherited, and worth not relearning

These were paid for on the ARM project and apply here before the code exists:

- **A peripheral's clocks and resets are part of its contract.** A complete
  driver that was never switched on reads nothing and looks like it works: the
  ARM thermal sensor had no bus gate, no reset release and no module clock, so
  every reading would have been refused on a board while the governor appeared
  healthy.
- **A pin controller is not one block.** The pin that mattered there lived in a
  *second* controller with its own clock gate, and the name parser rejected it
  as a typo — so the feature was unusable on every board that had it.
- **A fake proves the policy and nothing about the registers.** The frequency
  governor was green against a fake clock while three of five operating points
  failed with `EINVAL`, because nothing had ever run the real arithmetic.
- **A reading that cannot be believed must be treated as the worst case**, not
  acted on. An impossible −264 °C sent that governor to the top of the table
  with nothing watching the temperature.
- **A test that encodes an assumption protects the assumption, not the user.**
- **Nothing large goes on a kernel stack** (`-Wframe-larger-than=1024`).
