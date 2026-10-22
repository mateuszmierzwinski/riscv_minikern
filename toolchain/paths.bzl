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

# The target triple and ISA. rv64gc is what the U74 cores in the JH7110
# implement (RV64GC = IMAFDC), and what QEMU's virt machine offers by default.
TARGET_TRIPLE = "riscv64-unknown-elf"
MARCH = "rv64gc"
MABI = "lp64d"
