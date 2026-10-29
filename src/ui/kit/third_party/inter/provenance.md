# Inter provenance

Reviewed: 2026-10-30. Upstream: <https://rsms.me/inter/>. Pinned release: **v4.1**.

Every vendored face is unmodified and was extracted from
<https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip>.
Archive SHA-256, computed from the downloaded bytes (re-downloaded and re-verified 2026-10-30):
`9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e`.

`manifest.json` records archive members, byte sizes, SHA-256 digests computed from those
extracted bytes, and which Bloom module consumes each file. `LICENSE` is the byte-identical
upstream LICENSE.txt. The SIL Open Font License 1.1 permits bundling and redistribution with Bloom
when its copyright and license travel with the fonts; the font is not sold separately or modified.
The license also appears in Bloom's embedded license catalog and THIRD_PARTY_NOTICES.md.

| File | Archive member | Consumer | SHA-256 |
| --- | --- | --- | --- |
| `LICENSE` | `LICENSE.txt` | ui | `262481e844521b326f5ecd053e59b98c8b2da78c8ee1bdbb6e8174305e54935a` |
| `InterVariable.ttf` | `InterVariable.ttf` | ui | `4989b125924991b90d05b2d16e0e388c48f7d5bb8b30539bbf9c755278d0ccaf` |
| `Inter-Regular.ttf` | `extras/ttf/Inter-Regular.ttf` | render | `40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82` |
| `Inter-Medium.ttf` | `extras/ttf/Inter-Medium.ttf` | render | `97ad806f526e41546d46365bb3a393145f75b7b1568913db74549ad8b8dba872` |
| `Inter-SemiBold.ttf` | `extras/ttf/Inter-SemiBold.ttf` | render | `78a843fade9d4612a5567302fb595b56976eb5fcebf4fea5a5912d638bafcde3` |

## Interface: the upright variable face

Bloom chrome uses `InterVariable.ttf`, the upright variable font (axes `opsz` 14-32, default 14,
and `wght` 100-900, default 400). Its name table declares the family `Inter Variable`; Qt
registers it under that name. Each type role is a named weight on the `wght` axis (Medium 500 for
Ui/UiSmall, SemiBold 600 for UiSmallStrong/Title), applied with `QFont::setWeight()` and
`QFont::setVariableAxis()`, and `opsz` is pinned to 14. It is registered with Qt's application font
database from the `kinetik_fonts` resource. The italic variable face, `Inter.ttc` and the display
cuts are not vendored.

## Text layers: the static faces

The static Regular, Medium and SemiBold faces are not interface fonts. They are text-layer
content: the document's closed embedded face mapping (font ids 1-3) names them, the render module
embeds their bytes at configure time for its Qt-free CPU text rasterizer (with per-face byte-count
assertions in `src/render/CMakeLists.txt`), and the platform font catalogue's tests read them as
fixtures. Their bytes are unchanged from the 2026-09-15 intake. They are not in the Qt resource
pack, and interface typography never selects them. DejaVu Sans Book remains at its own path as the
text-layer default.

Changing this intake requires a new archive verification, manifest, license review and golden
approval.
