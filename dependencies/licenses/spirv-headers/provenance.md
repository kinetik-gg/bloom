# SPIRV-Headers Provenance

Reviewed: 2026-09-19

## Source

- Upstream project: <https://github.com/KhronosGroup/SPIRV-Headers>
- Source ref: exact commit `29981f65241605e08b0ede4cfeb999fe3b723c6a`, the revision glslang's
  `known_good.json` names for the `vulkan-sdk-1.4.357.0` source ref
- Archive: `https://codeload.github.com/KhronosGroup/SPIRV-Headers/tar.gz/29981f65241605e08b0ede4cfeb999fe3b723c6a`
- SHA-256 of the acquired archive:
  `232899f1ad4104fb5bc377b94596c7621575eee62ad9a9e8f929b63a7dd8a7ad`
- Exact archive size: `572275` bytes

## Acquisition Record

- Acquired 2026-09-19 over HTTPS and independently byte-verified by the supervisor; the same bytes
  are reused and the ExternalProject download step reports a cache hash match. No re-download.
- Archive inspection: 141 entries, **0 symbolic links, 0 absolute paths, 0 `..` paths**.
- GitHub publishes no detached signature for this commit archive (`provenancePolicy: not-published`).
- The lock version field is the exact commit, because this pin is a glslang known_good commit rather
  than an independently verified SPIRV-Headers project release tag; the upstream CMake project
  declares its own version separately and that is not claimed here.

## Status

- NOT QUALIFIED. Acquisition provenance only. No production prefix manifest exists. This is a
  build-only header source; it installs nothing.
