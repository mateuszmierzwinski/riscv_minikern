"""Staging for the monorepo's central fuzz corpus.

Go's fuzzer discovers seed inputs at "testdata/fuzz/<FuzzName>/" relative to the
test's working directory — which under Bazel is the package's runfiles
directory — and that location is not configurable. It is baked into the
testing package.

The corpus is nevertheless kept centrally, at //testdata/fuzz/<package path>,
because one directory is what can be synced between machines, reviewed as a
unit, and carried to CI. This rule bridges the two: it copies a package's slice
of the central corpus into the per-package layout Go insists on, inside the
test's runfiles, so nothing is duplicated in the source tree.

    load("//testdata/fuzz:fuzz_corpus.bzl", "fuzz_corpus")

    fuzz_corpus(name = "fuzz_corpus", package = "src/go/x/y")

    go_test(
        name = "y_test",
        srcs = ["y_test.go"],
        data = [":fuzz_corpus"],
        embed = [":y"],
    )
"""

# CORPUS_ROOT is both where the files are kept and the path Go looks for them
# under. They are the same string by coincidence, not by dependency.
_CORPUS_ROOT = "testdata/fuzz"

def _staging_script(output, sources):
    """Returns the shell that reproduces the corpus layout under output."""
    lines = ["set -e", 'mkdir -p "%s"' % output.path]
    for relative, source in sources:
        directory = relative.rsplit("/", 1)[0] if "/" in relative else ""
        if directory:
            lines.append('mkdir -p "%s/%s"' % (output.path, directory))
        lines.append('cp -f "%s" "%s/%s"' % (source.path, output.path, relative))
    return "\n".join(lines)

def _slice_for_package(corpus, package):
    """Returns (path relative to the fuzz name root, file) for one package."""
    prefix = _CORPUS_ROOT + "/" + package + "/"
    return [(f.short_path[len(prefix):], f)
            for f in corpus
            if f.short_path.startswith(prefix)]

def _fuzz_corpus_impl(ctx):
    output = ctx.actions.declare_directory(_CORPUS_ROOT)
    sources = _slice_for_package(ctx.files.corpus, ctx.attr.package)

    # An empty corpus is the normal state for a package that has been fuzzed
    # and found nothing. It must still produce the directory, or every such
    # package fails to build the day fuzzing is introduced.
    ctx.actions.run_shell(
        inputs = [source for _, source in sources],
        outputs = [output],
        command = _staging_script(output, sources),
        mnemonic = "StageFuzzCorpus",
        progress_message = "Staging fuzz corpus for %s" % ctx.attr.package,
    )
    return [DefaultInfo(
        files = depset([output]),
        runfiles = ctx.runfiles(files = [output]),
    )]

fuzz_corpus = rule(
    implementation = _fuzz_corpus_impl,
    doc = "Stages one package's slice of //testdata/fuzz into its runfiles.",
    attrs = {
        "package": attr.string(
            mandatory = True,
            doc = "Workspace-relative package path, e.g. src/go/x/y.",
        ),
        "corpus": attr.label(
            default = "//testdata/fuzz:corpus",
            allow_files = True,
            doc = "The central corpus. Overridden only by tests of this rule.",
        ),
    },
)
