#!/usr/bin/env bash
# Coverage of the host tests, through Bazel (Rule 23.1): `bazel coverage`
# writes LCOV, kept here as coverage.lcov, and a line-coverage summary per
# source directory is printed. Sonar has no C analyser on this server
# (CLAUDE.md §2), so the report is for reading, and the summary is the
# number to watch. The board suite is not covered: it runs on the board.
#
#   ./gen_coverage.sh                 # the default output base
#   BAZEL_OUTPUT_BASE=/tmp/cov ./gen_coverage.sh   # beside another build
set -euo pipefail

project="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# The workspace: the nearest directory above holding MODULE.bazel — the
# monorepo's root here, the project's own root in the published tree.
root="$project"
while [ ! -f "$root/MODULE.bazel" ] && [ "$root" != / ]; do
    root="$(dirname "$root")"
done
out="$project/coverage.lcov"
startup=()
[ -n "${BAZEL_OUTPUT_BASE:-}" ] && startup=(--output_base="$BAZEL_OUTPUT_BASE")

# The host tests only: everything else here is cross-compiled for the board
# and cannot link a coverage runtime, and `manual` tests need the board.
mapfile -t tests < <(cd "$root" && bazel "${startup[@]}" query \
    'tests(//...) except attr(tags, manual, //...)' 2>/dev/null)
(cd "$root" && bazel "${startup[@]}" coverage --combined_report=lcov "${tests[@]}")
report="$(cd "$root" && bazel "${startup[@]}" info output_path)/_coverage/_coverage_report.dat"
cp "$report" "$out"
echo "wrote $out"
# Lines found (LF) and hit (LH) per file, summed per directory under src/.
awk -F: '
    /^SF:/ { file = $2; sub(/.*riscv_minikern\//, "", file); n = split(file, p, "/");
             dir = n > 2 ? p[1] "/" p[2] "/" p[3] : file; next }
    /^LF:/ { found[dir] += $2; tf += $2; next }
    /^LH:/ { hit[dir] += $2; th += $2; next }
    END {
        for (d in found) if (d ~ /^src\//)
            printf "%-28s %6d / %6d lines  %5.1f%%\n", d, hit[d], found[d],
                   found[d] ? 100 * hit[d] / found[d] : 0
        printf "%-28s %6d / %6d lines  %5.1f%%\n", "total", th, tf, tf ? 100 * th / tf : 0
    }' "$out" | sort
