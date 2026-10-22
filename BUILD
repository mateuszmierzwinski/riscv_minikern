load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load("@rules_shell//shell:sh_test.bzl", "sh_test")
load("//:version.bzl", "MINIKERN_VERSION")
load(":riscv_minikern.bzl", "kernel_binary", "kernel_image")

# minikern for RISC-V — a from-scratch kernel for the StarFive JH7110
# (SiFive U74), implementing Linux's syscall interface so that Go binaries
# run unchanged. The migration of github.com/mateuszmierzwinski/arm_minikern
# off 32-bit ARM; see README.md and TASKS.md.
package(default_visibility = ["//visibility:private"])

exports_files(["src/arch/riscv/link.ld"])

# ── portable code: the same sources build for the host and for the board ───
#
# Every target here is architecture-neutral by construction — src/core
# includes nothing from arch/ or board/ — which is why the host tests below
# run on day one of the port rather than at the end of it.

cc_library(
    name = "kfmt",
    srcs = ["src/lib/kfmt.c"],
    hdrs = ["src/lib/kfmt.h"],
    strip_include_prefix = "src",
)

cc_library(
    name = "core_boot",
    srcs = [
        "src/core/bootinfo.c",
        "src/core/cmdline.c",
        "src/core/fdt.c",
    ],
    hdrs = [
        "src/core/bootinfo.h",
        "src/core/cmdline.h",
        "src/core/errno.h",
        "src/core/fdt.h",
        "src/core/types.h",
    ],
    strip_include_prefix = "src",
)

cc_library(
    name = "console",
    srcs = ["src/core/console.c"],
    hdrs = [
        "src/core/console.h",
        "src/core/ports.h",
    ],
    strip_include_prefix = "src",
    deps = [":kfmt"],
)

# The physical page allocator and the kernel heap: carried over, pure, and
# host-tested. Widened to 64-bit physical addresses, because DRAM on this
# board starts at 0x4000_0000 and can run past 0x2_0000_0000.
cc_library(
    name = "memory",
    srcs = [
        "src/core/kheap.c",
        "src/core/pmm.c",
    ],
    hdrs = [
        "src/core/kheap.h",
        "src/core/pmm.h",
        "src/core/ports.h",
    ],
    strip_include_prefix = "src",
    deps = [":core_boot"],
)

cc_test(
    name = "pmm_test",
    size = "small",
    srcs = ["test/pmm_test.c"],
    deps = [
        ":ktest",
        ":memory",
    ],
)

# The block layer and the filesystem, carried over whole: a block device
# port, a buffer cache over it, and XFS. All pure, all host-tested.
cc_library(
    name = "storage",
    srcs = [
        "src/core/bcache.c",
        "src/core/blockscale.c",
    ],
    hdrs = [
        "src/core/bcache.h",
        "src/core/blockdev.h",
        "src/core/blockscale.h",
    ],
    strip_include_prefix = "src",
    deps = [":core_boot"],
)

# XFS, carried over whole: mount, read, logged writes, create, remove, grow.
# Pure over the buffer cache, and host-tested against filesystems mkfs.xfs
# made — which is the property that matters, because the authority on whether
# this implementation is right is the vendor's own checker, not this project.
cc_library(
    name = "xfs",
    srcs = glob(["src/core/xfs/*.c"]),
    hdrs = glob(["src/core/xfs/*.h"]),
    strip_include_prefix = "src",
    deps = [
        ":console",
        ":core_boot",
        ":crypto",
        ":storage",
    ],
)

cc_library(
    name = "crypto",
    srcs = ["src/lib/crc32c.c"],
    hdrs = ["src/lib/crc32c.h"],
    strip_include_prefix = "src",
)

cc_test(
    name = "xfs_test",
    size = "small",
    srcs = ["test/xfs_test.c"],
    deps = [
        ":ktest",
        ":xfs",
    ],
)

cc_test(
    name = "blockscale_test",
    size = "small",
    srcs = ["test/blockscale_test.c"],
    deps = [
        ":ktest",
        ":storage",
    ],
)

cc_test(
    name = "bcache_test",
    size = "small",
    srcs = ["test/bcache_test.c"],
    deps = [
        ":ktest",
        ":storage",
    ],
)

# Sv39 translation, as pure arithmetic: no CSR is touched here, which is what
# lets it be tested on the host before any hardware installs a table.
cc_library(
    name = "pgtable",
    srcs = ["src/arch/riscv/pgtable.c"],
    hdrs = ["src/arch/riscv/pgtable.h"],
    strip_include_prefix = "src",
    deps = [":core_boot"],
)

# ── host tests, which need no board and no emulator ────────────────────────

