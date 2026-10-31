# musl — provenance

| | |
|---|---|
| Upstream | https://musl.libc.org/ |
| Version | **1.2.5** (release tarball `musl-1.2.5.tar.gz`) |
| SHA-256 of the tarball | `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4` |
| Vendored | 2026-10-31 |
| License | MIT (`COPYRIGHT`) |
| Used by | `src/c/experiments/riscv_minikern` user programs (`musl_binary`), the C library of minikern's user space |

## Kept and dropped

- **Kept:** everything except the architectures below — `src/`, `include/`, `crt/`,
  `ldso/`, `compat/`, `tools/`, `arch/riscv64`, `arch/generic`, and the top-level
  files (`COPYRIGHT`, `README`, `INSTALL`, `WHATSNEW`, `VERSION`, `Makefile`,
  `configure`, `dynamic.list`).
- **Dropped:** every `arch/<a>/` and `src/<dir>/<a>/` for an architecture other
  than riscv64 — minikern runs on one machine. Nothing else was changed; no file
  is patched.

## How it is built

`BUILD` + `musl.bzl` reproduce musl's own Makefile for `ARCH=riscv64`: the same
source selection (an arch file replaces the generic file of the same stem), the
same `sed`-generated `bits/alltypes.h`, `bits/syscall.h` and `version.h`, the same
include order and C99/freestanding flags, `-O3` for `OPTIMIZE_GLOBS`. Only a
static `libc` and `crt1` are built — no `libc.so`, no dynamic linker.

`long double` on riscv64 is software binary128, so a link also needs the
compiler-rt routines in `third_party/c/compiler_rt_builtins`.

## Upgrading

Replace the tree with the new release (pruned the same way), update the version
and hash above, rebuild `//:musl_hello.elf` and
run the board case `§10: a program built against musl runs`.
