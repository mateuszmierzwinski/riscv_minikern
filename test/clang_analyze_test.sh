#!/usr/bin/env bash
# The Clang static analyser over this kernel, as a Bazel test.
#
# `scan-build` is the usual front end for this and it is **not installed here**
# (it ships in `clang-analyzer`, which needs root). It is only a driver: it
# sets up a fake compiler, intercepts a build, and writes HTML. The analysis
# itself is `clang --analyze`, which is in the compiler already — so this runs
# that directly, per source file, and reports plainly instead of rendering a
# report nobody opens.
#
# Why this is separate from clang-tidy even though tidy also runs the analyser:
# tidy runs it under its own driver and check filter, and the two disagree
# about which checkers are on by default. Running the analyser on its own terms
# is cheap and catches the difference.
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

command -v clang >/dev/null 2>&1 || { echo "clang is not installed" >&2; exit 1; }

resource="$(clang -print-resource-dir)"
flags=(--analyze -Xclang -analyzer-output=text
       -Isrc --target=riscv64-unknown-elf -march=rv64gc -mabi=lp64d
       -ffreestanding -nostdinc -isystem "$resource/include")

# The checkers that matter for this code. `security.insecureAPI` is off because
# it is about libc functions a freestanding kernel does not have, and
# `optin.performance` is a layout preference.
flags+=(-Xclang -analyzer-disable-checker -Xclang security.insecureAPI)

mapfile -t sources < <(ls src/core/*.c src/kernel/*.c src/drivers/*.c \
                          src/arch/riscv/*.c src/board/jh7110/*.c src/lib/*.c 2>/dev/null)
[ ${#sources[@]} -gt 0 ] || { echo "no sources found under $proj" >&2; exit 1; }

report="${TEST_TMPDIR:-/tmp}/clang-analyze.txt"
: > "$report"
for src in "${sources[@]}"; do
    clang "${flags[@]}" "$src" >> "$report" 2>&1
done
found=$(grep -cE 'warning:|error:' "$report" || true)

if [ "$found" -ne 0 ]; then
    echo "clang --analyze: $found finding(s) over ${#sources[@]} files" >&2
    grep -E 'warning:|error:' "$report" | sed "s|$proj/||" >&2
    exit 1
fi
echo "clang --analyze: clean over ${#sources[@]} files"
