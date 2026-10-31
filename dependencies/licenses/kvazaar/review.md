# Kvazaar 2.3.2 intake review

## Role and boundary

Kvazaar is Bloom's bundled HEVC software encoder. The superbuild compiles the unmodified
BSD-3-Clause release from its pinned archive on Linux, macOS and Windows (CMake, the adjacent
`kvazaar.configure-arguments` vector) into a static, position-independent, library-only archive:
no command-line tool, no tests, no Crypto++ integration. FFmpeg's LGPL `libkvazaar` wrapper links
that archive into the worker's shared `libavcodec`; FFmpeg's version script keeps every Kvazaar
symbol private. Kvazaar's x86 kernels are compiler intrinsics with run-time CPU dispatch, so no
assembler is involved.

## License

Kvazaar is BSD-3-Clause since 2.0 (`LICENSE`, byte-identical to the archive's copy). The compiled
library also contains Colin Plumb's public-domain MD5 (`src/extras/libmd5.c`). On MSVC only it
adds the upstream ISC-licensed thread wrapper (`src/threadwrapper/`, Tampere University) and the
BSD-2-Clause NetBSD `getopt` (`src/extras/getopt.c`); their notices are reproduced in
`THIRD_PARTY_NOTICES.md`. The "Greatest" test library (ISC) is not built. Because the archive is
linked into the LGPL `libavcodec`, the lock records the corresponding-source obligation with the
exact source archive digest.

## Patent posture

The owner's decision (2026-10-31) accepts bundling a source-built HEVC encoder; codec patent
questions outside ProRes are covered by Bloom's existing deployment warning. No patent license is
claimed.

## Qualification posture

Software HEVC is the FFmpeg worker's `hevc` encode declaration, served by `libkvazaar`, and is the
silent fallback when VA-API hardware encode is unavailable or refused. It is a development
qualification exercised by the encode/reopen round trip.
