# SPIRV-Tools Provenance

Reviewed: 2026-09-19

## Source

- Upstream project: <https://github.com/KhronosGroup/SPIRV-Tools>
- Source ref: exact commit `b707790a898e44038547df54580022fc1cf89c3d`, the revision glslang's
  `known_good.json` names for the `vulkan-sdk-1.4.357.0` source ref
- Archive: `https://codeload.github.com/KhronosGroup/SPIRV-Tools/tar.gz/b707790a898e44038547df54580022fc1cf89c3d`
- SHA-256 of the acquired archive:
  `05d8af89737bde57571c48dbd36714c9f520a69623e14de72c3be6b600e277d6`
- Exact archive size: `3521745` bytes

## Acquisition Record

- Acquired 2026-09-19 over HTTPS and independently byte-verified by the supervisor; the same bytes
  are reused and the ExternalProject download step reports a cache hash match. No re-download.
- Archive inspection: 1841 entries, **0 symbolic links, 0 absolute paths, 0 `..` paths**.
- GitHub publishes no detached signature for this commit archive (`provenancePolicy: not-published`).
- The lock version field is the exact commit, because this pin is a glslang known_good commit rather
  than an independently verified project release tag. The built `spirv-val` reports an internal
  version string (`SPIRV-Tools v2026.3`, emitted commit constant `01bed166...`) that is generated
  into the source tree and does not by itself prove the source commit; the pinned commit above is
  the authoritative identity and is recorded separately.

## Status

- NOT QUALIFIED. Acquisition provenance only. No production prefix manifest exists. The validator
  now ships as the runtime GPU shader-validation tool staged under `bloom-gpu-tools/`; the lock
  records its shipping roles and every staged binary is digest-bound to this acquisition by the
  generated inventory.
- Verify with `spirv-val --version` against this record when the tool is used.
