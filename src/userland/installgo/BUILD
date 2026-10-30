load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# installs a netfs tree and a kernel onto the SD card for an unattended local boot.
go_binary(
    name = "installgo",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
