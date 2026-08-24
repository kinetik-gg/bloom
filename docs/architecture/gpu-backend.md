# GPU Backend Architecture

Status: working

Updated: 2026-08-25

## Purpose

Bloom should use the GPU for interactive compositing, display processing, and other parallel work
without making project semantics depend on a vendor, operating system, or UI toolkit. The CPU
reference evaluator remains the correctness oracle. GPU execution is an acceleration strategy over
the same immutable evaluation request.

The initial working direction is one Vulkan renderer on Linux and Windows and the same Vulkan code
through MoltenVK on macOS. This is not accepted until the cross-platform spike in this document
passes.

## Decision Summary

- Target Vulkan 1.2 as the provisional baseline API.
- Use native Vulkan drivers on Linux and Windows and bundle a pinned MoltenVK build on macOS.
- Use Vulkan-Hpp for typed C++ bindings and RAII handles. Wrap Vulkan Memory Allocator behind
  Bloom-owned resource types rather than exposing it through public render interfaces.
- Author Bloom GPU kernels as Vulkan GLSL and compile them to SPIR-V. Compile shipped kernels
  offline; compile generated OCIO shader programs off the UI thread and cache the resulting modules
  and pipelines.
- Keep a Bloom-owned backend interface between the evaluator and Vulkan. It represents Bloom
  operations and resources, not a second generic graphics API.
- Keep presentation separate from offscreen evaluation. Qt may create windows and surfaces, but Qt
  types do not enter `src/render` or `src/runtime`.
- Never wait for GPU work, compile a shader or pipeline, upload a full frame, or destroy a busy GPU
  resource on the UI thread.

