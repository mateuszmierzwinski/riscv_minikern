load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# Writes to the card measured and read back: one CMD24 per block ran under
# 0.3 MB/s, too slow for a file server's uploads; CMD25 streams them. The
# board suite runs it with and without `minikern.sdsinglewrites` (the
# control) and judges the content both ways ("§4: SD writes ...").
go_binary(
    name = "sdwrite",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
