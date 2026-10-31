# OpenH264 2.6.0 provenance

- Upstream source release: OpenH264 v2.6.0, released 2025-02-12.
- Selection rationale: v2.6.0 is the reviewed release already pinned for FFmpeg's wrapper and is
  supported by FFmpeg 8.1's `libopenh264` encoder. v2.7.0 (2026-10-22) is newer than this intake's
  qualification window and is left for a separate update review.
- Source archive URL: <https://github.com/cisco/openh264/archive/refs/tags/v2.6.0.tar.gz>
- Source archive retrieval date: 2026-09-18 (UTC date)
- Source archive size: 60,302,243 bytes
- Source archive SHA-256: `sha256:558544ad358283a7ab2930d69a9ceddf913f4a51ee9bf1bfb9e377322af81a69`
- Upstream signature: none published for the GitHub tag archive; the lock pins the archive digest.

The same locked archive is the corresponding-source archive for the static library linked into
`libavcodec`. Bloom does not modify the source; the superbuild verifies the SHA-256 before
extraction and builds from the populated archive cache with downloads disabled.
