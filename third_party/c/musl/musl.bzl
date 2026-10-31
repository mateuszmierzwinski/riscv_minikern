"""musl's source selection, as its Makefile makes it.

A BUILD file cannot define functions, so the rules that pick musl's sources
and lay out its installed headers live here.
"""

def _stem(path):
    return path.rsplit(".", 1)[0]

def _source_dirs():
    dirs = [d for d in native.glob(["src/*"], exclude_directories = 0) if "." not in d.rsplit("/", 1)[1]]
    return dirs + ["src/malloc/mallocng"]

def musl_sources(arch):
    """Every libc source for `arch`: `src/<dir>/*.c`, with a file of the same
    stem under `src/<dir>/<arch>/` (`.c`, `.s` or `.S`) replacing it."""
    dirs = _source_dirs()
    base = native.glob([d + "/*.c" for d in dirs], allow_empty = True)
    arch_srcs = native.glob([d + "/" + arch + "/*." + ext for d in dirs for ext in ["c", "s", "S"]], allow_empty = True)
    replaced = {_stem(s.replace("/" + arch + "/", "/")): True for s in arch_srcs}
    return [s for s in base if _stem(s) not in replaced] + arch_srcs

def musl_optimized(path):
    """Whether musl builds `path` at -O3: `OPTIMIZE_GLOBS`, which is
    `internal/*.c malloc/*.c string/*.c` — each directory's own files only,
    so mallocng is not among them."""
    for d in ["src/internal/", "src/malloc/", "src/string/"]:
        if path.startswith(d) and path.count("/") == 2:
            return True
    return False

def musl_public_headers(arch):
    """The installed header tree, `make install`'s layout: path under
    `include/` to the vendored file. `bits/` comes from the arch tree, or
    from the generic one where the arch has none."""
    public = {h[len("include/"):]: h for h in native.glob(["include/**/*.h"])}
    public.update({"bits/" + h.rsplit("/", 1)[1]: h for h in native.glob(["arch/generic/bits/*.h"])})
    public.update({"bits/" + h.rsplit("/", 1)[1]: h for h in native.glob(["arch/" + arch + "/bits/*.h"])})
    return public
