# minimp3 (2026-07-27 snapshot) License And Feature Review

Reviewed: 2026-09-15

## License

The upstream project dedicates the code to the public domain under CC0 1.0 and ships the complete
license text in this repository. The CC0 grant is compatible with Bloom's Apache-2.0 distribution;
the license file is retained for auditability and attribution cataloguing.

The core and extended headers are unmodified. Bloom enables `MINIMP3_FLOAT_OUTPUT` and uses the
extended in-memory open/read path for bounded MP3 probe and decode. File mapping, callback I/O,
and unrelated convenience paths are not part of the Bloom adapter.

## Build Boundary

Both headers are included in exactly one Bloom-owned translation unit. That translation unit is an
object-library target with third-party warnings disabled and strict floating-point flags enabled.
Bloom's public audio types contain no `minimp3` types, macros, or pointers. No superbuild recipe,
`FetchContent`, or lock entry is used.
