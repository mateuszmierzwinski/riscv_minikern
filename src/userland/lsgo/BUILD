load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# lists directories named on the command line (a look at a disk before changing it).
go_binary(
    name = "lsgo",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
