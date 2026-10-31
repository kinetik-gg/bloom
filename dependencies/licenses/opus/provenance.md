# Opus 1.5.2 provenance

- Upstream release: Opus 1.5.2 (Xiph.Org), released 2024-04-12.
- Selection rationale: the latest upstream release at the 2026-10-31 intake; FFmpeg 8.1's
  `libopus` wrapper needs the multistream API every 1.x release provides.
- Archive URL: <https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz> (Xiph's distribution
  tarball)
- Archive retrieval date: 2026-10-31 (UTC date)
- Archive size: 7,839,412 bytes
- Archive SHA-256: `sha256:65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1`,
  equal to the entry in Xiph's published
  <https://downloads.xiph.org/releases/opus/SHA256SUMS.txt>
- Upstream signature: none published for this release; the lock pins the archive digest.

The same locked archive is the corresponding-source archive for the static library linked into
`libavcodec`. Bloom does not modify the source; the superbuild verifies the SHA-256 before
extraction and builds from the populated archive cache with downloads disabled.
