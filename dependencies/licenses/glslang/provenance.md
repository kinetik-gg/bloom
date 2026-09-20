# glslang Provenance

Reviewed: 2026-09-19

## Source

- Upstream project: <https://github.com/KhronosGroup/glslang>
- Source ref: Khronos tag `vulkan-sdk-1.4.357.0`, commit
  `168d452a4f460d24b588fed08477a81c44ee27a1`
- Archive: `https://codeload.github.com/KhronosGroup/glslang/tar.gz/refs/tags/vulkan-sdk-1.4.357.0`
- SHA-256 of the acquired archive:
  `81038794e20494556edbcc0fc70fa984d71d1b440f9c49adf2cbaaa60a519757`
- Exact archive size: `4412438` bytes

## Version Accuracy

- The `vulkan-sdk-1.4.357.0` tag is a Vulkan SDK **source ref**, not the glslang software version.
  glslang parses its project version from `CHANGES.md` (`parse_version.cmake`); this build generated
  `GLSLANG_VERSION 16.4.0` (`include/glslang/build_info.h`), and `glslang --version` emits
  `Glslang Version: 11:16.4.0`. The lock version is therefore `16.4.0`; the source ref and commit
  above are retained separately so the SDK snapshot is not mislabeled as a release.

## Acquisition Record

- Acquired 2026-09-19 over HTTPS and independently byte-verified by the supervisor; the same bytes
  are reused and the ExternalProject download step reports a cache hash match. No re-download.
- Archive inspection: 3533 entries, **0 symbolic links, 0 absolute paths, 0 `..` paths**.
- GitHub publishes no detached signature for this tag archive (`provenancePolicy: not-published`).

## Status

- NOT QUALIFIED. Acquisition provenance only. No production prefix manifest exists. This is a
  build-only compiler tool; no end-user runtime artifact ships from it.
