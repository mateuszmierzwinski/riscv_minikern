#!/usr/bin/env bash
#
# gen_fuzz_c.sh — run one of this kernel's libFuzzer harnesses and harvest
# what it finds. The C counterpart of the monorepo's gen_fuzz.sh.
#
#   SEED corpus   //testdata/fuzz/src/c/experiments/riscv_minikern/<Name>/
#                 committed; replayed by `<harness>_replay` on every
#                 `bazel test`. This script adds crashers to it.
#   CACHE corpus  ~/.cache/monorepo-fuzz/riscv_minikern/<Name>/
#                 the fuzzer's working set, never committed.
#
#   ./gen_fuzz_c.sh fuzz_xfs [seconds]     # default 120
#
# The whole tree is instrumented from the command line — coverage for the
# fuzzer, AddressSanitizer and UBSan for the verdict — because `copts` on
# the binary would reach only the harness, and an uninstrumented parser
# gives the fuzzer nothing to steer by. Fuzzing is run by hand and its
# output committed; the build never fuzzes.
set -euo pipefail

harness="${1:-}"
seconds="${2:-120}"
declare -A corpus_of=([fuzz_edid]=FuzzEdid [fuzz_elf]=FuzzElf [fuzz_fdt]=FuzzFdt [fuzz_xfs]=FuzzXfs)
if [[ -z "$harness" || -z "${corpus_of[$harness]:-}" ]]; then
    echo "usage: $0 {${!corpus_of[*]}} [seconds]" >&2
    exit 1
fi
name="${corpus_of[$harness]}"
project="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# The workspace: the nearest directory above holding MODULE.bazel — the
# monorepo's root here, the project's own root in the published tree.
root="$project"
while [ ! -f "$root/MODULE.bazel" ] && [ "$root" != / ]; do
    root="$(dirname "$root")"
done
seed="$root/testdata/fuzz/src/c/experiments/riscv_minikern/$name"
cache="$HOME/.cache/monorepo-fuzz/riscv_minikern/$name"
crashes="$(mktemp -d)"
trap 'rm -rf "$crashes"' EXIT
mkdir -p "$seed" "$cache"

# libFuzzer is clang's: the host toolchain Bazel detects is gcc, so the
# harness builds with `CC=clang` in an output base of its own, which keeps
# the switch from reconfiguring — and emptying — the everyday build's cache.
base=(--output_base="$HOME/.cache/monorepo-fuzz/bazel-base")
flags=(--repo_env=CC=clang --copt=-fsanitize=fuzzer-no-link,address,undefined --copt=-fno-sanitize-recover=all
       --linkopt=-fsanitize=address,undefined)
(cd "$root" && bazel "${base[@]}" build "${flags[@]}" "//:$harness")
binary="$(cd "$root" && bazel "${base[@]}" cquery "${flags[@]}" --output=files "//:$harness" 2>/dev/null)"

status=0
exec_root="$(cd "$root" && bazel "${base[@]}" info "${flags[@]}" execution_root 2>/dev/null)"
(cd "$crashes" && "$exec_root/$binary" -max_total_time="$seconds" -max_len=262144 -rss_limit_mb=4096 \
    -artifact_prefix="$crashes/" "$cache" "$seed") || status=$?

found=0
for f in "$crashes"/crash-* "$crashes"/leak-* "$crashes"/timeout-* "$crashes"/oom-*; do
    [[ -e "$f" ]] || continue
    h="$(sha256sum "$f" | cut -c1-16)"
    cp "$f" "$seed/$h"
    echo "gen_fuzz_c: $(basename "$f") kept as $seed/$h"
    found=$((found + 1))
done
echo "gen_fuzz_c: $harness ran ${seconds}s, $found finding(s), fuzzer exit $status"
[[ $found -eq 0 ]]
