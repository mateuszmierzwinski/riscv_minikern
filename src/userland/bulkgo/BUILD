load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# netfs bulk reads, timed: the reproducer for stalls.
go_binary(
    name = "bulkgo",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
