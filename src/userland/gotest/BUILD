load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# A stock gc Go binary for linux/riscv64, built by the ordinary Go rules.
#
# `pure` and `static` because there is no dynamic loader and no libc here:
# the kernel loads one ELF and runs it. Nothing about this target knows what
# it will run on, which is the point of the test that uses it — the binary
# was not built for this kernel, it was built for Linux.
go_binary(
    name = "gotest",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
