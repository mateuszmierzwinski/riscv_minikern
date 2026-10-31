"""The kernel image: a cc_binary built for the board, through a transition.

The same shape the ARM project used, with two differences that are the point
of the port:

  * **no uImage.** SBI firmware takes an ELF payload directly (and U-Boot's
    `booti` takes a flat binary), so the mkimage wrapper and its host-tool
    dependency are gone.
  * **one image for both machines.** QEMU's virt and the VisionFive 2 differ
    in peripherals, which the device tree describes at run time, not in the
    ISA or the load address.
"""

load("@rules_cc//cc:defs.bzl", "cc_binary")
load("//:version.bzl", "MINIKERN_VERSION")

_PLATFORM = "//platforms:visionfive2"
_TOOLCHAIN = "//toolchain:rv64gc_toolchain"

def _board_transition_impl(_settings, _attr):
    return {
        "//command_line_option:platforms": _PLATFORM,
        "//command_line_option:extra_toolchains": [_TOOLCHAIN],
    }

_board_transition = transition(
    implementation = _board_transition_impl,
    inputs = [],
    outputs = [
        "//command_line_option:platforms",
        "//command_line_option:extra_toolchains",
    ],
)

def _kernel_image_impl(ctx):
    src = ctx.attr.kernel[0][DefaultInfo].files.to_list()[0]
    out = ctx.actions.declare_file(ctx.label.name + ".elf")
    ctx.actions.symlink(output = out, target_file = src)
    return [DefaultInfo(files = depset([out]))]

kernel_image = rule(
    implementation = _kernel_image_impl,
    attrs = {
        "kernel": attr.label(cfg = _board_transition, mandatory = True),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
    doc = "Builds the kernel for the board platform and names the ELF the firmware loads.",
)

def kernel_binary(name, srcs, deps, load_address = None, binary = False, **kwargs):
    """A cc_binary with the kernel's link script and the version stamped in.

    `load_address` overrides where the image is linked. The default is QEMU's
    `0x80200000`; a real VisionFive 2 has DRAM at `0x40000000`, so its image
    links at `0x40200000` — measured from the board's own U-Boot, which
    reports `kernel_addr_r=0x40200000`.

    `binary` emits a **flat image** rather than an ELF, which is what a
    loader's `booti` takes. The linker produces it directly
    (`--oformat=binary`), which matters here: Fedora's binutils cannot read a
    RISC-V object at all, so there is no `objcopy` to convert one with.
    """
    linkopts = ["-Wl,-T,$(location src/arch/riscv/link.ld)"]
    if load_address:
        linkopts.append("-Wl,--defsym=KERNEL_LOAD=%s" % load_address)
    if binary:
        linkopts.append("-Wl,--oformat=binary")
    cc_binary(
        name = name,
        srcs = srcs,
        additional_linker_inputs = ["src/arch/riscv/link.ld"],
        linkopts = linkopts,
        local_defines = ['MINIKERN_VERSION=\\"%s\\"' % MINIKERN_VERSION],
        target_compatible_with = ["//platforms:riscv64"],
        deps = deps,
        **kwargs
    )

def user_binary(name, srcs, **kwargs):
    """A user-mode program: the same toolchain, a different address.

    It links at 0x10000 with its own script and depends on nothing from the
    kernel — which is the point. A program built out of the kernel's own
    headers would prove the loader works on something tailored to it; this
    one is an ordinary static ELF64 that happens to have been built here.
    """
    cc_binary(
        name = name,
        srcs = srcs,
        additional_linker_inputs = ["src/userland/user.ld"],
        linkopts = ["-Wl,-T,$(location src/userland/user.ld)"],
        target_compatible_with = ["//platforms:riscv64"],
        **kwargs
    )

def musl_binary(name, srcs, deps = [], **kwargs):
    """A user-mode program written against a C library: musl, linked static.

    Unlike `user_binary` it has no link script of its own — `lld`'s default
    layout starts at 0x10000 and keeps the program headers in the first
    segment, which is what `AT_PHDR` needs — and `_start` is musl's
    (`crt1`), so `main` is an ordinary `main`. Nothing in it knows it runs
    on minikern: that is the test of the syscall interface it exercises.
    """
    cc_binary(
        name = name,
        srcs = srcs,
        deps = deps + [
            "//third_party/c/musl:crt1",
            "//third_party/c/musl:c",
            "//third_party/c/musl:headers",
        ],
        target_compatible_with = ["//platforms:riscv64"],
        **kwargs
    )

def user_image(name, program):
    """Builds a user program for the board platform and names its ELF."""
    kernel_image(name = name, kernel = program)
