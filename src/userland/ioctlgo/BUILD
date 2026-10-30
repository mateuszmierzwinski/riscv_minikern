load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# ioctl from user space: terminal attributes, FIONREAD, FIOCLEX, FIONBIO.
go_binary(
    name = "ioctlgo",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
