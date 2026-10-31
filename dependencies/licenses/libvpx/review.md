# libvpx 1.17.0 intake review

## Role and boundary

libvpx is Bloom's bundled VP9 software encoder. The superbuild compiles the unmodified upstream
release from its pinned archive on Linux, macOS and Windows (libvpx's configure script, the adjacent
`libvpx.configure-arguments` vector) into a static, position-independent, library-only archive with
the VP9 encoder and its high-bit-depth (Profile 2, 10-bit) path only: no VP8, no VP9 decoder
(FFmpeg's native decoder reads VP9), no tools, examples, unit tests, WebM I/O or libyuv. x86
assembly is built with the superbuild-private NASM; ARM uses libvpx's NEON kernels. On Windows the
configure script's Visual Studio target builds with the static runtime FFmpeg's MSVC link uses.
FFmpeg's LGPL `libvpx` wrapper links the archive into the worker's shared `libavcodec`, and FFmpeg's
version script keeps every libvpx symbol private.

## License

libvpx is BSD-3-Clause (`LICENSE`). Google's additional patent grant for VP8/VP9 (`PATENTS`) is a
notice file of the lock component and of `THIRD_PARTY_NOTICES.md`. The compiled x86 assembly
include `x86inc.asm` is ISC (`x86inc-LICENSE`). The vendored libwebm, libyuv, googletest and
nalloc are disabled or test-only and are not built. Because the archive is linked into the LGPL
`libavcodec`, the lock records the corresponding-source obligation with the exact source archive
digest. No GPL or nonfree code is involved.

## Qualification posture

VP9 encode is a declaration of the FFmpeg worker's software generation (`vp9`, served by
`libvpx-vp9`) for WebM. It runs on the CPU in the worker, takes 8-bit (Profile 0) or 10-bit
(Profile 2) 4:2:0 frames and is reopened through FFmpeg's native VP9 decoder for the export QC.
It is a delivery encoder, not an archival or delivery-qualified pipeline.
