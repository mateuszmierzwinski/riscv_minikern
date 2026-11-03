load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# The storage a file server needs, asked the way a Go program asks it:
# /proc/volumes, mount(2) of another disk's volume read-only, umount2's
# EBUSY while a directory is open, the refusals, and a file larger than the
# whole-file limit read where it is asked for (over netfs). Never writes a
# block volume. Judged by the board suite ("§3: volumes ...").
go_binary(
    name = "volgo",
    srcs = ["main.go"],
    goarch = "riscv64",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
