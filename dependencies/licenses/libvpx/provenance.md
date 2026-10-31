# libvpx 1.17.0 provenance

- Upstream release: libvpx v1.17.0 (the WebM Project), tagged 2026-08-21; annotated tag object
  `82615ff0733cd4063838c352bc2e2ab225f37ade` on commit
  `6df3ec34557879fff673706f4a1d9fbd0f3a6f0e`.
- Selection rationale: the latest upstream release at the 2026-10-31 intake; FFmpeg 8.1's `libvpx`
  wrapper requires libvpx 1.4.0 or newer.
- Archive URL: <https://github.com/webmproject/libvpx/archive/refs/tags/v1.17.0.tar.gz>
  (GitHub's tag archive; the project publishes no separate distribution tarball)
- Archive retrieval date: 2026-10-31 (UTC date)
- Archive size: 5,670,546 bytes
- Archive SHA-256: `sha256:1020f184046187baa2985dbde38e0691f49c44088bca7a1842b0236c6081dc0a`;
  two independent downloads on the intake date produced identical bytes.
- Upstream signature: the tag is not signed; the lock pins the archive digest.

The same locked archive is the corresponding-source archive for the static library linked into
`libavcodec`. Bloom does not modify the source; the superbuild verifies the SHA-256 before
extraction and builds from the populated archive cache with downloads disabled.
