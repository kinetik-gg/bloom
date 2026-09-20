# VulkanMemoryAllocator v3.4.0 License And Feature Review

Reviewed: 2026-09-19

## License

- SPDX expression: `MIT`. Permissive; compatible with Bloom's Apache-2.0 distribution under
  ADR 0014.
- Attribution is satisfied by shipping the bound `LICENSE.txt` bytes with distributed builds.
  `copyrightFiles`/`noticeFiles` are empty by review: the copyright line lives inside the license
  text itself. `sourceObligation: none`; `modified: false` with an empty patch set.

## Recorded Linkage

- Recorded as `linkage: header-only` under lock schema 1.3. This is accurate: the component installs
  the single header `include/vk_mem_alloc.h` and a CMake package config, and builds neither a static
  nor a shared library. The `shippingRoles` are `cmake-package`, `data`, and `license`, matching
  those actual installed artifacts. Schema 1.3's `header-only` value is the only honest
  representation; no static stand-in is used.

## Feature Minimization

The recipe
(`dependencies/superbuild/projects/vulkan-memory-allocator.cmake`) installs the header and the
CMake package config only:

| CMake option | Value | Contract justification |
| --- | --- | --- |
| `VMA_ENABLE_INSTALL` | `ON` | Required so `VulkanMemoryAllocatorConfig.cmake`, the exported `GPUOpen::VulkanMemoryAllocator` target, and `include/vk_mem_alloc.h` land in the shared prefix. |
| `VMA_BUILD_DOCUMENTATION` | `OFF` | The Doxygen HTML documentation is a build-time tool dependency and is not part of the shipped surface. |
| `VMA_BUILD_SAMPLES` | `OFF` | The sample application is not part of the shipped surface. |

The header is not compiled by any Bloom target in this slice; no `VMA_IMPLEMENTATION` translation
unit exists yet.
