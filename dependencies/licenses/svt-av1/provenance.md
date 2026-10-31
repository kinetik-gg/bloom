# SVT-AV1 4.2.0 provenance

- Upstream release: SVT-AV1 v4.2.0 (Alliance for Open Media), released 2026-07-14; tag commit
  `9292ec8e32bce26f781f277ec8739b53426c4300`.
- Selection rationale: the latest upstream release at the 2026-10-31 intake; FFmpeg 8.1's
  `libsvtav1` wrapper requires SVT-AV1 0.9.0 or newer and has a dedicated 4.x code path.
- Archive URL: <https://gitlab.com/AOMediaCodec/SVT-AV1/-/archive/v4.2.0/SVT-AV1-v4.2.0.tar.gz>
  (GitLab's tag archive; the project publishes no separate distribution tarball)
- Archive retrieval date: 2026-10-31 (UTC date)
- Archive size: 11,070,537 bytes
- Archive SHA-256: `sha256:c7b13c4a84bd3751aa35fcc72be13e6875467e7c2216879251a486e5b1e4e740`;
  two independent downloads on the intake date produced identical bytes.
- Upstream signature: none published; the lock pins the archive digest.

The same locked archive is the corresponding-source archive for the static library linked into
`libavcodec`. Bloom does not modify the source; the superbuild verifies the SHA-256 before
extraction and builds from the populated archive cache with downloads disabled.
