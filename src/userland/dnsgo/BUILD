load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# A name resolved by a stock Go program on a card-booted board: Go's
# resolver stats and fstats /etc/resolv.conf, and the synthesised file
# answered neither, so Go asked [::1]:53. Judged by the board suite.
go_binary(
    name = "dnsgo",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
