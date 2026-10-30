load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# /proc, /sys and /dev from user space.
go_binary(
    name = "procgo",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
