load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# paths with . and .. from user space.
go_binary(
    name = "dotgo",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
