# compiler-rt builtins: the quad-precision `long double` routines a riscv64
# program needs and Fedora does not ship for this target.
#
# On riscv64 `long double` is IEEE binary128, done in software. musl uses it
# in `printf`, `strtold` and its math, so a static link with no compiler
# runtime fails on these 16 symbols (`__addtf3`, `__multf3`, ...). Only the
# files that define them are vendored, plus `riscv/fp_mode.c` for the
# rounding mode they consult (PROVENANCE.md).

load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

licenses(["notice"])  # Apache-2.0 WITH LLVM-exception, LICENSE.TXT

exports_files(["LICENSE.TXT"])

cc_library(
    name = "builtins",
    srcs = glob([
        "lib/builtins/*.c",
        "lib/builtins/*.h",
        "lib/builtins/*.inc",
        "lib/builtins/riscv/*.c",
    ]),
    # Upstream code: the toolchain's -Werror is for ours.
    copts = ["-w"],
    target_compatible_with = ["//platforms:riscv64"],
)
