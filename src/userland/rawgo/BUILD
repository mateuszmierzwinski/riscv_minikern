load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# the console in raw mode: termios, VMIN/VTIME, ^C as data.
go_binary(
    name = "rawgo",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
