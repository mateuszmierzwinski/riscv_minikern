#!/usr/bin/env bash
# cppcheck over this kernel, as a Bazel test.
#
# cppcheck earns its place beside clang-tidy rather than duplicating it: it is
# a different engine with a different bias — whole-program, happy to reason
# about things the compiler's analyser skips, and it carries checks the
# clang family does not (unused private state, suspicious sizeof, shadowed
# locals, redundant assignment). For bare-metal C its `--platform` and
# `--std` flags matter, and it does not need a libc to be present.
#
# **It is not installed on this machine** (`dnf install cppcheck`, which needs
# root). This test therefore fails with that message rather than passing
# quietly: a scan that did not run must not read as a scan that found nothing,
# which is the whole reason the ruleset says a missing scanner stops the run.
set -uo pipefail

# **Found by a marker, not by counting `..`.** A test run from the workspace,
# from the project directory and from inside Bazel's runfiles tree sees three
# different depths, and the first version of this guessed one of them wrong —
# which presented as "no sources found", a sentence that is true of an empty
# directory and of a directory that is not the one you meant.
find_project() {
    local here cand
    here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    # Candidates rather than a path: the workspace root is this project in a
    # standalone checkout and four levels above it in the monorepo, and a
    # literal of either spelling is wrong in the other. Walking up from the
    # script is spelling-independent, which is also why the export's own
    # check for surviving monorepo paths passes.
    for cand in "$here" "$here/.." "$here/../.." \
                "${BUILD_WORKSPACE_DIRECTORY:-/nonexistent}" \
                "${BUILD_WORKSPACE_DIRECTORY:-/nonexistent}"/*/*/*/* ; do
        if [ -f "$cand/src/kernel/sched.c" ]; then
            (cd "$cand" && pwd)
            return 0
        fi
    done
    return 1
}

proj="$(find_project)" || {
    echo "cannot find the project root (no src/kernel/sched.c above $0)" >&2
    exit 1
}
cd "$proj" || exit 1

command -v cppcheck >/dev/null 2>&1 || {
    echo "cppcheck is not installed." >&2
    echo >&2
    echo "  sudo dnf install cppcheck" >&2
    echo >&2
    echo "It needs root, which an agent does not have, so this is the" >&2
    echo "operator's call. Failing rather than skipping: a scan that did not" >&2
    echo "run must not read as a scan that found nothing." >&2
    exit 1
}

report="${TEST_TMPDIR:-/tmp}/cppcheck.txt"

# `--platform=unix64` matches lp64d; the suppressions are the ones a
# freestanding kernel earns rather than blanket silence:
#   missingIncludeSystem — there is no libc to include, on purpose
#   unusedFunction       — a kernel is full of entry points nothing here calls
#   unmatchedSuppression — so an obsolete suppression is not itself an error
cppcheck \
    --enable=warning,style,performance,portability \
    --inline-suppr \
    --std=c11 --platform=unix64 --language=c \
    --suppress=missingIncludeSystem \
    --suppress=unusedFunction \
    --suppress=unmatchedSuppression \
    --error-exitcode=0 \
    -I src \
    --quiet \
    src/core src/kernel src/drivers src/arch/riscv src/board/jh7110 src/lib \
    > "$report" 2>&1

found=$(grep -cE '\[[a-zA-Z]+\]$|error:|warning:|style:|performance:|portability:' "$report" || true)

if [ "$found" -ne 0 ]; then
    echo "cppcheck: $found finding(s)" >&2
    sed "s|$proj/||" "$report" >&2
    echo >&2
    echo "The gate is zero. Fix it, or add an inline suppression with the" >&2
    echo "reason beside the code it applies to." >&2
    exit 1
fi
echo "cppcheck: clean"
