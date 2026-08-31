# OpenJPH 0.31.0 Security Review (Vendored Inside OpenEXR)

Reviewed: 2026-08-31

- Exposure: OpenJPH implements the HTJ2K encode/decode path compiled into `libOpenEXRCore-3_4.a`.
  Bloom's write path never selects HTJ2K compression (see `review.md`), so this code is unused at
  runtime for Bloom's own output; it becomes reachable only if a future consumer decodes an
  untrusted EXR file containing an HTJ2K part, which `dependencies/licenses/openexr/security.md`
  records as that future consumer's concern, not qualified here.
- No published GitHub security advisory exists for `aous72/OpenJPH` at review time (checked the
  repository's Security Advisories API endpoint: zero results).
- OpenEXR's own GitHub advisories include HTJ2K-integration findings against OpenEXR's
  `internal_ht.cpp` glue code (e.g. GHSA-ghfj-fx47-wg97, patched in 3.4.7; GHSA-hphq-wq62-4mj3,
  "OpenEXR HTJ2K planar decode wraps the row endpoint for negative origins", patched in 3.4.14).
  Both predate the locked 3.4.15 and are covered by
  `dependencies/licenses/openexr/security.md`'s version-floor check.
- Disposition: accepted for intake as a vendored component of OpenEXR 3.4.15. Re-review tracks
  OpenEXR's own version changes since OpenJPH is not independently versioned in the lock.
