# minimp3 (2026-07-27 snapshot) Provenance (Vendored In-Tree Headers)

Reviewed: 2026-09-15

## Source

`minimp3` is the single-header MP3 decoder from `lieff/minimp3`. The extended streaming API is
vendored beside the core header because bounded probe/decode uses its frame-counting interface.
Both headers are compiled directly into Bloom's Qt-free audio adapter and are not acquired by the
dependency superbuild.

- Upstream project: <https://github.com/lieff/minimp3>
- Pinned commit: `ea99364f61c14656440e8d77e9c233ccf3124633` (2026-07-27), the immutable tree used for
  every vendored file below.
- Core header URL:
  `https://raw.githubusercontent.com/lieff/minimp3/ea99364f61c14656440e8d77e9c233ccf3124633/minimp3.h`
- Extended header URL:
  `https://raw.githubusercontent.com/lieff/minimp3/ea99364f61c14656440e8d77e9c233ccf3124633/minimp3_ex.h`
- License URL:
  `https://raw.githubusercontent.com/lieff/minimp3/ea99364f61c14656440e8d77e9c233ccf3124633/LICENSE`

Acquisition was over HTTPS on 2026-09-15. SHA-256 values were computed over the downloaded bytes
before they were copied into the repository. The headers and license are unmodified.

| Vendored path | Bytes | SHA-256 |
| --- | ---: | --- |
| `src/media/third_party/minimp3/minimp3.h` | 76831 | `57e437c5c1f0e8b243885d3929c8973b5e6c778451e0100ab4251d19915cb3ad` |
| `src/media/third_party/minimp3/minimp3_ex.h` | 50696 | `8437f3fc1d4d8ab2269a1624f5380a08df1967a048f8887789bae6e25db7db79` |
| `src/media/third_party/minimp3/LICENSE` | 6556 | `6a1ee543e5282cd9061881edf462e6fdab181f328da71fc2c9a6950a80e94d01` |
| `dependencies/licenses/minimp3/LICENSE` | 6556 | `6a1ee543e5282cd9061881edf462e6fdab181f328da71fc2c9a6950a80e94d01` |

No detached signature, Sigstore bundle, or signed release identity was published with this direct
header acquisition. Re-review is required for any upstream commit change or capability expansion.

## Status

This is checked-in source, not a superbuild component. The dependency-intake contract therefore
requires the license and review records and repository notices, but does not permit fabricating a
lock entry for it.
