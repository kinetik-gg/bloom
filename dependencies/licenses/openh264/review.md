# OpenH264 2.6.0 intake review

## Role and boundary

OpenH264 is Bloom's bundled H.264 software encoder. The superbuild compiles the unmodified
BSD-2-Clause source release from its pinned archive on Linux, macOS and Windows (Meson, the
adjacent `openh264.configure-arguments` vector) into a static, position-independent archive.
FFmpeg's LGPL `libopenh264` wrapper links that archive into the worker's shared `libavcodec`; the
FFmpeg version script keeps every OpenH264 symbol private to `libavcodec`. Only the encoder is
enabled (`--disable-decoder=libopenh264`); H.264 decode stays FFmpeg's native decoder.

Nothing is downloaded at run time. There is no Cisco binary, no consent flow, no per-user install
directory and no loader shim: the encoder ships inside Bloom's worker on every platform, and the
artist installs nothing beyond Bloom.

## License

The source is BSD-2-Clause (`LICENSE`, byte-identical to the archive's copy). Binary
redistribution reproduces that notice through `THIRD_PARTY_NOTICES.md` and the license texts
shipped with the worker. Because the archive is linked into the LGPL `libavcodec`, the lock
records the corresponding-source obligation with the exact source archive digest so the shipped
`libavcodec` can be rebuilt and relinked from locked sources.

## Patent posture

The owner's decision (2026-10-31) accepts bundling a source-built H.264 encoder; codec patent
questions outside ProRes are covered by Bloom's existing deployment warning. The BSD license grants
no patent rights, and no patent license is claimed. Cisco's separately distributed binary terms do
not apply because Bloom does not use Cisco's binary.

## Qualification posture

Software H.264 is the FFmpeg worker's `h264` encode declaration, served by `libopenh264`, and is
the silent fallback when VA-API hardware encode is unavailable or refused. It is a development
qualification exercised by the encode/reopen round trip; it is neither Apple authorization nor an
archival claim.
