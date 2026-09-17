# FFmpeg 8.1.2 provenance

- Upstream release: FFmpeg 8.1.2 “Hoare”, released 2026-06-17.
- Selection rationale: as of the 2026-09-17 intake, 8.1.2 is the latest stable point release on
  the maintained 8.1 release branch. The newer 9.0 line is a newly cut feature branch, so the
  maintained point-release line is the reviewed LTS-style dependency baseline for this worker
  intake.
- Archive URL: <https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz>
- Archive retrieval date: 2026-09-17 (UTC date)
- Archive size: 11,710,924 bytes
- Archive SHA-256: `sha256:464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c`
- Detached signature URL: <https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz.asc>
- Detached signature size: 520 bytes
- Detached signature SHA-256: `sha256:0a0963fccd70597838073f3e31b20f4a4d8cc2b5e577472c9a5a1f22624246f8`
- Signature evidence: `ffmpeg-8.1.2.tar.xz.asc` is checked in beside this record and was copied
  byte-for-byte from the downloaded official signature.
- Verification key: FFmpeg release signing key, fingerprint
  `FCF986EA15E6E293A5644F10B4322F04D67658D8`, downloaded from
  <https://ffmpeg.org/ffmpeg-devel.asc>.
- Verification result: `gpg --verify` reported a good signature from
  `FFmpeg release signing key <ffmpeg-devel@ffmpeg.org>`.

The same locked archive is the corresponding-source archive for the LGPL build. Bloom does not
modify the source archive; the superbuild verifies its SHA-256 before extraction and can build
from the populated archive cache with downloads disabled.
