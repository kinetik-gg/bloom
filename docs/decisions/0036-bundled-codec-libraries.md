# ADR 0036: Bundled Codec Libraries In The Media Worker

Status: accepted

Date: 2026-10-31

Issues: #368 (encoders), #369, #398 (VP9/AV1 decode and encode)

## Context

ADR 0020 admitted H.264 software encode only through Cisco's separately distributed OpenH264
binary: the superbuild built a private link-time stub, the worker carried a Bloom-owned loader shim,
and the desktop asked the artist to consent to Cisco's license and downloaded the binary into a
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

| Library | License | Role | FFmpeg component |
| --- | --- | --- | --- |
| OpenH264 2.6.0 | BSD-2-Clause | H.264 software encode | `libopenh264` encoder |
| Kvazaar 2.3.2 | BSD-3-Clause | HEVC software encode (8-bit Main) | `libkvazaar` encoder |
| dav1d 1.5.4 | BSD-2-Clause | AV1 decode | `libdav1d` decoder, `av1` parser |
| SVT-AV1 4.2.0 | BSD-3-Clause-Clear, AOM Patent License 1.0 | AV1 software encode (8-bit and 10-bit) | `libsvtav1` encoder |
| libvpx 1.17.0 | BSD-3-Clause, Google VP8/VP9 patent grant | VP9 software encode (Profile 0 and 2) | `libvpx_vp9` encoder |
| Opus 1.5.2 | BSD-3-Clause | Opus audio encode | `libopus` encoder |
| nv-codec-headers 12.1.14.1 | MIT | NVIDIA NVENC H.264 and HEVC Main/Main 10 (header-only; the driver is loaded at run time) | `h264_nvenc`, `hevc_nvenc` |

VP9 and Opus decode need no external library: FFmpeg's native `vp9` and `opus` decoders and parsers
are enabled. WebM is read by the existing Matroska demuxer and written by FFmpeg's `webm` muxer.

Each is a static, position-independent archive (dependency-intake.md, "Bundled codec
libraries"), so the worker generation directory gains no runtime file and FFmpeg's version script
keeps the codec symbols private. Each has its own lock component, license/provenance/security
review, notice and recipe argument vector checked byte for byte. The patent grants that ship with
SVT-AV1 and libvpx are notice files of their lock components and are reproduced in
`THIRD_PARTY_NOTICES.md`.

Encode selection stays ADR 0035's chain, now NVENC → VA-API → software on Linux and NVENC →
software on Windows (`ffmpeg-nvenc`, `ffmpeg-vaapi`, `ffmpeg`). NVENC is tried whenever the
encoder probe found an NVIDIA driver and the preference is not Software (Automatic included: it is
fast and gives real 10-bit HEVC); VA-API when Hardware is chosen, or for Automatic HEVC when a
render node encodes HEVC. Any refusal at `begin()` (no driver, a driver that cannot open the
profile) falls through to the next route and finally to `ffmpeg`, whose software handshake always
declares the bundled encoders. The NVENC worker generation gets an 8 GiB address-space floor
because the NVIDIA driver reserves several GiB when it opens CUDA. nv-codec-headers' 12.1 line
needs a Linux driver 530.41.03 or newer (Windows 531.61); an older driver simply falls through. The fallback is recorded
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

Web delivery (document 1.33): `vp9-webm` (VP9 with Opus), `av1-webm` (AV1 with Opus) and
`av1-mp4` (AV1 with AAC) are export formats with the codec profiles `8bit` and `10bit`, and the
built-in presets AV1 MP4, AV1 WebM and VP9 WebM use 8-bit. They are software-only, served by the
FFmpeg worker's software generation on every platform, so they never fall back and are never
refused; a Hardware preference is ignored for them. Both encoders deliver real 10-bit, so no depth
adjustment applies. They encode at constant quality (CRF 28, SVT-AV1 preset 8, libvpx good
deadline at speed 4) with a one-second keyframe interval; the document's bit rate applies to H.264
and HEVC only. Opus is defined at 48 kHz, so a WebM output mixes its audio at 48 kHz whatever the
output's sample rate says. Matroska and WebM store millisecond timestamps, so the worker's reopen
check compares a WebM stream's frame and sample times on that grid (MOV, MP4 and MXF stay exact),
and the lossy Opus audio is compared with the same float tolerance as AAC. libvpx and SVT-AV1 leave
packet durations unset; the worker gives their packets one frame each so an MP4 track's duration is
exact.

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
