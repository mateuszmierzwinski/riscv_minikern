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
    name = "bootinfo_test",
    size = "small",
    srcs = ["test/bootinfo_test.c"],
    deps = [
        ":core_boot",
        ":ktest",
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
        "src/arch/riscv/sbi.c",
        "src/arch/riscv/start.S",
        "src/arch/riscv/trap.c",
        "src/arch/riscv/trap_entry.S",
    ],
    hdrs = [
        "src/arch/riscv/frame.h",
        "src/arch/riscv/layout.h",
        "src/arch/riscv/sbi.h",
        "src/arch/riscv/trap.h",
    ],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [":console"],
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
    ],
    hdrs = [
        "src/board/jh7110/console.h",
        "src/board/jh7110/selftest.h",
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
    ],
    alwayslink = True,
)

kernel_binary(
    name = "kernel",
    srcs = [],
    deps = [
        ":arch_riscv",
        ":board_jh7110",
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

test_suite(
    name = "qemu_tests",
    tags = ["manual"],
    tests = [
        ":boot_qemu_test",
        ":selftest_qemu_test",
    ],
)
