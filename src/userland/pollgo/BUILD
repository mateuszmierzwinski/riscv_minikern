load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# ppoll and pselect6 from user space.
go_binary(
    name = "pollgo",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
