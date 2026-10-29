# Geist Mono Provenance

Reviewed: 2026-10-30 (first intake 2026-09-02)

## Component

| Field | Value |
| --- | --- |
| Upstream project | Geist (Geist Mono family) |
| Upstream repository | <https://github.com/vercel/geist-font> |
| Pinned release | `v1.7.2` |
| Source archive | <https://github.com/vercel/geist-font/releases/download/v1.7.2/geist-font-v1.7.2.zip> |
| Source archive SHA-256 | `7fc800d2ac6b92844895196e5041aca55d814c15db70c44f79b3b83ab82b04e2` |
| License | SIL Open Font License 1.1 |
| License file | `LICENSE` (upstream `geist-font/OFL.txt`, byte-identical) |
| Modified | No. Every file below is byte-identical to its file in the pinned archive. |

The archive SHA-256 above was computed over the exact bytes downloaded from that URL with
`sha256sum`, not copied from an upstream publication: first on 2026-09-02, and again on
2026-10-30 when the variable face was taken from the same archive.

The upstream license file is named `OFL.txt`. It is stored here as `LICENSE`, byte for byte, so
that the repository hygiene checker's vendored-attribution rule
(`tools/quality/repository_checks/repository_checks.cpp`, `kVendoredLicenseFilenames`) finds it.
Only the filename differs.

### Which release is "latest"

This repository's release tags are not monotonically ordered: `1.8.0` was published 2026-03-03,
and `v1.7.1` and `v1.7.2` were published after it, on 2026-05-20 and 2026-06-01. The pin above is
the most recently published release at review time (`v1.7.2`), chosen by publication date rather
than by version string, because the version strings do not order. A future re-pin must make the
same check rather than assuming the highest number is the newest.

## Vendored files

Only the upright Geist Mono variable face is vendored. The archive also carries Geist (sans),
Geist Pixel, the italic variable face, the static faces and web fonts; none of those are vendored.

| Path | Archive member | SHA-256 | Registers as |
| --- | --- | --- | --- |
| `LICENSE` | `geist-font/OFL.txt` | `c683bfbcc7e087f5d37a54ef628f10387c451a83ddc459b151403a164ac46c90` | -- |
| `GeistMono-Variable.ttf` | `geist-font/GeistMono/variable/GeistMono[wght].ttf` | `0e1af3f507a1c8dfbb03d13ffad585834cd45ed7ccb78c756c7ce7873d180d30` | `Geist Mono` |

`manifest.json` records the same members, byte sizes and digests; `bloom.ui.kit_fonts` verifies
them against the embedded bytes. The face is stored under a bracket-free name (the upstream name
`GeistMono[wght].ttf` is awkward in CMake lists and Qt resource paths); its bytes are unchanged.

## The variable face, not the static faces

The interface family is Inter Variable, so the monospaced family is variable too: one file per
family, and every role weight is a named point on the face's `wght` axis (100-900, default 400).
The value role uses Medium 500, set with `QFont::setWeight()` and `QFont::setVariableAxis()`, so
FreeType, CoreText and DirectWrite draw the same outlines. The static `GeistMono-Regular.ttf` and
`GeistMono-Medium.ttf` of the 2026-09-02 intake (SHA-256
`42d8ad2e610238e64e8abfcde3037c63f7850a73928742b7ab7229d897bcb155` and
`90b15711dc3779b2e64e8aff5228154dd019a90bce4947549c4a8a8a43f2ac25`) are removed; nothing else in
Bloom consumed them.

## Status

Pinned and reviewed. Adding a face, changing a face, or moving to a new upstream release replaces
this record wholesale.
