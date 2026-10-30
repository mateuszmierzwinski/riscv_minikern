load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# a pipe read through Go's poller, as os/exec copies a child's output.
go_binary(
    name = "pipego",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
