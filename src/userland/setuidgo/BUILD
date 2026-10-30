load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# Go's syscall.Setuid, which changes every thread through a signal broadcast.
go_binary(
    name = "setuidgo",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
