# nv-codec-headers 12.1.14.1 provenance

- Upstream release: nv-codec-headers n12.1.14.1 (FFmpeg project).
- Selection rationale: the newest release on the 12.1 line, which FFmpeg 8.1 accepts and whose
  minimum NVIDIA driver is Linux 530.41.03 / Windows 531.61.
- Archive URL: <https://github.com/FFmpeg/nv-codec-headers/releases/download/n12.1.14.1/nv-codec-headers-12.1.14.1.tar.gz>
  (the release's distribution tarball)
- Archive retrieval date: 2026-10-31 (UTC date)
- Archive size: 78,065 bytes
- Archive SHA-256: `sha256:043e46265517855f13bcb98b3a225b2761b2a83983b1645fa1d05876f1c3a781`
- Upstream signature: none published; the lock pins the archive digest.

Bloom installs the headers unmodified. The superbuild verifies the SHA-256 before extraction and
installs from the populated archive cache with downloads disabled.
