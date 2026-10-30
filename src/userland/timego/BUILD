load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# file times on the root XFS: stat, utimensat, UTIME_OMIT.
go_binary(
    name = "timego",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
