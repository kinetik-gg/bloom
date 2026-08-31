# OpenColorIO 2.5.2 Provenance

Reviewed: 2026-08-31

## Source

- Upstream project: <https://github.com/AcademySoftwareFoundation/OpenColorIO> (opencolorio.org)
- Release tag: `v2.5.2` (named by `docs/architecture/dependency-intake.md`'s Initial Qualification
  Candidates as "Current 2.5 security/bug-fix release and CY2026 family"; required by
  `docs/architecture/color-management.md`'s OCIO CPU display path)
- Archive: `https://github.com/AcademySoftwareFoundation/OpenColorIO/archive/refs/tags/v2.5.2.tar.gz`
- SHA-256 of the acquired archive:
  `722601e01b78b7a12da4829cb450674935f404b0e508f3f20046fa77570e3272`

## Acquisition Record

- Acquired 2026-08-31 over HTTPS; digest computed from the exact downloaded bytes before
  extraction. Archive inspection: 1,980 entries (245 directories, 1,647 regular files, 88
  executable-bit regular files, **zero symbolic links**).
- This is GitHub's tag-generated archive, for which upstream publishes no signature
  (`provenancePolicy: not-published`). The Academy Software Foundation does not publish a
  detached signature or Sigstore bundle for OpenColorIO tag archives at this release, matching
  the same not-published disposition already recorded for Imath, libdeflate, and OpenEXR.

## License

- BSD-3-Clause, copyright Contributors to the OpenColorIO Project. `LICENSE` beside this file
  holds the exact bytes from the acquired archive.
- The archive also carries a `vendor/` and `ext/` tree of optional/vendored source for build
  configurations this recipe does not enable (e.g. `ext/sse2neon` for non-x86 SIMD, apps-only
  helper code). None of it is compiled by this recipe's static, library-only build — see
  `review.md`'s Feature Minimization tables for the exact enabled surface and the mandatory
  external dependencies this recipe consumes from the shared prefix instead of any vendored copy.

## Status

- NOT QUALIFIED. Acquisition provenance only; qualification follows the intake contract gates.
