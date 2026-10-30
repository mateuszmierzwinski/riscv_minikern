load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# /proc/stat splits user time from system time.
go_binary(
    name = "cpugo",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
