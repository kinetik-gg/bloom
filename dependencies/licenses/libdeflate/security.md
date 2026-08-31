# libdeflate 1.26 Security Review

Reviewed: 2026-08-31

- Exposure: libdeflate is the DEFLATE codec behind every ZIP/ZIPS-compressed OpenEXR scanline
  Bloom writes and — through the reopen-verification step required by
  `docs/architecture/frame-output.md` — reads back immediately after writing. It is not yet
  exposed to arbitrary untrusted files; decode of externally sourced EXR files is a future
  consumer-side concern (see `dependencies/licenses/openexr/security.md`).
- No published GitHub security advisory exists for `ebiggers/libdeflate` at review time (checked
  the repository's Security Advisories API endpoint: zero results). No NVD CVE entry matches
  `libdeflate` either.
- Feature minimization removes the gzip container format, the `libdeflate-gzip` CLI, and the test
  programs from the build (see `review.md`); only the zlib-wrapper compress/decompress entry
  points OpenEXR calls are compiled in.
- Disposition: accepted for intake at 1.26. Re-review on any libdeflate advisory or version
  change.
