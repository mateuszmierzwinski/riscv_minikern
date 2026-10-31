# compiler-rt builtins (subset) — provenance

| | |
|---|---|
| Upstream | https://github.com/llvm/llvm-project, `compiler-rt/lib/builtins` |
| Version | **llvmorg-21.1.8** (`compiler-rt-21.1.8.src.tar.xz` from the release) |
| SHA-256 of the tarball | `dd54ae21aee1780fac59445b51ebff601ad016b31ac3a7de3b21126fd3ccb229` |
| Vendored | 2026-10-31 |
| License | Apache-2.0 WITH LLVM-exception (`LICENSE.TXT`) |
| Used by | `//third_party/c/musl:c` — every musl program minikern runs |

## Why only these files

On riscv64 `long double` is IEEE binary128, done in software, and Fedora ships no
riscv64 compiler runtime (`clang -print-libgcc-file-name` names a file that does
not exist). A static musl program then fails to link on 16 symbols: `__addtf3`,
`__subtf3`, `__multf3`, `__divtf3`, `__eqtf2`/`__netf2`/`__letf2`/`__getf2`,
`__extenddftf2`, `__extendsftf2`, `__trunctfdf2`, `__trunctfsf2`, `__fixtfsi`,
`__fixunstfsi`, `__floatsitf`, `__floatunsitf`.

**Kept:** the `.c` files defining those (plus the `di` conversions beside them),
`riscv/fp_mode.c` (the rounding mode they consult), and exactly the headers and
`.inc` files those include — the list was produced by `clang -MM` over them, not
chosen by hand. **Dropped:** everything else in compiler-rt. Nothing is patched.

## Upgrading

Take the same files from the new release (re-run `clang -MM` to find the headers),
update the version and hash above, and rebuild a musl program.
