load("@io_bazel_rules_go//go:def.bzl", "go_binary")

go_binary(
    name = "childgo",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
