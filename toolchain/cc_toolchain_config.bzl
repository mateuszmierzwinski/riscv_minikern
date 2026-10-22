"""A freestanding C toolchain for the SiFive U74, over the host's clang.

The core is named rather than the ISA (toolchain/paths.bzl): `-mcpu=sifive-u74`
gives both the instruction set the JH7110's application cores implement and
the pipeline model its scheduler should assume.
"""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:cc_toolchain_config_lib.bzl", "feature", "flag_group", "flag_set", "tool_path")
load("@rules_cc//cc:defs.bzl", "CcToolchainConfigInfo", "cc_common")
load(":paths.bzl", "AR", "CLANG", "CLANG_INCLUDE", "LLD", "MABI", "MCPU", "NM", "TARGET_TRIPLE", "UNAVAILABLE")

_COMPILE_ACTIONS = [
    ACTION_NAMES.assemble,
    ACTION_NAMES.preprocess_assemble,
    ACTION_NAMES.c_compile,
]

_LINK_ACTIONS = [ACTION_NAMES.cpp_link_executable]

# Kernel code. Notable against the ARM project this replaces:
#
#   * **no soft-float flag.** On ARM the kernel was built soft-float so it
#     could not touch VFP state by accident before enabling it. RISC-V's F and
#     D registers are gated by one field (sstatus.FS) that starts off, so the
#     hardware enforces what a compiler flag had to there — and the kernel can
#     use the ABI the rest of the world uses.
#   * **-mcmodel=medany**, because the kernel is linked high and its code must
#     reach its data with PC-relative addressing from anywhere in the address
#     space.
#   * **-mno-relax**, so the assembler does not rewrite references the linker
#     script places by hand.
_COMPILE_FLAGS = [
    "--target=" + TARGET_TRIPLE,
    "-mcpu=" + MCPU,
    "-mabi=" + MABI,
    "-mcmodel=medany",
    "-mno-relax",
    # `tp` is the kernel's: it carries the hart id, which is how every
    # per-hart array is indexed and how the kernel lock knows who holds it.
    # Measured on this toolchain (2026-10-22): clang does **not** allocate x4
    # even under register pressure, because the ABI reserves it as the thread
    # pointer — so this flag changes nothing today. It is here to say the
    # register is dedicated, and to keep it dedicated if that convention ever
    # stops being honoured, since nothing in the kernel would fail loudly:
    # the hart id would simply be wrong.
    "-ffixed-x4",
    "-std=c17",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-pic",
    "-fno-common",
    "-fno-stack-protector",
    "-ffunction-sections",
    "-fdata-sections",
    "-O2",
    "-g",
    "-Wall",
    "-Wextra",
    "-Werror",
    # A kernel stack is small and a large frame on one is a bug, not a style.
    # On ARM this caught a 16 KiB structure being built on an 8 KiB stack by
    # anything that could send a packet to a closed port.
    "-Wframe-larger-than=1024",
]

_LINK_FLAGS = [
    "--target=" + TARGET_TRIPLE,
    "-mcpu=" + MCPU,
    "-mabi=" + MABI,
    "-mno-relax",
    "-nostdlib",
    "-static",
    "--ld-path=" + LLD,
    "-Wl,--gc-sections",
    "-Wl,--build-id=none",
    "-Wl,-z,noexecstack",
]

def _flags_feature(name, actions, flags):
    return feature(
        name = name,
        enabled = True,
        flag_sets = [flag_set(actions = actions, flag_groups = [flag_group(flags = flags)])],
    )

def _impl(ctx):
    tools = {
        "gcc": CLANG,
        "cpp": CLANG,
        "ld": LLD,
        "ar": AR,
        "nm": NM,
        "objcopy": UNAVAILABLE,
        "objdump": UNAVAILABLE,
        "strip": UNAVAILABLE,
        "gcov": UNAVAILABLE,
    }
    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        toolchain_identifier = "minikern-rv64gc",
        host_system_name = "x86_64-linux",
        target_system_name = TARGET_TRIPLE,
        target_cpu = "riscv64",
        target_libc = "none",
        compiler = "clang",
        abi_version = MABI,
        abi_libc_version = "none",
        tool_paths = [tool_path(name = k, path = v) for k, v in tools.items()],
        cxx_builtin_include_directories = [CLANG_INCLUDE],
        features = [
            _flags_feature("minikern_compile_flags", _COMPILE_ACTIONS, _COMPILE_FLAGS),
            _flags_feature("minikern_link_flags", _LINK_ACTIONS, _LINK_FLAGS),
        ],
    )

rv64gc_toolchain_config = rule(
    implementation = _impl,
    attrs = {},
    provides = [CcToolchainConfigInfo],
)
