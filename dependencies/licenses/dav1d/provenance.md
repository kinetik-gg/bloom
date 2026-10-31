# dav1d 1.5.4 provenance

- Upstream release: dav1d 1.5.4, released 2026-07-14.
- Selection rationale: the latest upstream release at the 2026-10-31 intake; FFmpeg 8.1's
  `libdav1d` wrapper requires dav1d 1.0.0 or newer.
- Archive URL: <https://downloads.videolan.org/pub/videolan/dav1d/1.5.4/dav1d-1.5.4.tar.xz>
- Archive retrieval date: 2026-10-31 (UTC date)
- Archive size: 1,038,852 bytes
- Archive SHA-256: `sha256:686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5`,
  equal to VideoLAN's published `dav1d-1.5.4.tar.xz.sha256`
- Detached signature URL: <https://downloads.videolan.org/pub/videolan/dav1d/1.5.4/dav1d-1.5.4.tar.xz.asc>
- Detached signature size: 195 bytes
- Detached signature SHA-256: `sha256:b7e9664c8d847b2e88b059843b9977a74df2a7c7d911b094821dc236671bf9fc`
- Signature evidence: `dav1d-1.5.4.tar.xz.asc` is checked in beside this record, copied
  byte-for-byte from the downloaded signature.
- Verification key: VideoLAN Release Signing Key, fingerprint
  `65F7C6B4206BD057A7EB73787180713BE58D1ADC`, retrieved from a public keyserver.
- Verification result: `gpg --verify` reported a good signature made 2026-07-14 by the VideoLAN
  Release Signing Key. The key's validity period ended 2026-08-01, after the signature was made.

The same locked archive is the corresponding-source archive for the static library linked into
`libavcodec`. Bloom does not modify the source; the superbuild verifies the SHA-256 before
extraction and builds from the populated archive cache with downloads disabled.
