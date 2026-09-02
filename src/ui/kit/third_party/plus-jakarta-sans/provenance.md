# Plus Jakarta Sans Provenance

Reviewed: 2026-09-02

## Component

| Field | Value |
| --- | --- |
| Upstream project | Plus Jakarta Sans |
| Upstream repository | <https://github.com/tokotype/PlusJakartaSans> |
| Pinned release | `2.7.1` |
| Source archive | <https://github.com/tokotype/PlusJakartaSans/releases/download/2.7.1/PlusJakartaSans-2.7.1.zip> |
| Source archive SHA-256 | `4bfc5cdf97d750423bb3d1d40ed8e529bc92288924d9c65e18ff486acefac66c` |
| License | SIL Open Font License 1.1 |
| License file | `LICENSE` (upstream `PlusJakartaSans-2.7.1/OFL.txt`, byte-identical) |
| Modified | No. Every file below is byte-identical to its file in the pinned archive. |

The archive SHA-256 above was computed over the exact bytes downloaded from that URL on
2026-09-02 with `sha256sum`, not copied from an upstream publication.

The upstream license file is named `OFL.txt`. It is stored here as `LICENSE`, byte for byte, so
that the repository hygiene checker's vendored-attribution rule
(`tools/quality/repository_checks/repository_checks.cpp`, `kVendoredLicenseFilenames`) finds it.
Only the filename differs; the digest below is over the same bytes upstream ships.

## Vendored files

| Path | SHA-256 | Registers as |
| --- | --- | --- |
| `LICENSE` | `995c7199cab65954f545996326755daee7b63cc6b42b06c13da1f9502ab08a99` | -- |
| `PlusJakartaSans-Regular.ttf` | `6bcfbb10639b8a206b4f8a0a1a29459e7255b1481c95e20d587dd1f1a4b24646` | `Plus Jakarta Sans` |
| `PlusJakartaSans-Medium.ttf` | `8e60a920af7fcc9d9e2768aaa51c8595a1c72a9ff3da87c216a319023158ebf2` | `Plus Jakarta Sans Medium` (typographic family `Plus Jakarta Sans`) |
| `PlusJakartaSans-SemiBold.ttf` | `de75ddb3917c5a3f2b7d0cd65fcf63795710f2e1fd944199a73a01731e66fd43` | `Plus Jakarta Sans SemiBold` (typographic family `Plus Jakarta Sans`) |

## Static faces, not the variable font

`docs/ux/visual-language.md` prefers upstream variable TTFs "when they behave consistently through
the supported Qt version on all three platforms" and requires a tested static fallback otherwise.
This release ships `variable/PlusJakartaSans[wght].ttf`; Bloom vendors the three static faces
instead, because the shipped weight set is exactly three (Regular 400, Medium 500, SemiBold 600),
those three are what implemented components use, and a static face resolves identically on every
Qt 6.8 platform without depending on named-instance handling in the platform font engine.

Static faces name their heavier weights as separate families, so `src/ui/kit/fonts.cpp` asks for
the exact face first and the base family second. `src/ui/tests/kit_fonts_tests.cpp` asserts through
`QFontInfo` that the resolved face is the bundled one and that the title role does not collapse
onto the UI role's face with a synthesized weight.

## Status

Pinned and reviewed. Adding a weight, changing a face, or moving to a new upstream release
replaces this record wholesale.
