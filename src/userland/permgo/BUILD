load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# appending to a file, the way init writes service logs.
go_binary(
    name = "permgo",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
