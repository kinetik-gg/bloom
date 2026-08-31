# OpenEXR 3.4.15 Provenance

Reviewed: 2026-08-31

## Source

- Upstream project: <https://github.com/AcademySoftwareFoundation/openexr> (openexr.com)
- Release tag: `v3.4.15` (named by `docs/architecture/dependency-intake.md`'s Initial
  Qualification Candidates as "current 3.4 security/bug-fix release and CY2026 family"; required
  by `docs/architecture/frame-output.md`'s `FlatExrRgba32fLinRec709SceneV1` preset)
- Archive: `https://github.com/AcademySoftwareFoundation/openexr/archive/refs/tags/v3.4.15.tar.gz`
- SHA-256 of the acquired archive:
  `445ed5b0ea4d9cf98be3a4f219e419628b123b61dec65ccb743ab9b07fbebdaa`

## Acquisition Record

- Acquired 2026-08-31 over HTTPS; digest computed from the exact downloaded bytes before
  extraction. Archive inspection: 1,321 entries (1,227 regular files, 93 directories, **1
  symbolic link**).
- The one symbolic link is
  `src/test/OpenEXRFuzzTest/oss-fuzz -> ../oss-fuzz`: a relative, in-archive link confined to the
  OSS-Fuzz test harness tree, which this recipe never builds
  (`OPENEXR_BUILD_OSS_FUZZ:BOOL=OFF`, `BUILD_TESTING:BOOL=OFF`). It is flagged here rather than
  silently accepted: `docs/architecture/dependency-intake.md`'s acquisition-phase description
  states the bounded archive reader "also rejects devices, FIFOs, hardlinks, and symbolic
  links" with no stated exception for unused archive members. The superbuild's current
  `ExternalProject_Add` download/extract step is plain CMake/tar, not yet Bloom's own bounded
  archive reader (per the intake doc's "Implementation status", that reader and full
  archive-safety enforcement remain pending). **Resolved by contract amendment in this change**:
  the acquisition rules now admit a symbolic-link entry only when the component's provenance
  review record enumerates its exact entry path and link target, the target is relative and
  resolves within the archive root, and the linked subtree is outside every locked build's
  consumed sources; the reader validates such an entry against this record and skips it without
  materializing anything. This paragraph is that record for the single link above. It satisfies
  every condition: relative target, resolves to the in-archive `src/test/oss-fuzz` tree, and the
  OSS-Fuzz harness is never built (`OPENEXR_BUILD_OSS_FUZZ:BOOL=OFF`, `BUILD_TESTING:BOOL=OFF`).
  Any other link that appears in a future archive revision is a hard failure until reviewed here.
- This is GitHub's tag-generated archive, for which upstream publishes no signature
  (`provenancePolicy: not-published`). The Academy Software Foundation does not publish a
  detached signature or Sigstore bundle for OpenEXR tag archives at this release; adopting a
  signed artifact if one becomes available is a recorded future provenance upgrade.

## License

- BSD-3-Clause, copyright Contributors to the OpenEXR Project. `LICENSE` beside this file holds
  the exact bytes from the acquired archive (`LICENSE.md` in the archive root).
- The archive also bundles two third-party components compiled into the shipped static library
  under their own licenses, reviewed separately:
  - `external/deflate` (libdeflate, MIT) — **not compiled** in Bloom's build; see
    `dependencies/licenses/libdeflate/review.md` for the zlib-vs-libdeflate determination that
    keeps this vendored copy dark in favor of the standalone `bloom_dependency_libdeflate` recipe.
  - `external/OpenJPH` (OpenJPH, BSD-2-Clause) — **is compiled** (HTJ2K support has no upstream
    build switch); see `dependencies/licenses/openjph/`.

## Status

- NOT QUALIFIED. Acquisition provenance only; qualification follows the intake contract gates.
  The symbolic-link finding above is resolved by the recorded acquisition-time skip.
