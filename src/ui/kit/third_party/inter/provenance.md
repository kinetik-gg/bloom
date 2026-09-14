# Inter provenance

Reviewed: 2026-09-15. Upstream: <https://rsms.me/inter/>. Pinned release: **v4.1**.

The unmodified static Regular, Medium and SemiBold faces were extracted from
<https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip>.
Archive SHA-256, computed from the downloaded bytes:
`9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e`.

`manifest.json` records archive members, byte sizes and SHA-256 digests computed from those
extracted bytes. `LICENSE` is the byte-identical upstream LICENSE.txt. The SIL Open Font
License 1.1 permits bundling and redistribution with Bloom when its copyright and license
travel with the fonts; the font is not sold separately or modified. The license also appears
in Bloom's embedded license catalog and THIRD_PARTY_NOTICES.md.

| File | SHA-256 |
| --- | --- |
| `LICENSE` | `262481e844521b326f5ecd053e59b98c8b2da78c8ee1bdbb6e8174305e54935a` |
| `Inter-Regular.ttf` | `40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82` |
| `Inter-Medium.ttf` | `97ad806f526e41546d46365bb3a393145f75b7b1568913db74549ad8b8dba872` |
| `Inter-SemiBold.ttf` | `78a843fade9d4612a5567302fb595b56976eb5fcebf4fea5a5912d638bafcde3` |

The extracted name tables (IDs 1/2/16/17) declare Regular as `Inter`, Medium as
`Inter Medium` and SemiBold as `Inter SemiBold`; both heavier faces also declare typographic
family `Inter`. TypeRole names the static family explicitly before the common family and
platform fallback. Ui and UiSmall use Medium; Title uses SemiBold. Tests verify actual Qt
resolution and all resource digests. Geist Mono remains the value face.

DejaVu Sans Book remains at its existing path solely for the unchanged render text source.
Changing this intake requires a new archive verification, manifest, license review and golden
approval. No variable, italic or additional weight is shipped.
