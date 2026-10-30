load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# file times from netfs and FAT through stat.
go_binary(
    name = "timesgo",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
