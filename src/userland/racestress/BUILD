load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# The four-hart race's reproducer: ordinary Go, built the ordinary way, doing
# nothing but the operations that have been seen to fail.
#
# `pure` and `static` for the same reason as `gotest`: there is no dynamic
# loader and no libc here, and nothing about this target knows what it will
# run on.
go_binary(
    name = "racestress",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
