# miniaudio 0.11.25 License And Feature Review

Reviewed: 2026-09-15

## License

The upstream license offers public domain or MIT-0. Bloom relies on the MIT-0 alternative and ships
the complete, unmodified upstream license text beside the header and in the application license
catalogue's `dependencies/licenses/miniaudio/` record. MIT-0 is compatible with Bloom's Apache-2.0
distribution and imposes no attribution text beyond retaining the license notice.

The header is unmodified. Bloom uses only the device configuration, callback, start, stop, and
uninitialization APIs needed by `MiniaudioBackend`; miniaudio's unrelated decoding, encoding, and
resource-management APIs do not define Bloom's audio model.

## Build Boundary

The header is included in exactly one Bloom-owned translation unit. That translation unit is an
object-library target with third-party warnings disabled and strict floating-point flags enabled.
Bloom's public playback types contain no `ma_*` types, macros, or pointers. No superbuild recipe,
`FetchContent`, or lock entry is used.
