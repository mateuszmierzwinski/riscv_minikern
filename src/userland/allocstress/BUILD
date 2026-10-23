load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# The §0 corruption with every other moving part removed: one goroutine,
# allocating and verifying, nothing else.
#
# `pure` and `static` for the same reason as `gotest`: there is no dynamic
# loader and no libc here, and nothing about this target knows what it will
# run on.
go_binary(
    name = "allocstress",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
