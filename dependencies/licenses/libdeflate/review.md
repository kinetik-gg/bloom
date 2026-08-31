# libdeflate 1.26 License And Feature Review

Reviewed: 2026-08-31

## License

- SPDX expression: `MIT`. Permissive; compatible with Bloom's Apache-2.0 distribution and static
  linking under ADR 0014.
- Attribution: satisfied by shipping the bound `LICENSE` bytes with distributed builds.
  `copyrightFiles`/`noticeFiles` are empty by review: both copyright lines live inside the
  license text itself. `sourceObligation: none`; `modified: false` with an empty patch set.

## zlib-vs-libdeflate Determination

`docs/architecture/dependency-intake.md`'s frozen decision for the OpenEXR recipe was: consume
the prefix's own zlib for ZIP-family compression if the locked OpenEXR version still supports it;
otherwise libdeflate becomes a third recipe with full intake rigor. Inspection of the downloaded
OpenEXR 3.4.15 archive (`dependencies/licenses/openexr/provenance.md`) settles this:

- `grep -rn "ZLIB\|zlib" cmake/*.cmake CMakeLists.txt` across the entire OpenEXR 3.4.15 source
  tree returns zero matches — OpenEXR removed zlib as a DEFLATE backend entirely in the 3.2 line.
- `cmake/OpenEXRSetup.cmake` instead does `find_package(libdeflate CONFIG QUIET)`, falling back to
  `pkg_check_modules(... libdeflate)`, and only after both fail does it compile a vendored copy of
  libdeflate bundled at `external/deflate` inside the OpenEXR archive
  (`OPENEXR_FORCE_INTERNAL_DEFLATE`).
- `src/lib/OpenEXRCore/compression.c` confirms the call surface: `libdeflate_zlib_compress` /
  `libdeflate_zlib_decompress_ex` — the zlib-*wrapper format* inside libdeflate, not zlib itself
  and not libdeflate's raw-gzip format.

So libdeflate is required, not optional, and per the frozen decision it is a full third recipe
(`dependencies/superbuild/projects/libdeflate.cmake`) rather than a silently vendored or
auto-fetched component. `dependencies/superbuild/projects/openexr.cmake` sets
`OPENEXR_FORCE_INTERNAL_DEFLATE:BOOL=OFF` and adds `CMAKE_PREFIX_PATH` pointing at the shared
prefix so `find_package(libdeflate CONFIG)` resolves this recipe's build, ranked ahead of any
default system location in CMake's config-mode search order. The superbuild configure log
confirms the outcome: `-- Using externally provided libdeflate: 1.26` (not "was not found, using
vendored code"). Because the vendored `external/deflate/*.c` sources in the OpenEXR archive are
only reached from inside `compression.c`'s `#if OPENEXR_USE_INTERNAL_DEFLATE` branch, and that
macro evaluates false when the external package is found, none of that vendored source is
compiled — verified against the actual object-file membership of the installed
`libOpenEXRCore-3_4.a` (no `deflate_compress.c.o`/`deflate_decompress.c.o`/`zlib_compress.c.o`/
`zlib_decompress.c.o` members). No separate vendored-libdeflate lock record is therefore needed.

## Feature Minimization

The recipe (`dependencies/superbuild/projects/libdeflate.cmake`) builds only the static library
and only the exact format surface OpenEXR calls:

| CMake option | Value | Contract justification |
| --- | --- | --- |
| `LIBDEFLATE_BUILD_STATIC_LIB` | `ON` | The one artifact Bloom needs in the static prefix. |
| `LIBDEFLATE_BUILD_SHARED_LIB` | `OFF` | Matches the static-only prefix convention; avoids a second, unused build product. |
| `LIBDEFLATE_COMPRESSION_SUPPORT` | `ON` | OpenEXR both compresses (write) and decompresses (reopen verification per frame-output.md) — both directions are load-bearing. |
| `LIBDEFLATE_DECOMPRESSION_SUPPORT` | `ON` | See above. |
| `LIBDEFLATE_ZLIB_SUPPORT` | `ON` | Required: `libdeflate_zlib_compress`/`libdeflate_zlib_decompress_ex` are the exact functions `compression.c` calls. |
| `LIBDEFLATE_GZIP_SUPPORT` | `OFF` | The gzip container format is a distinct, independently gated source set (`lib/gzip_compress.c`/`lib/gzip_decompress.c`) that OpenEXR never calls; confirmed independent of `ZLIB_SUPPORT` in libdeflate's own `CMakeLists.txt`. |
| `LIBDEFLATE_BUILD_GZIP` | `OFF` | The `libdeflate-gzip` CLI program; Bloom never shells out to it. |
| `LIBDEFLATE_BUILD_TESTS` | `OFF` | Test programs are not part of the shipped surface. |
| `LIBDEFLATE_INSTALL` | `ON` | Required so the CMake package config (`libdeflate-config.cmake`) lands in the shared prefix for OpenEXR's `find_package(libdeflate CONFIG)`. |
