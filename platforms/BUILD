# The machines this kernel is built for. The constraint is the project's own:
# the root module does not register @platforms, and a kernel target has
# nothing in common with a host platform anyway (no OS, no libc).
package(default_visibility = ["//:__subpackages__"])

constraint_setting(name = "machine")

# One ISA constraint, because one image runs on both machines below: the
# differences are peripherals, which the device tree describes at run time.
constraint_value(
    name = "riscv64",
    constraint_setting = ":machine",
    # The C library and compiler runtime its user programs link are vendored
    # under third_party/c and built only for this machine.
    visibility = [
        "//:__subpackages__",
        "//third_party/c/compiler_rt_builtins:__pkg__",
        "//third_party/c/musl:__pkg__",
    ],
)

# The StarFive JH7110: one S7 monitor core plus four U74 application cores —
# the VisionFive 2.
platform(
    name = "visionfive2",
    constraint_values = [":riscv64"],
)

# QEMU's virt machine, which models everything this kernel needs (PLIC, CLINT,
# SBI, virtio) and is therefore where the work is verified.
platform(
    name = "qemu_virt",
    constraint_values = [":riscv64"],
)
