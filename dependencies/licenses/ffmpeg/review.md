# FFmpeg 8.1.2 intake review

## License and build boundary

The locked expression is `LGPL-2.1-or-later`. The recipe explicitly disables GPL, nonfree, and
LGPLv3-only selections, builds shared libraries, disables static libraries and programs, and uses
no crypto library. Its only external codec libraries are permissively licensed, source-built
static archives linked into `libavcodec` (OpenH264 BSD-2-Clause, Kvazaar BSD-3-Clause, dav1d
BSD-2-Clause, SVT-AV1 BSD-3-Clause-Clear with the AOM patent license, libvpx BSD-3-Clause, Opus
BSD-3-Clause; each has its own review beside this one); FFmpeg's version script keeps their symbols
private, and their locked source archives complete `libavcodec`'s corresponding source. The
selected source record includes FFmpeg’s upstream license summary; the corresponding source is the
exact locked release archive plus the reviewed patch below.

## Patch

`dependencies/patches/ffmpeg/0001-libkvazaar-pad-frames-to-whole-blocks.patch` changes only
`libavcodec/libkvazaar.c`: the wrapper refused widths and heights that are not multiples of eight,
although Kvazaar codes any even extent when the caller pads the picture to whole blocks (Kvazaar's
own command-line tool does this and Kvazaar writes the conformance window that crops it). The
patched wrapper allocates the padded picture, repeats the last column and row into the padding and
accepts every even extent. The patch is LGPL-2.1-or-later like the file it changes, applied in the
lock's recorded order, and ships with the corresponding source.

FFmpeg is admitted inside `bloom-media-worker` on Linux, macOS and Windows (ADR 0035, 2026-10-31),
always as LGPL shared libraries private to the worker's generation directory: Linux `lib/` with a
`$ORIGIN/lib` rpath, macOS `lib/` inside `Bloom.app` with `@rpath` install names, Windows DLLs beside
the worker executable. The Bloom desktop target has no FFmpeg dependency and must remain free of
`libav*` symbols and links. The worker boundary does not change LGPL reverse-engineering,
relinking, notice, source, or transitive-license duties; the shipped shared libraries remain
replaceable by the user on every platform.

## Allow-list rationale

The configure vector is checked byte-for-byte against
`dependencies/superbuild/projects/ffmpeg.configure-arguments`:

- Containers: MOV, Matroska, MXF, MPEG-TS, WAV, MP3, and AAC demuxers; MOV, MP4, Matroska, WebM,
  MXF, and WAV muxers.
- Decode: H.264, HEVC, VP9 (FFmpeg's native decoder), AV1 (`libdav1d`), ProRes, DNxHD, MJPEG,
  TIFF, AAC, MP3, Opus (FFmpeg's native decoder), and the explicitly enumerated native PCM
  decoders. WebM is read by the Matroska demuxer.
- Encode: `libopenh264` (H.264), `libkvazaar` (HEVC), `libsvtav1` (AV1), `libvpx-vp9` (VP9),
  `libopus` (Opus), `prores_ks`, DNxHD, TIFF, AAC, the NVIDIA NVENC (`h264_nvenc`, `hevc_nvenc`)
  and VA-API H.264/HEVC hardware encoders, and the explicitly enumerated native PCM encoders.
- Parsers and bitstream filters are limited to the entries needed by those paths; the file
  protocol is the only protocol.
- Programs, documentation, network access, autodetection, and all unspecified components remain
  disabled. On x86 and x86-64 targets FFmpeg's hand-written x86 assembly is built with the
  superbuild-private NASM 2.16.03 build tool (`nasm.cmake`, passed through `--x86asmexe`); NASM is
  never linked or shipped. Other targets use FFmpeg's inline/NEON code. The build is
  position-independent and shared-only.
- Platform adjustments change the toolchain and loader naming only: macOS adds
  `--install-name-dir=@rpath`; Windows adds `--toolchain=msvc --target-os=win64` and an `--arch`.
  The codec wrappers (`libopenh264`, `libkvazaar`, `libdav1d`, `libsvtav1`, `libvpx`, `libopus`)
  are enabled on every platform; `--pkg-config-flags=--static` makes configure read the static
  archives' runtime libraries. No platform adds a GPL, nonfree or version-3 component.
- NVENC (`--enable-ffnvcodec --enable-nvenc`) compiles against the MIT nv-codec-headers and loads
  the NVIDIA driver's encode library at run time; it needs neither `--enable-nonfree` nor
  `--enable-gpl`, and nothing from NVIDIA is linked or shipped. macOS builds omit it.
- VA-API is enabled on Linux when libva development files are present, with only the H.264 and
  HEVC VA-API hardware accelerators. `BLOOM_FFMPEG_ENABLE_VAAPI=OFF` is the explicit fallback when
  libva is unavailable; macOS and Windows never enable it.

## Patent and authorization posture

H.264 and HEVC decoding and encoding ship with Bloom (owner decision, 2026-10-31): software
encode is the bundled OpenH264 and Kvazaar, with NVENC and VA-API hardware encode preferred when a
device answers. AV1, VP9 and Opus encode ship the same way through SVT-AV1, libvpx and libopus;
their upstream royalty-free patent grants (the AOM Patent License 1.0 and Google's VP8/VP9 grant)
are reproduced in `THIRD_PARTY_NOTICES.md`. Codec patent questions outside ProRes are covered by
Bloom's existing deployment warning; no patent license is claimed. VA-API hardware use is an implementation capability, not
authorization evidence.

FFmpeg’s ProRes implementation is never Apple-authorization evidence. The future worker may use
`prores_ks` for the reviewed preview workflow only, and every capability report must retain
`apple_authorized=false` and `delivery_qualified=false` until an independently authorized provider
passes Bloom’s qualification and legal gates.