cc_test(
    name = "kfmt_test",
    size = "small",
    srcs = ["test/kfmt_test.c"],
    deps = [
        ":kfmt",
        ":ktest",
    ],
)

cc_library(
    name = "ktest",
    testonly = True,
    hdrs = [
        "test/fdt_build.h",
        "test/ktest.h",
    ],
    strip_include_prefix = "test",
)

cc_test(
    name = "cmdline_test",
    size = "small",
    srcs = ["test/cmdline_test.c"],
    deps = [
        ":core_boot",
        ":ktest",
    ],
)

cc_test(
    name = "fdt_test",
    size = "small",
    srcs = ["test/fdt_test.c"],
    deps = [
        ":core_boot",
        ":ktest",
    ],
)

cc_test(
    name = "pgtable_test",
    size = "small",
    srcs = ["test/pgtable_test.c"],
    deps = [
        ":ktest",
        ":pgtable",
    ],
)

cc_test(
    name = "bootinfo_test",
    size = "small",
    srcs = ["test/bootinfo_test.c"],
    deps = [
        ":core_boot",
        ":ktest",
    ],
)

# The live memory: one frame allocator and one heap over what the device tree
# described. The pure parts are :memory above, which the host tests cover.
cc_library(
    name = "kernel_mem",
    srcs = ["src/kernel/mem.c"],
    hdrs = ["src/kernel/mem.h"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":console",
        ":memory",
        ":pgtable",
    ],
)

# Drivers for devices an emulator models faithfully. They exist so that the
# storage and network stacks — carried over with host tests but never run
# against a real device on this architecture — are exercised before the
# board's own controllers are written, which means a later failure in an SD
# or NVMe driver is in that driver rather than in the filesystem above it.
# PCI Express and the M.2 slot behind it. The bus walker assigns the memory
# windows itself, because no firmware here has: a device whose BARs are still
# zero answers configuration reads perfectly and ignores its registers.
# The board's pins. The arithmetic that maps a pin to a register is pure and
# host-tested, because the JH7110 uses a *byte* per pin where a bit would be
# conventional — and writing a bit where a byte belongs lands on a
# neighbouring pin, which no boot log would show. The register *behaviour* is
# not tested and cannot be: no emulator models this controller.
cc_library(
    name = "drivers_gpio",
    srcs = [
        "src/drivers/crg_jh7110.c",
        "src/drivers/gpio_jh7110.c",
    ],
    hdrs = [
        "src/drivers/crg_jh7110.h",
        "src/drivers/gpio_jh7110.h",
    ],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":arch_riscv",
        ":console",
    ],
)

# The pure halves, which the host can judge.
cc_library(
    name = "gpio_offsets",
    hdrs = [
        "src/drivers/crg_jh7110.h",
        "src/drivers/gpio_jh7110.h",
    ],
    strip_include_prefix = "src",
    deps = [":core_boot"],
)

cc_test(
    name = "crg_jh7110_test",
    size = "small",
    srcs = ["test/crg_jh7110_test.c"],
    deps = [
        ":gpio_offsets",
        ":ktest",
    ],
)

cc_test(
    name = "gpio_jh7110_test",
    size = "small",
    srcs = ["test/gpio_jh7110_test.c"],
    deps = [
        ":gpio_offsets",
        ":ktest",
    ],
)

cc_library(
    name = "drivers_pci",
    srcs = [
        "src/drivers/nvme.c",
        "src/drivers/pci.c",
        "src/drivers/sdhci.c",
    ],
    hdrs = [
        "src/drivers/nvme.h",
        "src/drivers/pci.h",
        "src/drivers/sdhci.h",
    ],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":arch_riscv",
        ":console",
        ":kernel_mem",
        ":storage",
    ],
)

cc_library(
    name = "drivers_virtio",
    srcs = [
        "src/drivers/virtio.c",
        "src/drivers/virtio_blk.c",
    ],
    hdrs = [
        "src/drivers/virtio.h",
        "src/drivers/virtio_blk.h",
    ],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":arch_riscv",
        ":console",
        ":kernel_mem",
        ":storage",
    ],
)

# The live block device and the cache over it.
cc_library(
    name = "kernel_disk",
    srcs = ["src/kernel/disk.c"],
    hdrs = ["src/kernel/disk.h"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":console",
        ":drivers_pci",
        ":drivers_virtio",
        ":kernel_mem",
        ":storage",
    ],
)

# The filesystem on the disk: the carried-over XFS over the live cache.
cc_library(
    name = "kernel_xfs",
    srcs = ["src/kernel/xfsmount.c"],
    hdrs = ["src/kernel/xfsmount.h"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":console",
        ":kernel_disk",
        ":kernel_mem",
        ":xfs",
    ],
)

