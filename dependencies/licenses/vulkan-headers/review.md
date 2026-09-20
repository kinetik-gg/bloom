# Vulkan-Headers vulkan-sdk-1.4.357.0 License And Feature Review

Reviewed: 2026-09-19

## License

- SPDX expression: `Apache-2.0 OR MIT`. Permissive; compatible with Bloom's Apache-2.0
  distribution under ADR 0014.
- Reached artifact: headers and registry/data plus CMake package config. No executable code is
  compiled or linked from this component. Attribution is satisfied by shipping the bound
  `LICENSE.md`, `Apache-2.0.txt`, and `MIT.txt` bytes with distributed builds.
  `copyrightFiles`/`noticeFiles` are empty by review: the copyright and license obligations live
  inside the shipped license texts. `sourceObligation: none`; `modified: false` with an empty patch
  set.

## Recorded Linkage

- Recorded as `linkage: header-only` under lock schema 1.3. This is accurate: the component installs
  headers, registry data, and a CMake package config, and builds no static or shared library. The
  `shippingRoles` are `cmake-package`, `data`, and `license`, matching those actual installed
  artifacts. Schema 1.3's `header-only` value is the only honest representation; no static stand-in
  is used.

## Feature Minimization

The recipe (`dependencies/superbuild/projects/vulkan-headers.cmake`) installs headers, registry data,
and the CMake package config only:

| CMake option | Value | Contract justification |
| --- | --- | --- |
| `VULKAN_HEADERS_ENABLE_INSTALL` | `ON` | Required so `VulkanHeadersConfig.cmake` and `include/vulkan` land in the shared prefix for later `find_package(VulkanHeaders CONFIG)`. |
| `VULKAN_HEADERS_ENABLE_TESTS` | `OFF` (upstream default is derived from `PROJECT_IS_TOP_LEVEL`) | The ExternalProject configures this tree as top level, which would otherwise enable the test suite. Tests are not part of the shipped surface. |
| `VULKAN_HEADERS_ENABLE_MODULE` | `OFF` | The Vulkan-Hpp C++ named module is a build-toolchain-specific surface; the classic headers are what Bloom consumes. |

No in-recipe download or `FetchContent` is used; the archive contains no submodules.
