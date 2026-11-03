load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# The card's three largest programs read at once: the SD controller let a
# second command start while the first slept for its interrupt, and loading
# three QuokkaOS services together ran one of them on another's bytes.
# Judged by the board suite ("§4: concurrent reads from the SD card ...").
go_binary(
    name = "sdconcurrent",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
