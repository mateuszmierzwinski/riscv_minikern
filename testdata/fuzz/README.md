# Central fuzz corpus

Every committed fuzz input in the monorepo lives here, under the path of the
package that owns it:

    testdata/fuzz/<package path>/<FuzzName>/<content hash>

for example

    testdata/fuzz/src/go/purplemail/internal/dkim/FuzzVerify/06ba4bdb19de593e

One directory, so the whole corpus is one thing to sync between machines, carry
to CI, and review as a unit.

## Why it needs a rule to work

Go's fuzzer discovers seed inputs at `testdata/fuzz/<FuzzName>/` **relative to
the test's working directory**, and that location is not configurable — it is
baked into the `testing` package. Under Bazel the working directory is the
package's runfiles directory, so a corpus kept anywhere else is simply not
found.

`fuzz_corpus.bzl` bridges the gap. It copies a package's slice of this directory
into the per-package layout Go insists on, inside the test's runfiles, so the
source tree holds exactly one copy — this one.

```python
load("//testdata/fuzz:fuzz_corpus.bzl", "fuzz_corpus")

fuzz_corpus(
    name = "fuzz_corpus",
    package = "src/go/x/y",
)

go_test(
    name = "y_test",
    srcs = ["y_test.go"],
    data = [":fuzz_corpus"],
    embed = [":y"],
)
```

Adding a crasher needs no edit here or in `BUILD`: the glob picks it up, and the
next ordinary `bazel test` replays it as a subtest named after the file.

## Seed corpus vs cache corpus

This directory is the **seed** corpus: committed, small, and replayed on every
test run. It is the regression gate.

The fuzzer's **cache** corpus — the working set of inputs that reached a new
code path, kept so a run starts where the last one stopped — lives outside the
repository at `~/.cache/monorepo-fuzz/` and is never committed. Deleting it
costs time, never correctness.

`../../gen_fuzz.sh` runs a target and moves anything it finds into this
directory. The build itself never fuzzes.
