# ADR 0006: Gated Vulkan GPU Backend

Status: working

Date: 2026-08-25

## Context

Bloom needs GPU acceleration for responsive motion graphics and VFX work on Linux, macOS, and
Windows. GPU work must remain separate from the Qt UI thread, preserve CPU-defined correctness, and
interoperate with professional color and image libraries without making project state depend on a
graphics API.

The evaluated choices were direct Vulkan with MoltenVK, Qt QRhi, Dawn/WebGPU, Diligent Engine, and
separate native Vulkan, Metal, and D3D12 backends.

Qt documents QRhi as a semi-public API with no source or binary compatibility guarantee across Qt
minor versions. Dawn provides native D3D12, Metal, and Vulkan implementations but introduces WebGPU
capability and WGSL contracts, while its native shared-resource extensions are not uniformly
implemented. Diligent's open path to macOS still uses Vulkan portability because its native Metal
backend is commercially licensed. Three native backends would maximize platform control but multiply
the implementation and parity burden before Bloom proves its first compositor.

## Decision

Adopt this gated working direction:

- Implement one Vulkan 1.2 render backend using Vulkan-Hpp and Bloom-owned resource abstractions.
- Use native Vulkan on Linux and Windows and a pinned MoltenVK distribution on macOS.
- Use Vulkan GLSL compiled to SPIR-V for Bloom kernels. Use OpenColorIO's Vulkan GLSL GPU output for
  display/color transforms and compile generated programs off the UI thread.
- Keep the CPU evaluator as the initial correctness reference and deterministic-final default.
- Run GPU submission, waits, compilation, transfers, cache maintenance, and device recovery outside
  the Qt UI thread.
- Keep offscreen evaluation independent of Qt. Isolate Vulkan/Qt surface and swapchain integration in
  a replaceable presentation adapter.
- Treat every GPU feature as capability-probed. Unsupported operations use a validated CPU path or
  fail explicitly; they never degrade render semantics silently.
- Do not use QRhi as Bloom's canonical renderer. It may be tested as an isolated presentation
  adapter only with an exactly pinned Qt minor version.

This decision becomes `accepted` only when the cross-platform spike and failure tests in
[`../architecture/gpu-backend.md`](../architecture/gpu-backend.md) pass on all three target operating
systems.

## Consequences

- Most renderer and shader code is shared across the three operating systems.
- Bloom retains explicit control over compute, memory, synchronization, diagnostics, and headless
  execution.
- OpenColorIO's Vulkan GLSL output integrates without maintaining three color-shader sources.
- Vulkan complexity, synchronization safety, memory budgeting, and pipeline caching become Bloom's
  responsibility.
- macOS correctness and performance depend on MoltenVK's documented portability subset and SPIR-V
  translation; advertised Vulkan versions alone are insufficient evidence.
- Qt presentation needs careful same-device synchronization, surface lifecycle handling, and a CPU
  fallback path.
- GPU output may differ numerically across hardware, so operation-specific parity tolerances and
  cache identities are required before GPU final rendering is enabled.
- Bloom must ship and audit notices for MoltenVK, Vulkan-Hpp, VMA, the shader compiler, and all
  selected transitive dependencies.

## Rejection And Revisit Triggers

Reject or narrow this direction if the spike shows any of the following:

- unexplained CPU/GPU or cross-platform image divergence;
- MoltenVK cannot support required compositing kernels or sustained interactive workloads without
  a material quality or latency penalty;
- a same-device Qt viewer cannot present without routine full-frame CPU readback;
- shader or pipeline compilation cannot be kept out of interactive latency paths; or
- device-loss and memory-pressure recovery cannot preserve UI responsiveness.

If MoltenVK is the blocker, compare Dawn's native Metal backend and a narrow native Metal backend
behind the same Bloom interface. If Qt embedding alone is the blocker, compare a custom Vulkan
`QWindow` with an isolated QRhiWidget adapter without changing the offscreen renderer.

## References

- [GPU backend architecture and acceptance gate](../architecture/gpu-backend.md)
- [Vulkan specification](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html)
- [MoltenVK README and portability notes](https://github.com/KhronosGroup/MoltenVK/blob/main/README.md)
- [MoltenVK runtime guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md)
- [Qt QRhi compatibility contract](https://doc.qt.io/qt-6/qrhi.html)
- [Dawn native overview](https://dawn.googlesource.com/dawn/+/HEAD/docs/dawn/overview.md)
- [Dawn experimental shared-texture interop](https://dawn.googlesource.com/dawn/+/HEAD/docs/dawn/features/shared_texture_memory.md)
- [Diligent Engine backend and license table](https://github.com/DiligentGraphics/DiligentEngine)
- [OpenColorIO GPU languages](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/main/include/OpenColorIO/OpenColorTypes.h)
