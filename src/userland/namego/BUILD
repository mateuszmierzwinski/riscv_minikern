load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# processes are named after their program, and /proc/ps/by-name lists them.
go_binary(
    name = "namego",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
