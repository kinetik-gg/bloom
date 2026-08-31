# OpenEXR 3.4.15 Security Review

Reviewed: 2026-08-31

## Parsing-Surface Posture

OpenEXR is a complex binary-format parser (headers, chunk tables, scanline/tile/deep encodings,
ten-plus compression codecs, custom attribute types). This review's scope is intake — acquiring,
building, and minimizing this component — not qualifying it to decode untrusted files.

- Bloom's only qualified use in this slice is the flat, single-part, scanline
  `FlatExrRgba32fLinRec709SceneV1` write path (`docs/architecture/frame-output.md`): Bloom
  constructs the header attributes itself (exact allowlist, `ZIP_COMPRESSION` only,
  `INCREASING_Y` line order, no deep/tiled/multipart/HTJ2K), writes pixels Bloom already owns in
  memory, then immediately reopens and verifies the staged file with the same pinned reader before
  it can replace a publication target. This is a controlled write-then-verify round trip over
  Bloom-authored data, not decode of an arbitrary external file.
- **Decode of untrusted, externally sourced EXR files is explicitly out of scope for this intake**
  and is a future consumer-side concern: any future EXR *import* capability must re-run this
  security review against that expanded exposure (untrusted headers, untrusted compressed chunk
  data, all codecs reachable including HTJ2K/PIZ/PXR24/B44/DWA — see `review.md`'s "Compression
  Feature Minimization" for why those codecs remain compiled in even though Bloom's write path
  never selects them) before any untrusted-file decode path is qualified.

## Known Vulnerabilities

- Checked the `AcademySoftwareFoundation/openexr` GitHub Security Advisories API endpoint at
  review time: 73 advisories exist against this repository, spanning severities low through
  critical (heap overflows, uninitialized-memory disclosure, and denial-of-service findings,
  concentrated in deep/tiled decode, the `exr*` CLI tools Bloom does not build, HTJ2K decode, and
  Python bindings Bloom does not build).
- Every one of those 73 advisories' `patched_versions` for the `3.4.x` line is `3.4.14` or
  earlier (the newest-patched entry is GHSA-hphq-wq62-4mj3, "OpenEXR HTJ2K planar decode wraps the
  row endpoint for negative origins", patched in `3.4.14`). The locked version, `3.4.15`,
  post-dates every published fix; no known unpatched CVE or advisory affects it at review time.
- No separate NVD CVE search was needed beyond the GitHub advisory sweep above, since GitHub's
  advisory database mirrors NVD entries for this repository and the version-floor check already
  covers the full advisory history.

## Disposition

Accepted for intake at 3.4.15, restricted to the write-then-verify flat-scanline round trip
described above. Re-review on any OpenEXR advisory or version change, and mandatorily before any
future untrusted-file decode capability is qualified.
