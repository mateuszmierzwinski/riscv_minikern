load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# The init of a throwaway Linux guest that leaves an XFS log dirty the way
# Linux writes it (scripts/make_linux_log.sh). Built for the host's
# architecture, because the guest is the host's own kernel under QEMU.
go_binary(
    name = "init",
    srcs = ["main.go"],
    goarch = "amd64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
