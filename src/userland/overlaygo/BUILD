load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# an overlay of two directories, mounted and changed through ordinary Go file calls.
go_binary(
    name = "overlaygo",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
