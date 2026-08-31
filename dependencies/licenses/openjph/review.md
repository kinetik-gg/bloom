# OpenJPH 0.31.0 License And Feature Review (Vendored Inside OpenEXR)

Reviewed: 2026-08-31

## License

- SPDX expression: `BSD-2-Clause`. Permissive; compatible with Bloom's Apache-2.0 distribution
  and static linking under ADR 0014.
- Attribution: satisfied by shipping the bound `LICENSE` bytes with distributed builds.
  `copyrightFiles`/`noticeFiles` are empty by review: the copyright lines live inside the license
  text. `sourceObligation: none`; `modified: false` (Bloom applies no patch to OpenEXR's vendored
  copy).

## Feature Minimization

There is no CMake switch in OpenEXR 3.4.15 to omit HTJ2K/OpenJPH support from `OpenEXRCore` — it
is unconditionally compiled in, and Bloom's frame-output contract does not need it: the flat
OpenEXR preset (`docs/architecture/frame-output.md`, "Flat OpenEXR Preset Version 1") writes and
verifies exactly `compression = ZIP_COMPRESSION`; HTJ2K is one of the ten compression identifiers
the preset never emits. `OPENEXR_FORCE_INTERNAL_OPENJPH:BOOL=ON` is set for determinism (no
ambient host `openjph` package probe), and OpenEXR's own build forces the narrowest form of the
vendored copy available: `OJPH_INSTALL=OFF` (never installed as its own package),
`OJPH_BUILD_EXECUTABLES=OFF` (no CLI tools), `OJPH_LIB_TYPE=OBJECT` (object library merged
directly into `OpenEXRCore`, not a separate shared/static artifact). This is the minimum footprint
upstream OpenEXR 3.4.15 permits; there is no further reduction available without patching OpenEXR
itself, which is out of this task's scope.

This compiled-in HTJ2K surface is unused dead code from Bloom's write path but remains reachable
parsing surface if a future consumer decodes an untrusted EXR file containing an HTJ2K part —
recorded in `dependencies/licenses/openexr/security.md` as that future consumer's concern.
