# The monorepo's central fuzz corpus.
#
# Every crasher and every hand-added seed input lives here, under the path of
# the package that owns it, so the whole corpus is one directory to sync and one
# directory to review. //testdata/fuzz:fuzz_corpus.bzl stages a package's slice
# back into the per-package layout Go's fuzzer requires.
#
# Nothing here needs editing when a crasher is added: the glob picks it up.

exports_files(["fuzz_corpus.bzl"])

filegroup(
    name = "corpus",
    srcs = glob(
        ["**/*"],
        exclude = [
            "BUILD",
            "*.bzl",
            "README.md",
        ],
        allow_empty = True,
    ),
    visibility = ["//visibility:public"],
)
