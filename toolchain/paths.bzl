"""Host tools the kernel build shells out to. One place, read by the toolchain and the image rule.

**clang, not gcc.** Fedora 43 ships no riscv64 cross gcc that this account can
install (packages need root), and clang is a cross compiler by construction:
one binary targets every architecture it was built with. It produced a working
freestanding RV64 S-mode image on this machine with no root at all, which is
why it is the toolchain rather than the fallback. That is a deviation from the
ARM project this grew from, and it is recorded in CLAUDE.md §4.
"""

CLANG = "/usr/bin/clang"

# clang's own freestanding headers (stdint.h, stddef.h, stdbool.h). A
# freestanding kernel uses exactly these and no others, and Bazel requires
# the directory to be declared or it rejects the "absolute path inclusion".
CLANG_INCLUDE = "/usr/lib/clang/21/include"
LLD = "/usr/bin/ld.lld-19"
# The archiver and the symbol lister. GNU binutils, because this machine has
# no llvm-ar — and measured to work: `ar` archives and indexes RV64 objects,
# and `nm` reads them, even though Fedora's BFD is configured for x86 only.
AR = "/usr/bin/ar"
NM = "/usr/bin/nm"

# **objcopy, objdump and strip are deliberately unavailable.** Fedora's
# binutils cannot recognise a RISC-V object ("Unable to recognise the format
# of the input file"), and nothing in this build asks it to: SBI firmware
# loads the kernel's **ELF** directly, so there is no flat binary to extract
# and no uImage to wrap. That is two host-tool dependencies the ARM project
# needed (objcopy, mkimage) and this one does not. Pointing them at a
# failing command means a build step that starts needing one fails loudly
# instead of quietly producing a file nobody can boot.
UNAVAILABLE = "/bin/false"

# The target triple, the core, and the ABI.
#
# **`-mcpu=sifive-u74` rather than a bare `-march`**, because the JH7110's
# application cores *are* U74s and naming the core tells the compiler two
# things a bare ISA string cannot: the instruction set it implements
# (RV64GC — IMAFDC, which is what `rv64gc` would have said) **and its
# pipeline**, which is what scheduling instructions well depends on. The U74
# is a dual-issue in-order core, so the order of independent instructions is
# worth something; on an out-of-order core it would not be.
#
# The ISA is left exactly as the core defines it. The temptation is to add
# extensions — bit manipulation, vectors — but a JH7110 has neither, and an
# image built with them does not fault helpfully: it takes an illegal
# instruction somewhere in the middle of whatever used them, which is the
# worst way to find out.
TARGET_TRIPLE = "riscv64-unknown-elf"
MCPU = "sifive-u74"
MABI = "lp64d"
