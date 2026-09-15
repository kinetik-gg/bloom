# miniaudio 0.11.25 Provenance (Vendored In-Tree Header)

Reviewed: 2026-09-15

## Source

`miniaudio` is the single-header audio device library from `mackron/miniaudio`. It is compiled
directly into Bloom's private device backend and is not acquired by the dependency superbuild, so it
has no lock entry or qualified-prefix artifact.

- Upstream project: <https://github.com/mackron/miniaudio>
- Pinned commit: `9634bedb5b5a2ca38c1ee7108a9358a4e233f14d` (2026-03-03), the immutable tree used for
  every vendored file below.
- Header URL:
  `https://raw.githubusercontent.com/mackron/miniaudio/9634bedb5b5a2ca38c1ee7108a9358a4e233f14d/miniaudio.h`
- License URL:
  `https://raw.githubusercontent.com/mackron/miniaudio/9634bedb5b5a2ca38c1ee7108a9358a4e233f14d/LICENSE`

Acquisition was over HTTPS on 2026-09-15. SHA-256 values were computed over the downloaded bytes
before they were copied into the repository. The header and license are unmodified.

| Vendored path | Bytes | SHA-256 |
| --- | ---: | --- |
| `src/media/third_party/miniaudio/miniaudio.h` | 4108168 | `ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89` |
| `src/media/third_party/miniaudio/LICENSE` | 2597 | `457f1b500e0adf6bc059edddfa78a2f62012e7c3bb43476c20e0bd23b25ba0eb` |
| `dependencies/licenses/miniaudio/LICENSE` | 2597 | `457f1b500e0adf6bc059edddfa78a2f62012e7c3bb43476c20e0bd23b25ba0eb` |

No detached signature, Sigstore bundle, or signed release identity was published with this direct
header acquisition. Re-review is required for any upstream commit change or capability expansion.

## Status

This is checked-in source, not a superbuild component. The dependency-intake contract therefore
requires the license and review records and repository notices, but does not permit fabricating a
lock entry for it.
