# dav1d 1.5.4 intake review

## Role and boundary

dav1d is Bloom's bundled AV1 decoder. The superbuild compiles VideoLAN's unmodified, signed
BSD-2-Clause release from its pinned archive on Linux, macOS and Windows (Meson, the adjacent
`dav1d.configure-arguments` vector) into a static, position-independent, library-only archive: no
tools, tests, examples or documentation. x86 assembly is built with the superbuild-private NASM;
ARM uses dav1d's own assembly. FFmpeg's LGPL `libdav1d` wrapper links the archive into the worker's
shared `libavcodec`, and FFmpeg's version script keeps every dav1d symbol private.

## License

dav1d is BSD-2-Clause (`LICENSE`, byte-identical to the archive's `COPYING`); every compiled source
carries the same license. Because the archive is linked into the LGPL `libavcodec`, the lock
records the corresponding-source obligation with the exact source archive digest.

## Qualification posture

AV1 decode is a preview/ingest decode declaration of the FFmpeg worker (`av1`, served by
`libdav1d`) for MP4, Matroska and WebM sources. It runs on the CPU in the worker; the decoded planes
enter Bloom's normal color pipeline.
