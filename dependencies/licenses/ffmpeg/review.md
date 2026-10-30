# FFmpeg 8.1.2 intake review

## License and build boundary

The locked expression is `LGPL-2.1-or-later`. The recipe explicitly disables GPL, nonfree, and
LGPLv3-only selections, builds shared libraries, disables static libraries and programs, and uses
no external codec or crypto libraries. The selected source record includes FFmpeg’s upstream
license summary; the corresponding-source record points to the exact locked release archive.

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

- Containers: MOV, Matroska, MXF, MPEG-TS, WAV, MP3, and AAC demuxers; MOV, Matroska, MXF, and WAV
  muxers.
- Decode: H.264, HEVC, ProRes, DNxHD, MJPEG, TIFF, AAC, MP3, and the explicitly enumerated native
  PCM decoders.
- Encode: `prores_ks`, DNxHD, TIFF, AAC, and the explicitly enumerated native PCM encoders.
- Parsers and bitstream filters are limited to the entries needed by those paths; the file
  protocol is the only protocol.
- Programs, documentation, network access, autodetection, and all unspecified components remain
  disabled. On x86 and x86-64 targets FFmpeg's hand-written x86 assembly is built with the
  superbuild-private NASM 2.16.03 build tool (`nasm.cmake`, passed through `--x86asmexe`); NASM is
  never linked or shipped. Other targets use FFmpeg's inline/NEON code. The build is
  position-independent and shared-only.
- Platform adjustments change the toolchain and loader naming only: macOS adds
  `--install-name-dir=@rpath`; Windows adds `--toolchain=msvc --target-os=win64` and an `--arch`.
  The libopenh264 wrapper is part of the Linux x86-64 profile only (its link-time stub and Cisco's
  runtime fetch are Linux-specific), so macOS and Windows builds omit it. No platform adds a GPL,
  nonfree or version-3 component.
- VA-API is enabled on Linux when libva development files are present, with only the H.264 and
  HEVC VA-API hardware accelerators. `BLOOM_FFMPEG_ENABLE_VAAPI=OFF` is the explicit fallback when
  libva is unavailable; macOS and Windows never enable it.

## Patent and authorization posture

H.264 and HEVC decoding is accepted as an internal preview/ingest implementation subject to the
deployment territory and counsel review; the build does not provide H.264 or HEVC software
encoding. Software encode is deliberately limited to `prores_ks`, DNxHD, PCM, AAC, and TIFF,
pending a separate OpenH264/KVazaar decision for any future H.264/HEVC encode path. VA-API
hardware use is an implementation capability, not authorization evidence.

FFmpeg’s ProRes implementation is never Apple-authorization evidence. The future worker may use
`prores_ks` for the reviewed preview workflow only, and every capability report must retain
`apple_authorized=false` and `delivery_qualified=false` until an independently authorized provider
passes Bloom’s qualification and legal gates.
