#!/usr/bin/env bash
# clang-tidy over this kernel's own sources, as a Bazel test.
#
# SonarQube is skipped for this project and not for the reason the ruleset
# gives: the server has no C analyser at all (27 languages, none of them C —
# that one is a commercial-edition feature). For a freestanding kernel the
# tools below are the better fit anyway, because they can be told the target
# triple and the absence of a libc, where Sonar's rules are tuned for
# application code and argue with memory-mapped registers, inline assembly and
# bare-metal patterns.
#
# **The check set is curated, and the gate is zero.** A gate that tolerates a
# standing count of findings teaches everyone to ignore it, so the checks here
# are the ones whose findings are defects in *this* kind of code, and anything
# they report fails the build. What was left out, and why, is in the project's
# CLAUDE.md — the short version is that four `bugprone-*` checks accounted for
# 106 of the first run's 124 findings and every one of them was a false
# positive for this codebase.
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

command -v clang-tidy >/dev/null 2>&1 || {
    echo "clang-tidy is not installed: dnf install clang-tools-extra (needs root)" >&2
    exit 1
}
command -v clang >/dev/null 2>&1 || { echo "clang is not installed" >&2; exit 1; }

# Analysed as what it is: freestanding RV64 with no libc. Analysing these
# sources as host code produces findings about a program that was never built.
resource="$(clang -print-resource-dir)"
cflags=(-Isrc --target=riscv64-unknown-elf -march=rv64gc -mabi=lp64d
        -ffreestanding -nostdinc -isystem "$resource/include"
        -Wall -Wextra)

# Everything off, then the classes that find real defects here.
#
# `clang-analyzer-*` is the path-sensitive engine and is the reason this gate
# exists: on its first run it found a diagnostic counter being fed from an
# uninitialised value, on the one code path the diagnostic was there to
# explain. `optin.performance.Padding` is excluded as a layout preference
# rather than a defect.
checks='-*'
checks+=',clang-analyzer-*,-clang-analyzer-optin.performance.Padding'
checks+=',bugprone-*'
checks+=',-bugprone-easily-swappable-parameters'          # API style, 64 hits, no defects
checks+=',-bugprone-implicit-widening-of-multiplication-result'  # constants and bounded indices
checks+=',-bugprone-assignment-in-if-condition'           # a house idiom, not a defect
checks+=',-bugprone-branch-clone'                         # two causes, one message, on purpose
checks+=',cert-*'
# A linker script's symbols are `__`-prefixed by universal convention, and the
# three checks that object are objecting to the convention rather than to a
# defect. Renaming them would touch the linker script and five files to please
# a checker.
checks+=',-cert-dcl37-c,-cert-dcl51-cpp,-bugprone-reserved-identifier'
# `security.insecureAPI` wants the C11 `_s` functions. A freestanding kernel
# has no libc — the `memcpy` it flags is *this kernel's own* — so the advice is
# to replace a function with one that does not exist.
checks+=',-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling'
checks+=',-clang-analyzer-security.insecureAPI.strcpy'
# Left ON deliberately, having each found something real on the first run:
#   clang-analyzer-core.*              a counter fed from an uninitialised value
#   clang-analyzer-security.ArrayBound an unbounded write behind an implicit invariant
#   clang-analyzer-security.PointerSub subtraction of two linker symbols

mapfile -t sources < <(ls src/core/*.c src/kernel/*.c src/drivers/*.c \
                          src/arch/riscv/*.c src/board/jh7110/*.c src/lib/*.c 2>/dev/null)
[ ${#sources[@]} -gt 0 ] || { echo "no sources found under $proj" >&2; exit 1; }

report="${TEST_TMPDIR:-/tmp}/clang-tidy.txt"
clang-tidy --quiet -checks="$checks" "${sources[@]}" -- "${cflags[@]}" > "$report" 2>&1
found=$(grep -cE 'warning:|error:' "$report" || true)

if [ "$found" -ne 0 ]; then
    echo "clang-tidy: $found finding(s) over ${#sources[@]} files" >&2
    grep -E 'warning:|error:' "$report" | sed "s|$proj/||" >&2
    echo >&2
    echo "The gate is zero. Fix it, or exclude the check in this script with the" >&2
    echo "reason written down — never raise a tolerated count." >&2
    exit 1
fi
echo "clang-tidy: clean over ${#sources[@]} files"
