# ADR 0036: Bundled Codec Libraries In The Media Worker

Status: accepted

Date: 2026-10-31

Issues: #368 (encoders), #369, #398 (VP9/AV1 decode)

## Context

ADR 0020 admitted H.264 software encode only through Cisco's separately distributed OpenH264
binary: the superbuild built a private link-time stub, the worker carried a Bloom-owned loader shim,
and the desktop asked the artist to consent to Cisco's licence and downloaded the binary into a
per-user directory. Cisco publishes that binary for Linux x86-64 only, so ADR 0035 dropped
`libopenh264` from the macOS and Windows FFmpeg builds. HEVC had no software encoder at all.

The owner's decisions (2026-10-31):

- "You can use or configure any codecs as long as it stays within the boundary we set, and ship it
  alongside Bloom. I don't want users to install anything other than Bloom itself."
- "I don't want to see 'unavailable'. Average artists would not care about installing missing
  deps."
- The boundary is LGPL shared FFmpeg in the worker plus permissive libraries (BSD, MIT, ISC,
  Apache). No GPL (x264, x265), no nonfree, no `--enable-gpl`, no `--enable-version3`. Codec
  patents outside ProRes are covered by the existing deployment warning.

## Decision

Permissively licensed codec libraries are built from pinned source archives and linked into the
worker's shared `libavcodec` through FFmpeg's LGPL wrappers, on Linux, macOS and Windows:

| Library | Licence | Role | FFmpeg component |
| --- | --- | --- | --- |
| OpenH264 2.6.0 | BSD-2-Clause | H.264 software encode | `libopenh264` encoder |
| Kvazaar 2.3.2 | BSD-3-Clause | HEVC software encode (8-bit Main) | `libkvazaar` encoder |

Each is a static, position-independent archive (dependency-intake.md, "Bundled codec
libraries"), so the worker generation directory gains no runtime file and FFmpeg's version script
keeps the codec symbols private. Each has its own lock component, licence/provenance/security
review, notice and recipe argument vector checked byte for byte.

Encode selection stays ADR 0035's chain. On Linux a hardware request puts `ffmpeg-vaapi` first;
any refusal at `begin()` (no render node, a driver that cannot open the profile) falls through to
`ffmpeg`, whose software handshake always declares the bundled encoders. The fallback is recorded
in `MediaProviderProvenanceV1` and the export evidence names the encoder that served; no artist
text reports it.

Kvazaar is an 8-bit encoder. An HEVC Main 10 request that no 10-bit encoder serves is delivered as
8-bit HEVC Main rather than refused, and never silently: the worker's reopen QC reports the
delivered profile and bit depth with the reason (`EncodeQcV1::profile`, `bitDepth`,
`adjustment`), the host copies the reason into `MediaProviderProvenanceV1::adjustment`, logs it,
and the export evidence states the delivered profile. FFmpeg's `libkvazaar` wrapper refused
extents that are not multiples of eight; the reviewed patch
`dependencies/patches/ffmpeg/0001-libkvazaar-pad-frames-to-whole-blocks.patch` pads frames to
whole blocks so Kvazaar's conformance window crops them, and every even extent encodes.

Removed with this decision: the Cisco runtime fetch (`OpenH264Runtime` install/verify/locate, the
`runtimeFetch` lock record, the consent entry in `Bloom.conf`), the worker's `libopenh264.so.8`
shim, the private link-time stub, the typed "H.264 encoder not installed" refusal and HEVC's
hardware-only rule. The host never produces `ExportAvailabilityCode::EncoderNotInstalled` or
`HardwareEncoderMissing`,
nor the `InstallOpenH264`, `LocateOpenH264` or `ChooseSoftwareEncoder` remedies; those enum values
and the compile-only `verifyH264RuntimeV1()`/`locateH264RuntimeV1()`/`installH264RuntimeV1()`
helpers remain only until the desktop deletes its install flow.

## Verification boundary

Built and tested on Linux x86-64. The macOS and Windows branches of the codec recipes (Meson with
Apple clang or MSVC, the MSVC static-runtime selection and `<name>.lib` naming) have not been
built on those systems.

## Consequences

ADR 0020's "H.264 review encode intake" runtime-fetch mechanism, ADR 0019's runtime-fetched-library
pattern for OpenH264 and ADR 0035's "OpenH264 is the Linux x86-64 profile" note are superseded. The
corresponding source of the shipped `libavcodec` now includes each bundled codec's locked source
archive.
