load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# A dual-stack Go server: UDP and TCP echo on wildcard ports, which Go
# opens as AF_INET6 sockets once the kernel has them, so IPv4 clients
# arrive IPv4-mapped. Judged from the PC by the board suite.
go_binary(
    name = "dualgo",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
