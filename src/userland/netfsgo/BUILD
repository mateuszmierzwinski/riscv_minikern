load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# netfs from user space: list, read, write, rename and remove under /net.
go_binary(
    name = "netfsgo",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
