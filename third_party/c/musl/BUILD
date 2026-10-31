# musl 1.2.5 for minikern's user space: a static libc for riscv64 programs.
#
# Built the way musl's own Makefile builds it, rule for rule, so that what
# runs on the board is musl and not an approximation of it:
#   - every `src/<dir>/*.c` (and `src/malloc/mallocng`), except where
#     `src/<dir>/riscv64/` has a file of the same stem, which replaces it;
#   - the generated headers (`bits/alltypes.h`, `bits/syscall.h`,
#     `version.h`) made by the same `sed` lines;
#   - musl's include order and C99/freestanding flags, `-O3` for
#     `internal`, `malloc` and `string` (`OPTIMIZE_GLOBS`).
# Only the riscv64 and generic arch trees are vendored (PROVENANCE.md).
#
# Everything is built for the board's platform only: a host build of a libc
# for another machine has no meaning.

load("@rules_cc//cc:defs.bzl", "cc_library")
load(":musl.bzl", "musl_optimized", "musl_public_headers", "musl_sources")

package(default_visibility = ["//visibility:public"])

licenses(["notice"])  # MIT, COPYRIGHT

exports_files(["COPYRIGHT"])

_RV64 = ["//platforms:riscv64"]

_ARCH = "riscv64"

genrule(
    name = "alltypes_h",
    srcs = [
        "arch/riscv64/bits/alltypes.h.in",
        "include/alltypes.h.in",
        "tools/mkalltypes.sed",
    ],
    outs = ["obj/include/bits/alltypes.h"],
    cmd = "sed -f $(location tools/mkalltypes.sed) $(location arch/riscv64/bits/alltypes.h.in) " +
          "$(location include/alltypes.h.in) > $@",
)

genrule(
    name = "syscall_h",
    srcs = ["arch/riscv64/bits/syscall.h.in"],
    outs = ["obj/include/bits/syscall.h"],
    cmd = "cp $< $@ && sed -n -e s/__NR_/SYS_/p < $< >> $@",
)

genrule(
    name = "version_h",
    srcs = ["VERSION"],
    outs = ["obj/src/internal/version.h"],
    cmd = "printf '#define VERSION \"%s\"\\n' \"$$(cat $<)\" > $@",
)

_GENERATED = [
    ":alltypes_h",
    ":syscall_h",
    ":version_h",
]

# Every header the build may include, so the sandbox has them all.
_PRIVATE_HEADERS = glob([
    "arch/riscv64/**/*.h",
    "arch/generic/**/*.h",
    "src/**/*.h",
    "include/**/*.h",
]) + _GENERATED

# Sources musl includes from other sources (`libc_calloc.c` includes
# `calloc.c`): inputs to every compile, never compiled on their own account.
_INCLUDED_SOURCES = glob(["src/**/*.c", "ldso/*.c"])

_HERE = "third_party/c/musl"

# musl's CFLAGS_C99FSE, CFLAGS_ALL include order and the CFLAGS_AUTO it
# chose for clang. `-w` as musl's configure sets it: this is upstream code,
# and the toolchain's `-Werror` is for ours.
_COPTS = [
    "-std=c99",
    "-nostdinc",
    "-ffreestanding",
    "-frounding-math",
    "-fno-strict-aliasing",
    "-D_XOPEN_SOURCE=700",
    "-I" + _HERE + "/arch/" + _ARCH,
    "-I" + _HERE + "/arch/generic",
    "-I$(GENDIR)/" + _HERE + "/obj/src/internal",
    "-I" + _HERE + "/src/include",
    "-I" + _HERE + "/src/internal",
    "-I$(GENDIR)/" + _HERE + "/obj/include",
    "-I" + _HERE + "/include",
    "-fomit-frame-pointer",
    "-fno-unwind-tables",
    "-fno-asynchronous-unwind-tables",
    "-fno-stack-protector",
    "-Wa,--noexecstack",
    "-w",
]

_SRCS = musl_sources(_ARCH)

cc_library(
    name = "c_optimized",
    srcs = [s for s in _SRCS if musl_optimized(s)] + _PRIVATE_HEADERS,
    copts = _COPTS + ["-O3"],
    target_compatible_with = _RV64,
    textual_hdrs = _INCLUDED_SOURCES,
)

cc_library(
    name = "c",
    srcs = [s for s in _SRCS if not musl_optimized(s)] + _PRIVATE_HEADERS,
    copts = _COPTS,
    target_compatible_with = _RV64,
    textual_hdrs = _INCLUDED_SOURCES,
    deps = [
        ":c_optimized",
        "//third_party/c/compiler_rt_builtins:builtins",
    ],
)

# `_start`: always linked, or nothing would ever reference it.
cc_library(
    name = "crt1",
    srcs = ["crt/crt1.c"] + _PRIVATE_HEADERS,
    copts = _COPTS + ["-DCRT"],
    alwayslink = True,
    target_compatible_with = _RV64,
    textual_hdrs = _INCLUDED_SOURCES,
    deps = [":c"],
)

# The installed header tree a program compiles against, as `make install`
# lays it out: `include/`, then each `bits/` header from the riscv64 tree,
# or from the generic one where riscv64 has none, then the generated ones.
_PUBLIC = musl_public_headers(_ARCH)

_SYSROOT = "sysroot/include/"

genrule(
    name = "sysroot_headers",
    srcs = list(_PUBLIC.values()) + [
        ":alltypes_h",
        ":syscall_h",
    ],
    outs = [_SYSROOT + rel for rel in _PUBLIC.keys()] + [
        _SYSROOT + "bits/alltypes.h",
        _SYSROOT + "bits/syscall.h",
    ],
    cmd = " && ".join([
        "mkdir -p $$(dirname $(RULEDIR)/{to}) && cp $(location {src}) $(RULEDIR)/{to}".format(
            src = src,
            to = _SYSROOT + rel,
        )
        for rel, src in _PUBLIC.items()
    ] + [
        "cp $(location :alltypes_h) $(RULEDIR)/" + _SYSROOT + "bits/alltypes.h",
        "cp $(location :syscall_h) $(RULEDIR)/" + _SYSROOT + "bits/syscall.h",
    ]),
)

cc_library(
    name = "headers",
    hdrs = [":sysroot_headers"],
    includes = ["sysroot/include"],
    target_compatible_with = _RV64,
)
