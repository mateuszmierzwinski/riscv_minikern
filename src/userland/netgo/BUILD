load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# Go's `net` package, unchanged, on this kernel. It needs more of the kernel
# than anything else: a descriptor layer, a real epoll, sockets over the IPv4
# stack, and non-blocking semantics throughout.
go_binary(
    name = "netgo",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
