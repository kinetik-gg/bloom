# FFmpeg 8.1.2 intake review

## License and build boundary

The locked expression is `LGPL-2.1-or-later`. The recipe explicitly disables GPL, nonfree, and
LGPLv3-only selections, builds shared libraries, disables static libraries and programs, and uses
no crypto library. Its only external codec library is permissively licensed and source-built as a
static archive linked into `libavcodec` (OpenH264, BSD-2-Clause, with its own review beside this
one); FFmpeg's version script keeps its symbols private, and its locked source archive completes
`libavcodec`'s corresponding source. The
selected source record includes FFmpeg’s upstream license summary; the corresponding-source record
points to the exact locked release archive.

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

- Containers: MOV, Matroska, MXF, MPEG-TS, WAV, MP3, and AAC demuxers; MOV, MP4, Matroska, MXF,
  and WAV muxers.
- Decode: H.264, HEVC, ProRes, DNxHD, MJPEG, TIFF, AAC, MP3, and the explicitly enumerated native
  PCM decoders.
- Encode: `libopenh264` (H.264), `prores_ks`, DNxHD, TIFF, AAC, the VA-API H.264/HEVC hardware
  encoders, and the explicitly enumerated native PCM encoders.
- Parsers and bitstream filters are limited to the entries needed by those paths; the file
  protocol is the only protocol.
- Programs, documentation, network access, autodetection, and all unspecified components remain
  disabled. On x86 and x86-64 targets FFmpeg's hand-written x86 assembly is built with the
  superbuild-private NASM 2.16.03 build tool (`nasm.cmake`, passed through `--x86asmexe`); NASM is
  never linked or shipped. Other targets use FFmpeg's inline/NEON code. The build is
  position-independent and shared-only.
- Platform adjustments change the toolchain and loader naming only: macOS adds
  `--install-name-dir=@rpath`; Windows adds `--toolchain=msvc --target-os=win64` and an `--arch`.
  The `libopenh264` wrapper is enabled on every platform; `--pkg-config-flags=--static` makes
  configure read the static archive's runtime libraries. No platform adds a GPL, nonfree or
  version-3 component.
- VA-API is enabled on Linux when libva development files are present, with only the H.264 and
  HEVC VA-API hardware accelerators. `BLOOM_FFMPEG_ENABLE_VAAPI=OFF` is the explicit fallback when
  libva is unavailable; macOS and Windows never enable it.

## Patent and authorization posture

H.264 and HEVC decoding and H.264 encoding ship with Bloom (owner decision, 2026-10-31): software
H.264 encode is the bundled OpenH264, with VA-API hardware encode preferred on Linux when requested
and a device answers. Codec patent questions outside ProRes are covered by Bloom's existing deployment
warning; no patent licence is claimed. VA-API hardware use is an implementation capability, not
authorization evidence.

FFmpeg’s ProRes implementation is never Apple-authorization evidence. The future worker may use
`prores_ks` for the reviewed preview workflow only, and every capability report must retain
`apple_authorized=false` and `delivery_qualified=false` until an independently authorized provider
passes Bloom’s qualification and legal gates.
