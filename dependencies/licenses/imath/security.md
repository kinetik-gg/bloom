# Imath 3.2.3 Security Review

Reviewed: 2026-08-31

- Exposure: Imath is a header-heavy numeric library (vector/matrix/quaternion arithmetic,
  half-float conversion). It performs no file, network, or archive parsing of its own; Bloom
  reaches it only indirectly through OpenEXR's use of `Imath::Box2i`/`V2f` types for window and
  pixel-aspect handling in the flat scanline EXR path (`docs/architecture/frame-output.md`).
  There is no untrusted-input parsing surface owned by Imath itself.
- No published GitHub security advisory exists for `AcademySoftwareFoundation/Imath` at review
  time (checked the repository's Security Advisories API endpoint: zero results). No NVD CVE
  entry matches `Imath` either.
- Disposition: accepted for intake at 3.2.3. Re-review on any Imath advisory or version change.
