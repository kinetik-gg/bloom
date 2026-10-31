# Opus 1.5.2 intake review

## Role and boundary

libopus is Bloom's bundled Opus audio encoder for the WebM formats. The superbuild compiles Xiph's
unmodified release from its pinned archive on Linux, macOS and Windows (Meson, the adjacent
`opus.configure-arguments` vector) into a static, position-independent, library-only archive: no
demo programs, tests or documentation, and the neural packet-loss concealment, DRED and OSCE
features are off, so no model weights are compiled in. x86 and ARM kernels are compiler intrinsics
with run-time dispatch; no assembler is needed. FFmpeg's LGPL `libopus` wrapper links the archive
into the worker's shared `libavcodec`, and FFmpeg's version script keeps every Opus symbol private.
FFmpeg's own LGPL Opus decoder reads exported audio back for the reopen QC.

## License

Opus is BSD-3-Clause (`LICENSE`, byte-identical to the archive's `COPYING`). The Opus codec's
contributors license their patents for Opus implementations royalty-free; those terms are
published at the Opus project's license page and need no shipped file. Because the archive is
linked into the LGPL `libavcodec`, the lock records the corresponding-source obligation with the
exact source archive digest. No GPL or nonfree code is involved.

## Qualification posture

Opus encode is an audio declaration of the FFmpeg worker (`opus`, served by `libopus`) for WebM
only, at 48 kHz. It is a delivery encoder; Opus is lossy, so its reopen QC compares sample counts
and layout, not PCM bytes.
