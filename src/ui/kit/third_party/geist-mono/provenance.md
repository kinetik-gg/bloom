# Geist Mono Provenance

Reviewed: 2026-09-02

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

The archive SHA-256 above was computed over the exact bytes downloaded from that URL on
2026-09-02 with `sha256sum`, not copied from an upstream publication.

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

Only the Geist Mono family is vendored. The archive also carries Geist (sans) and Geist Pixel;
Bloom's interface sans is Plus Jakarta Sans, so those are not vendored.

| Path | SHA-256 | Registers as |
| --- | --- | --- |
| `LICENSE` | `c683bfbcc7e087f5d37a54ef628f10387c451a83ddc459b151403a164ac46c90` | -- |
| `GeistMono-Regular.ttf` | `42d8ad2e610238e64e8abfcde3037c63f7850a73928742b7ab7229d897bcb155` | `Geist Mono` |
| `GeistMono-Medium.ttf` | `90b15711dc3779b2e64e8aff5228154dd019a90bce4947549c4a8a8a43f2ac25` | `Geist Mono Medium` (typographic family `Geist Mono`) |

## Static faces, not the variable font

The same reasoning as the interface family applies: the shipped monospaced weight set is Regular
400 and Medium 500, and the static faces resolve identically on every Qt 6.8 platform without
depending on the platform font engine's named-instance handling. `variable/GeistMono[wght].ttf` is
not vendored.

## Status

Pinned and reviewed. Adding a weight, changing a face, or moving to a new upstream release
replaces this record wholesale.