# ── the kernel ─────────────────────────────────────────────────────────────

# memset and friends: the kernel links no libc, and the compiler emits calls
# to these for a structure initialiser. Kernel-only — the host tests above
# use the host's, which is why this is not in :kfmt.
cc_library(
    name = "kmem",
    srcs = ["src/lib/kmem.c"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
)

cc_library(
    name = "arch_riscv",
    srcs = [
        "src/arch/riscv/mmu.c",
        "src/arch/riscv/plic.c",
        "src/arch/riscv/sbi.c",
        "src/arch/riscv/start.S",
        "src/arch/riscv/timer.c",
        "src/arch/riscv/trap.c",
        "src/arch/riscv/trap_entry.S",
        "src/kernel/irq.c",
    ],
    hdrs = [
        "src/arch/riscv/frame.h",
        "src/arch/riscv/layout.h",
        "src/arch/riscv/mmu.h",
        "src/arch/riscv/plic.h",
        "src/arch/riscv/sbi.h",
        "src/arch/riscv/timer.h",
        "src/arch/riscv/trap.h",
        "src/kernel/irq.h",
    ],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":console",
        ":pgtable",
    ],
    # _start is referenced by nothing, so without this the linker drops it
    # from the archive and the image boots into whatever follows.
    alwayslink = True,
)

cc_library(
    name = "board_jh7110",
    srcs = [
        "src/board/jh7110/console.c",
        "src/board/jh7110/main.c",
        "src/board/jh7110/selftest.c",
        "src/board/jh7110/uart.c",
    ],
    hdrs = [
        "src/board/jh7110/console.h",
        "src/board/jh7110/selftest.h",
        "src/board/jh7110/uart.h",
        "src/board/jh7110/version.h",
    ],
    # The series comes from the monorepo's single constant (Rule 28). It has
    # to be defined on the target that compiles main.c, not on the binary:
    # a local_define does not reach a dependency.
    local_defines = ['MINIKERN_VERSION=\\"%s\\"' % MINIKERN_VERSION],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":arch_riscv",
        ":console",
        ":core_boot",
        ":drivers_gpio",
        ":drivers_pci",
        ":drivers_virtio",
        ":kernel_disk",
        ":kernel_mem",
        ":kernel_xfs",
    ],
    alwayslink = True,
)

kernel_binary(
    name = "kernel",
    srcs = [],
    deps = [
        ":arch_riscv",
        ":board_jh7110",
        ":drivers_gpio",
        ":drivers_pci",
        ":drivers_virtio",
        ":kernel_disk",
        ":kernel_mem",
        ":kernel_xfs",
        ":kmem",
    ],
)

kernel_image(
    name = "minikern",
    kernel = ":kernel",
    tags = ["manual"],
)

# ── the emulator ───────────────────────────────────────────────────────────
#
# Unlike the board this kernel targets, QEMU's virt machine models everything
# the kernel needs: SBI (through OpenSBI), the PLIC, the CLINT and virtio. So
# the whole bring-up is verifiable here, which is the opposite of the
# position the ARM project ended in with three stubbed registers.

sh_test(
    name = "boot_qemu_test",
    size = "medium",
    srcs = ["test/boot_qemu_test.sh"],
    args = ["$(rootpath :minikern)"],
    data = [":minikern"],
    tags = ["manual"],
)

# P0.3: the trap path, proved by taking deliberate faults.
sh_test(
    name = "selftest_qemu_test",
    size = "medium",
    srcs = ["test/selftest_qemu_test.sh"],
    args = ["$(rootpath :minikern)"],
    data = [":minikern"],
    tags = ["manual"],
)

# P4.1: a real block device, and the first thing on this architecture to
# exercise the carried-over buffer cache against one.
sh_test(
    name = "disk_qemu_test",
    size = "medium",
    srcs = ["test/disk_qemu_test.sh"],
    args = ["$(rootpath :minikern)"],
    data = [":minikern"],
    tags = ["manual"],
)

# P4.4: XFS on a real device — the whole stack, judged against filesystems
# the vendor's own tools made and call clean.
sh_test(
    name = "xfs_qemu_test",
    size = "large",
    srcs = ["test/xfs_qemu_test.sh"],
    args = ["$(rootpath :minikern)"],
    data = [":minikern"],
    tags = ["manual"],
)

test_suite(
    name = "qemu_tests",
    tags = ["manual"],
    tests = [
        ":boot_qemu_test",
        ":disk_qemu_test",
        ":selftest_qemu_test",
        ":xfs_qemu_test",
    ],
)
