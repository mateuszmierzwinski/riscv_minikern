load("@rules_cc//cc:defs.bzl", "cc_toolchain")
load(":cc_toolchain_config.bzl", "rv64gc_toolchain_config")

package(default_visibility = ["//:__subpackages__"])

exports_files(["paths.bzl"])

filegroup(name = "empty")

rv64gc_toolchain_config(name = "rv64gc_config")

cc_toolchain(
    name = "rv64gc_cc",
    all_files = ":empty",
    compiler_files = ":empty",
    dwp_files = ":empty",
    linker_files = ":empty",
    objcopy_files = ":empty",
    strip_files = ":empty",
    supports_param_files = 0,
    toolchain_config = ":rv64gc_config",
)

# Selected only for the board platform, which no host build asks for: the
# image rule transitions into it.
toolchain(
    name = "rv64gc_toolchain",
    target_compatible_with = ["//platforms:riscv64"],
    toolchain = ":rv64gc_cc",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