The Vulkan specification describes explicit graphics and compute control, while MoltenVK implements
a Vulkan subset over public Metal APIs and converts SPIR-V to Metal Shading Language. MoltenVK also
documents known portability limits, so support is capability-driven rather than inferred from an API
version alone. [Vulkan specification](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html),
[MoltenVK documentation](https://github.com/KhronosGroup/MoltenVK/blob/main/README.md)

## Boundaries

```text
Qt UI thread
  - input, panels, surface lifecycle, completed-frame notification
                 |
                 | immutable evaluation request / cancellation token
                 v
Runtime scheduler and CPU worker pool
  - graph planning, decode, CPU reference nodes, staging preparation
                 |
                 | GPU work packet; no Qt types
                 v
GPU service thread
  - device and queues, submissions, fences, caches, resource retirement
                 |
          +------+------+
          |             |
          v             v
  offscreen images   presentation bridge
  and buffers        and per-viewer swapchains
```

`src/render` owns logical GPU resources, command generation, synchronization, and backend errors.
`src/runtime` decides which validated implementation evaluates a node. `src/ui` owns any `QWindow`,
`QVulkanInstance`, or `QWidget` integration. The document model does not know whether a result came
from CPU or GPU execution.

The backend interface should expose operations such as device discovery, capability reports,
resource creation, asynchronous work submission, completion, cancellation state, and diagnostics.
It must not mirror every Vulkan object or flag. Backend-native handles remain internal except through
a narrow, non-owning presentation interop contract.

## Execution And Threading

One long-lived GPU service owns a logical device and its queue submission state. Command recording
may later use worker threads, but submission ordering and resource retirement remain centralized.
The UI submits a render intent and receives either a completed-frame notification or a structured
failure. It never polls a fence.

- Requests carry snapshot identity, time, output, resolution, quality, color intent, and a
  cancellation generation.
- A newer interactive request supersedes older preview work. Submitted GPU commands may complete,
  but their results can be discarded without becoming current UI state.
- Shader compilation, pipeline creation, media decode, CPU/GPU transfers, and cache eviction run on
  workers or the GPU service thread.
- Resource destruction is deferred until the last submission that references the resource has
  completed.
- Final renders use bounded queues and back-pressure rather than allocating one frame's resources
  per requested output.
- GPU timing, queue delay, compilation time, upload/download volume, cache hits, and memory pressure
  are observable in diagnostics.

No subsystem may call `vkDeviceWaitIdle` or `vkQueueWaitIdle` as part of an interactive frame loop.
Those operations are limited to controlled shutdown or exceptional recovery paths, outside the UI
thread.

## Capability Policy

Bloom probes the physical device, queue families, limits, per-format features, memory budget, and
extensions at startup. The result is stored as a runtime capability report, not project state.

The initial baseline profile should require:

- Vulkan 1.2, a compute-capable queue, storage buffers, sampled images, and transfer operations;
- timeline semaphore support from the Vulkan 1.2 feature set;
- sampled and transfer support for the selected floating-point working textures;
- storage-image support for a candidate `R16G16B16A16_SFLOAT` path, or a validated storage-buffer
  implementation of the same operation;
- `VK_KHR_swapchain` only for interactive presentation; and
- enough 2D image extent for the active request, otherwise validated tiling or CPU evaluation.

On a portability implementation, Bloom enables `VK_KHR_portability_enumeration`, creates the
instance with `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`, and enables
`VK_KHR_portability_subset` when the selected device advertises it. Every required capability is
still checked directly. Optional extensions may improve throughput but cannot add authoring or
render semantics that have no portable implementation.

Capabilities are grouped by operation rather than a marketing tier. A node declares its CPU
implementation, GPU implementation, required formats/features, and parity tolerance. The scheduler
may form CPU and GPU regions, but it includes transfer cost when choosing a path. Repeated CPU/GPU
ping-pong is a planning failure to report and optimize, not an invisible default.

Large images must not be silently clamped. If an image exceeds a device limit or memory budget,
Bloom either uses a tested tiled implementation, evaluates on CPU, or reports an actionable error.

## Image Semantics And Correctness

The GPU backend does not choose Bloom's canonical color or alpha model. Evaluation requests provide
explicit color intent, and image descriptors carry channel type, dimensions, alpha semantics, and
color-space metadata independently of Vulkan formats.

`RGBA16F` is the initial candidate for interactive GPU intermediates because it is useful for HDR
compositing and bandwidth-sensitive previews. `RGBA32F`, packed display surfaces, deep data, and
other representations are capability- and operation-specific. Choosing a GPU format never reduces
the precision requested for strict final output without an explicit policy and visible diagnostic.

Every initial accelerated operation also has a CPU reference implementation. GPU parity is tested
with documented absolute, relative, and image-level tolerances appropriate to the operation; it is
not assumed to be bit-identical across vendors. Cache keys include the implementation revision,
shader revision, quality mode, relevant capabilities, and device/driver identity when those can
change results.

Until the parity suite is accepted, deterministic final export uses the CPU reference path by
default. Enabling GPU final rendering is a deliberate capability, not an automatic consequence of
having a GPU.

## Shaders And OpenColorIO

Bloom-authored kernels use one reviewed Vulkan GLSL source path. Shipped shaders are compiled to
SPIR-V at build time, validated, reflected into explicit bindings, and packaged with a source and
compiler version. Runtime specialization should prefer constants and pipeline variants over
unbounded source generation.

OpenColorIO currently exposes `GPU_LANGUAGE_GLSL_VK_4_6` as a GPU shader target. Bloom uses an OCIO
`GPUProcessor` and `GpuShaderDesc` to obtain shader text, uniforms, and LUT textures, then compiles
that trusted generated GLSL to SPIR-V on a worker. OCIO config, processor cache ID, shader text,
compiler revision, and device-relevant pipeline state participate in the cache key.
[OpenColorIO GPU language API](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/main/include/OpenColorIO/OpenColorTypes.h),
[OpenColorIO shader API](https://opencolorio.readthedocs.io/en/latest/api/shaders.html)

Glslang is the provisional GLSL-to-SPIR-V compiler because it is Khronos's reference front end and
can be used as a command-line tool or library. Its exact build and transitive licenses must be pinned
and audited before distribution. [Glslang project](https://github.com/KhronosGroup/glslang)

Arbitrary project-provided shader source is outside the initial scope. If scripting or shader nodes
are added later, validation, resource limits, and process isolation require a separate security
decision.

## ASWF Media Interop

OpenColorIO integrates by generating shader code and LUT data; it does not own Bloom's Vulkan
device. OpenImageIO and OpenEXR initially decode and encode through CPU memory followed by
asynchronous staging transfers. OpenEXRCore's thread-safe, non-blocking low-level API and support for
custom unpacking may later reduce copies, but direct GPU unpacking is a measured optimization rather
than an architectural prerequisite. [OpenEXR API](https://openexr.com/en/latest/API.html)

External GPU resource sharing is added only for a concrete integration and must specify ownership,
device identity, image layout/state, synchronization, lifetime, and fallback behavior. A raw native
handle alone is not an interop contract.

## Qt Presentation

Offscreen evaluation does not depend on Qt or a swapchain. A replaceable UI-side presentation
adapter consumes completed frame handles and presents them in a viewer.

The primary spike uses a custom Vulkan-capable `QWindow` embedded in the panel shell. Public Qt APIs
can associate a `QVulkanInstance` with a window, obtain a cross-platform `VkSurfaceKHR`, and query
presentation support without Bloom writing Win32, X11/Wayland, or Cocoa surface code. An advanced
renderer may manage its own device and swapchain instead of using `QVulkanWindow`.
[QVulkanInstance](https://doc.qt.io/qt-6/qvulkaninstance.html),
[Qt Vulkan integration](https://doc.qt.io/qt-6/qtgui-overview.html#vulkan-integration)

The target is a same-device GPU copy or sample into each viewer swapchain. CPU readback remains a
functional fallback and test path, not the normal interactive path. Multiple viewer panels share the
render device where surface support permits and own independent swapchain state.

`QRhiWidget` is useful for a comparison spike because it embeds accelerated rendering naturally in
Qt Widgets and supports Vulkan, Metal, and Direct3D. It is not the core GPU backend. Accessing the
underlying QRhi requires `Qt::GuiPrivate`; Qt labels the QRhi family semi-public and provides no
source or binary compatibility guarantee across Qt minor versions. QRhi resources also cannot be
shared between QRhi instances. If Bloom later uses QRhi in a presentation adapter, that adapter must
be isolated, tested against an exactly pinned Qt minor version, and replaceable without changing
render semantics. [QRhi compatibility and threading](https://doc.qt.io/qt-6/qrhi.html),
[QRhiWidget](https://doc.qt.io/qt-6/qrhiwidget.html)

## Failure And Fallback Policy

- No compatible GPU: start normally with the CPU renderer and show one persistent, actionable
  performance diagnostic.
- Unsupported node or format: schedule a CPU region when parity and transfer rules allow it; never
  substitute a different visual operation.
- Out of device memory: evict rebuildable GPU caches, retry within a bounded policy, then fall back
  or fail the request explicitly.
- Device loss: cancel outstanding GPU requests, invalidate all device resources, attempt one
  controlled device recreation off the UI thread, then continue on CPU if recreation fails.
- Shader or pipeline failure: retain the compiler and driver diagnostic, identify the affected node,
  and use its CPU implementation when available.
- Preview fallback or reduced precision: expose it in viewer status. Strict final output does not
  degrade silently.

## Alternatives Evaluated

| Option | Strengths | Why it is not the working choice |
| --- | --- | --- |
| Direct Vulkan + MoltenVK | One explicit compute/render model, SPIR-V toolchain, strong diagnostics, direct control of queues, memory, and synchronization | High implementation cost; macOS is a portability subset and must pass real parity, performance, and presentation tests |
| Qt QRhi / QRhiWidget | Natural Qt Widgets integration; runtime Vulkan, Metal, D3D11/12, and OpenGL backends | Core QRhi requires `Qt::GuiPrivate` and has no cross-minor source or binary compatibility guarantee; its resource/threading model would couple the headless renderer to Qt |
| Dawn / WebGPU | Permissive native implementation over D3D12, Metal, and Vulkan; standardized capability and validation model | WGSL and WebGPU limits add another shader contract; native shared-resource facilities are implementation extensions and are not uniformly implemented, making pro-app interop and zero-copy presentation a risk today |
| Diligent Engine | Consistent C/C++ API, HLSL-oriented shader path, explicit compute and modern API backends | The open distribution's native Metal backend is commercially licensed; its free macOS route still uses a Vulkan portability layer, so it adds an abstraction without removing the main Apple risk |
| Native D3D12 + Metal + Vulkan | Maximum native control, tooling, and interop on each OS | Three production backends and shader paths multiply implementation and parity work before Bloom proves its first compositor |

Dawn is BSD-3-Clause licensed. Diligent Engine is Apache-2.0 overall, but its official support table
marks the native Metal backend as commercially licensed and notes independent third-party licenses.
These facts do not replace a distribution-time dependency audit.
[Dawn overview and backends](https://dawn.googlesource.com/dawn),
[Dawn shared texture memory status](https://dawn.googlesource.com/dawn/+/HEAD/docs/dawn/features/shared_texture_memory.md),
[Dawn license](https://dawn.googlesource.com/dawn/+/HEAD/LICENSE),
[Diligent Engine support and licensing notes](https://github.com/DiligentGraphics/DiligentEngine)

## Cross-Platform Spike Gate

This direction becomes accepted only after the same bounded prototype passes on Linux, Windows, and
Apple Silicon macOS:

1. Select a source image, run transform/composite plus an OCIO display transform, and present it in
   a Qt Widgets viewer using the same SPIR-V shader assets and render code.
2. Compare fixed 8-bit, half-float, and float fixtures against CPU reference output, including
   out-of-range and alpha cases, with recorded per-operation tolerances and no unexplained platform
   divergence.
3. Exercise 4K and 8K frames, resize, panel replacement, viewer detach/reattach, rapid scrubbing,
   cancellation, and cache pressure without full-frame CPU readback in the normal presentation path.
4. Trace a cold OCIO shader compile, pipeline creation, frame evaluation, GPU fence delay, and an EXR
   decode/upload while a UI responsiveness sentinel runs. No heavy operation or GPU wait may execute
   on the Qt UI thread.
5. Run Vulkan validation cleanly in debug builds; capture GPU timings and a platform-native frame
   trace; serialize and safely invalidate pipeline caches.
6. Simulate shader failure, out-of-memory handling, and device loss. The viewer must remain
   responsive, stale frames must not become current, and CPU fallback must be visible.
7. Produce dependency manifests for the Vulkan loader/headers, Vulkan-Hpp, VMA, glslang toolchain,
   MoltenVK, and their bundled transitive components, including required notices.

Before production acceptance, expand parity CI or lab coverage beyond one GPU per operating system
to include at least AMD, Intel, NVIDIA, and Apple GPU families that Bloom claims to support.

If the prototype fails because of MoltenVK correctness, sustained performance, or presentation
interop, do not hide the gap behind reduced quality. Time-box an A/B spike of Dawn's native Metal
backend and a small native Metal backend behind the same Bloom interface, then replace or narrow this
working decision with evidence. If only the Qt embedding path fails, retain the offscreen Vulkan
result and compare the custom `QWindow` and isolated QRhiWidget presentation adapters.

## Dependency Licenses To Track

- Vulkan-Hpp and MoltenVK: Apache-2.0.
- Vulkan Memory Allocator: MIT.
- Glslang: multiple permissive components, led by BSD-3-Clause for glslang proper; audit the exact
  compiled target and optional dependencies.
- Qt: follow the repository's eventual Qt distribution and application license decision. Depending
  on a private Qt target does not provide an ABI promise and increases upgrade/build coupling.

[Vulkan-Hpp project](https://github.com/KhronosGroup/Vulkan-Hpp),
[Vulkan Memory Allocator project](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
[MoltenVK licensing](https://github.com/KhronosGroup/MoltenVK#licensing)
