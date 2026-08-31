# OpenEXR 3.4.15 License And Feature Review

Reviewed: 2026-08-31

## License

- SPDX expression: `BSD-3-Clause`. Permissive; compatible with Bloom's Apache-2.0 distribution
  and static linking under ADR 0014.
- Attribution: satisfied by shipping the bound `LICENSE` bytes with distributed builds.
  `copyrightFiles`/`noticeFiles` are empty by review: the copyright line lives inside the license
  text. `sourceObligation: none`; `modified: false` with an empty patch set (this task applies no
  patch to the OpenEXR source; see `provenance.md` for the open symbolic-link question that a
  future patch may need to address).

## zlib-vs-libdeflate Determination

See `dependencies/licenses/libdeflate/review.md` for the full grep-verified determination:
OpenEXR 3.4.15 has zero references to zlib anywhere in its CMake or C/C++ sources. ZIP/ZIPS
compression requires libdeflate; `bloom_dependency_libdeflate` supplies it as a third recipe, and
`OPENEXR_FORCE_INTERNAL_DEFLATE:BOOL=OFF` plus a `CMAKE_PREFIX_PATH` pointed at the shared prefix
make OpenEXR consume that recipe's build instead of its own vendored copy or a host package —
confirmed in the build log (`-- Using externally provided libdeflate: 1.26`).

## Compression Feature Minimization

`docs/architecture/frame-output.md`'s Flat OpenEXR Preset Version 1 writes and reopen-verifies
exactly one compression attribute value: `compression = ZIP_COMPRESSION`. Every other compression
identifier OpenEXR defines — `NONE`, `RLE`, `ZIPS`, `PIZ`, `PXR24`, `B44`, `B44A`, `DWAA`, `DWAB`,
and `HTJ2K` (via the OpenJPH codec) — is outside the preset's contract.

OpenEXR 3.4.15's `CMakeLists.txt` exposes **no per-codec build switch**: `grep -rn "^option("`
across the whole source tree finds only `OPENEXR_INSTALL_DOCS` and `BUILD_WEBSITE` at the top
level (plus fuzzer/test-only options this recipe never reaches). RLE/PIZ/PXR24/B44/B44A/DWAA/DWAB
are unconditionally compiled into `libOpenEXRCore-3_4.a`'s codec table; there is no
`OPENEXR_ENABLE_<CODEC>` to disable them. This recipe therefore cannot reduce the compiled codec
set below what upstream ships in one static library. The unused codecs are dead code from Bloom's
write path — Bloom's own code only ever requests `ZIP_COMPRESSION` — but remain reachable parsing
surface for any future consumer that decodes an untrusted EXR file. `security.md` records that as
a future consumer-side concern, not resolved by this intake.

HTJ2K specifically pulls in the vendored OpenJPH codec (`dependencies/licenses/openjph/`), which
*is* compiled (no build switch exists to omit it either); see that component's `review.md`.

## Build-Surface Feature Minimization

The recipe (`dependencies/superbuild/projects/openexr.cmake`) enables only the core static
libraries needed for Bloom's EXR read/write path:

| CMake option | Value | Contract justification |
| --- | --- | --- |
| `OPENEXR_BUILD_TOOLS` | `OFF` | `exrheader`/`exrenvmap`/etc. CLI utilities are not part of Bloom's linked surface. |
| `OPENEXR_INSTALL_TOOLS` | `OFF` | No-op given `BUILD_TOOLS=OFF`; set explicitly for clarity. |
| `OPENEXR_INSTALL_DEVELOPER_TOOLS` | `OFF` | Matches upstream default; developer-only tooling. |
| `OPENEXR_BUILD_EXAMPLES` | `OFF` | Example programs are not shipped. |
| `OPENEXR_BUILD_PYTHON` | `OFF` | Matches upstream default; Python bindings are unrelated to Bloom's C++ consumption. |
| `OPENEXR_BUILD_OSS_FUZZ` | `OFF` | Matches upstream default; fuzz harnesses are not shipped (also the tree containing the one archive symlink noted in `provenance.md`). |
| `BUILD_TESTING` | `OFF` | Keeps `src/test` out of the build ("tests off"). |
| `OPENEXR_INSTALL_DOCS` | `OFF` | Keeps generated manpages out of the prefix ("docs off"). |
| `BUILD_WEBSITE` | `OFF` | Keeps the readthedocs documentation source out of the build. |
| `OPENEXR_USE_TBB` | `OFF` | Matches upstream default; Bloom does not provide a locked TBB, and OpenEXR's own thread pool (`OPENEXR_ENABLE_THREADING`, left at its default `ON`) needs no external threading library. |
| `OPENEXR_FORCE_INTERNAL_DEFLATE` | `OFF` | See "zlib-vs-libdeflate Determination" above. |
| `OPENEXR_FORCE_INTERNAL_OPENJPH` | `ON` | Forces the deterministic, hermetic vendored-HTJ2K path instead of an ambient host `openjph` probe; see `dependencies/licenses/openjph/provenance.md`. |
| `OPENEXR_FORCE_INTERNAL_IMATH` | `OFF` | Consumes `bloom_dependency_imath` via `find_package(Imath 3.1 CONFIG)`; without this, an unresolved `find_package` would `FetchContent`-clone Imath from git — a network fetch the contract forbids. Confirmed resolved against the prefix in the build log (`-- Using Imath 3.2.3 from .../lib/cmake/Imath`), not fetched. |

The resulting installed static libraries are `libOpenEXRCore-3_4.a`, `libOpenEXR-3_4.a`,
`libOpenEXRUtil-3_4.a`, `libIex-3_4.a`, and `libIlmThread-3_4.a` — OpenEXR's own core/utility
library split, all required transitively; no `bin/` tools or `share/doc`/`share/man` entries are
installed by this component.
