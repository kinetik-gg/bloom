# Vulkan-Loader vulkan-sdk-1.4.357.0 License And Feature Review

Reviewed: 2026-09-19

## License

- Whole-component SPDX expression: `Apache-2.0 AND MIT AND CC-BY-4.0 AND HPND-Kevlin-Henney AND
  MIT-Khronos-old`. The archive contains all five; every license text under `LICENSES/` plus
  `LICENSE.txt` is bound beside this file, so no upstream alternative or non-code notice is
  dropped.
- Reached compiled graph for the Linux `release` profile: `loader/**` is `Apache-2.0` and
  `loader/cJSON.c` is `MIT`. `loader/dirent_on_windows.*` (`HPND-Kevlin-Henney`) is Windows-only;
  `tests/**` (`MIT-Khronos-old`) is unreachable because `BUILD_TESTS=OFF`; `loader/
  LoaderAndLayerInterface.md` (`CC-BY-4.0`) is documentation and is neither compiled nor shipped.
  The per-file mapping is exactly the upstream `REUSE.toml`.
- Linkage is `shared`: this is the end-user runtime loader binary. `sourceObligation: none`;
  `modified: false` with an empty patch set.

## Feature Minimization

The recipe (`dependencies/superbuild/projects/vulkan-loader.cmake`) builds a compute-only loader
and installs it into the shared prefix:

| CMake option | Value | Contract justification |
| --- | --- | --- |
| `BUILD_TESTS` | `OFF` | The test tree is not part of the shipped or consumed surface. |
| `LOADER_CODEGEN` | `OFF` | Uses the checked-in generated loader sources; no Python code generation. |
| `UPDATE_DEPS` | `OFF` | The loader's own dependency fetcher is never run; no implicit fetching. |
| `BUILD_WSI_XCB_SUPPORT` | `OFF` (Linux/BSD only) | Compute-only probe: avoids the XCB pkg-config dependency and window-system surface. |
| `BUILD_WSI_XLIB_SUPPORT` | `OFF` (Linux/BSD only) | As above, for Xlib. |
| `BUILD_WSI_XLIB_XRANDR_SUPPORT` | `OFF` (Linux/BSD only) | As above, for Xrandr. |
| `BUILD_WSI_WAYLAND_SUPPORT` | `OFF` (Linux/BSD only) | As above, for Wayland. |
| `BUILD_WSI_DIRECTFB_SUPPORT` | `OFF` (Linux/BSD only) | Upstream default; disabled explicitly. |

The WSI flags are passed only under the loader's own Linux/BSD platform branch. On Windows the
loader's `platform_wsi` defines `VK_USE_PLATFORM_WIN32_KHR` unconditionally, and on Apple it defines
the Metal/MacOS portability surfaces; this recipe neither overrides nor pretends to make those
platforms headless. This loader is a **compute-only bootstrap probe** for the current Linux slice,
not a presentation-ready loader for a future swapchain; enabling WSI is deferred to the presentation
work.
