# dr_wav 0.14.6 Provenance (Vendored In-Tree Header)

Reviewed: 2026-09-15

## Source

`dr_wav` is the single-header WAV decoder from the `mackron/dr_libs` repository. It is compiled
directly into Bloom's Qt-free audio adapter and is not acquired by the dependency superbuild, so it
has no lock entry or qualified-prefix artifact.

- Upstream project: <https://github.com/mackron/dr_libs>
- Pinned commit: `019e75ba8e9ac47222dfb87671029812069f68dc` (2026-08-31), the immutable tree used for
  every vendored file below.
- Header URL:
  `https://raw.githubusercontent.com/mackron/dr_libs/019e75ba8e9ac47222dfb87671029812069f68dc/dr_wav.h`
- License URL:
  `https://raw.githubusercontent.com/mackron/dr_libs/019e75ba8e9ac47222dfb87671029812069f68dc/LICENSE`

Acquisition was over HTTPS on 2026-09-15. SHA-256 values were computed over the downloaded bytes
before they were copied into the repository. The header and license are unmodified.

| Vendored path | Bytes | SHA-256 |
| --- | ---: | --- |
| `src/media/third_party/dr_wav/dr_wav.h` | 369705 | `03e70c1a2d9787cd7ed3e966c075bea7bac6373f759db9cd7ca9ccdfc4ec4493` |
| `src/media/third_party/dr_wav/LICENSE` | 2598 | `dd1c647e6f767f8ff4b2dfae0fed314726600a01e0cf1ef556afddd5fa96ff15` |
| `dependencies/licenses/dr_wav/LICENSE` | 2598 | `dd1c647e6f767f8ff4b2dfae0fed314726600a01e0cf1ef556afddd5fa96ff15` |

No detached signature, Sigstore bundle, or signed release identity was published with this direct
header acquisition. Re-review is required for any upstream commit change or capability expansion.

## Status

This is checked-in source, not a superbuild component. The dependency-intake contract therefore
requires the license and review records and repository notices, but does not permit fabricating a
lock entry for it.
