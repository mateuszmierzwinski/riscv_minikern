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

def kernel_binary(name, srcs, deps, **kwargs):
    """A cc_binary with the kernel's link script and the version stamped in."""
    cc_binary(
        name = name,
        srcs = srcs,
        additional_linker_inputs = ["src/arch/riscv/link.ld"],
        linkopts = [
            "-Wl,-T,$(location src/arch/riscv/link.ld)",
        ],
        local_defines = ['MINIKERN_VERSION=\\"%s\\"' % MINIKERN_VERSION],
        target_compatible_with = ["//platforms:riscv64"],
        deps = deps,
        **kwargs
    )
