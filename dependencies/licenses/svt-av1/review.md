# SVT-AV1 4.2.0 intake review

## Role and boundary

SVT-AV1 is Bloom's bundled AV1 software encoder. The superbuild compiles the unmodified upstream
release from its pinned archive on Linux, macOS and Windows (CMake, the adjacent
`svt-av1.configure-arguments` vector) into a static, position-independent, library-only archive:
no encoder application, no tests, no link-time optimization and no git hash in the binary. x86
assembly is built with the superbuild-private NASM; ARM uses SVT-AV1's NEON/SVE kernels. FFmpeg's
LGPL `libsvtav1` wrapper links the archive into the worker's shared `libavcodec`, and FFmpeg's
version script keeps every SVT-AV1 symbol private.

## License

The library is the BSD 3-Clause Clear License (`LICENSE.md`); some files carry the Alliance for
Open Media BSD-2-Clause text (`LICENSE-BSD2.md`). Two compiled third-party directories carry their
own permissive terms: Intel's safestringlib (MIT, `safestringlib-LICENSE`) and the FAST corner
detector (BSD-3-Clause, `fastfeat-LICENSE`). The vendored googletest is test-only and is not built.
The Clear license grants no patent rights; the Alliance for Open Media Patent License 1.0
(`PATENTS.md`) does, on the condition that it is reproduced with any distribution, so it is a
notice file of the lock component and of `THIRD_PARTY_NOTICES.md`. Because the archive is linked
into the LGPL `libavcodec`, the lock records the corresponding-source obligation with the exact
source archive digest. No GPL or nonfree code is involved.

## Qualification posture

AV1 encode is a declaration of the FFmpeg worker's software generation (`av1`, served by
`libsvtav1`) for WebM and MP4. It runs on the CPU in the worker, takes 8-bit or 10-bit 4:2:0
frames and is reopened through the bundled dav1d decoder for the export QC. It is a delivery
encoder, not an archival or delivery-qualified pipeline.
