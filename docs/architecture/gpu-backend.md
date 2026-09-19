# GPU Backend Architecture

Status: working

Updated: 2026-09-20

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
- Expose one Bloom-owned immutable capability report. Device initialization establishes only a
  hardware/bootstrap baseline; `Unavailable`, `PreviewOnly`, and `ReferenceParity` are
  per-operation-and-precision outcomes, not a device-wide marketing tier.
- Qualify a closed, versioned operation vocabulary against the CPU fixtures before the scheduler may
  select it. The first `GpuOperationId` values are `SolidV1`,
  `TranslationOpacityBilinearV1`, `SourceOverV1`, `OcioDisplayV1`, and `PackedDisplayV1`.

The Vulkan specification describes explicit graphics and compute control, while MoltenVK implements
a Vulkan subset over public Metal APIs and converts SPIR-V to Metal Shading Language. MoltenVK also
documents known portability limits, so support is capability-driven rather than inferred from an API
version alone. [Vulkan specification](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html),
[MoltenVK documentation](https://github.com/KhronosGroup/MoltenVK/blob/main/README.md)

## Implementation Status

Status vocabulary: `implemented` means present in the repository and locally exercised on Linux;
`pending` means defined but not yet built or qualified. Nothing below makes a Windows or macOS GPU
claim.

Implemented in this slice:

- A Qt-free and Vulkan-free `bloom::render` public surface (`bloom/render/gpu_device.hpp`): typed
  device state, per-operation-and-precision `GpuQualification`, ordered structured diagnostics, an
  immutable generation-scoped capability report, and opaque move-only buffer ownership. No `Vk*`
  type, native handle, or catch-all backend interface is public.
- An optional Vulkan bootstrap behind that surface. CMake resolves `Vulkan::Headers` and
  `GPUOpen::VulkanMemoryAllocator` only from the validated dependency prefix in `qualified` mode;
  when they are absent, or Vulkan is disabled, the same API is provided by a CPU-unavailable stub
  that reports a typed `BackendNotBuilt` diagnostic. No Vulkan header reaches the stub, the desktop
  application, or any public consumer header.
- Device probing that explicitly checks Vulkan 1.2, `timelineSemaphore`, and a compute-capable
  queue family before use; a missing loader, device, or entry point produces a typed Unavailable
  diagnostic instead of a startup crash.
- A privately opened dynamic loader (the platform loader name only, never a workspace path) and a
  Vulkan-Hpp typed RAII owner. The allocator is created with explicit `VmaVulkanFunctions`
  (`vkGetInstanceProcAddr`/`vkGetDeviceProcAddr`); VMA's implementation is isolated in its own
  translation unit and no static Vulkan prototypes are required.
- One bounded host-visible allocation path (64 MiB ceiling) that proves the allocator is live. The
  creating thread is recorded; exposed operations fail closed on another thread, and device
  destruction asserts its owner. An outstanding allocation co-owns that allocator generation, so
  the Vulkan allocator is torn down only after the last buffer is released, on the owner thread.
- The capability report advertises no operation: every operation/precision stays `Unavailable`
  until the frozen fixtures pass, and an unsupported entry is never inherited from another
  operation.

The first GPU operation is now implemented: the fixed **Bloom Neutral v1 display** compute
operation (`bloom/render/gpu_neutral_display.hpp`, operation `OcioDisplayV1`). Its Vulkan compute
shader is an offline artifact from `tools/gpu-shaders`; the checked-in `.comp` is pinned by SHA-256,
the manifest must bind that same digest, the build recompiles it with the locked `glslangValidator`,
validates the SPIR-V with the locked `spirv-val`, and the test hashes the embedded little-endian
word array against the pinned SPIR-V digest, so the source -> SPIR-V -> embedded-array relationship
is closed and no runtime code loads or compiles a shader. The pipeline caches the shader module,
descriptor layout, pipeline layout, compute pipeline, command pool, and fence once per device; one
job is outstanding at a time; `begin` copies and validates the source against device limits and the
byte budget, `poll` is a non-blocking fence query, and `readback` unpacks only after the fence
signals. Cancellation marks a discard without freeing in-flight resources. VMA allocations use the
correct host-access flags for writes and readback, host->compute and compute->host buffer barriers
are recorded, and non-coherent memory is flushed/invalidated with their `VkResult`s checked.
Submission retirement is tracked independently of the API job state: an unknown fence status does not
retire the submission, and owner-thread teardown drains within a bounded budget or quarantines (never
destroys a queue-busy generation) under a process-wide fuse. Wrong-thread `begin`/`poll`/`readback`
fail closed without mutating owned state; `state()`/`diagnostic()` are owner-thread-only reads.
`GpuNeutralDisplay::isBoundTo()` binds a pipeline to exactly its creating device, so no device can be
labelled with another device's results.

Bounded runtime qualification now exists for this one operation
(`bloom/runtime/gpu_neutral_display_qualification.hpp`), with no service, scheduler, frame product,
or UI activation. It runs on the native device/pipeline owner thread, refuses any processor other
than the exact default Bloom Neutral v1 handle: it reconstructs the expected canonical identity with
the official `writeDisplayProcessorIdentityV1` (pinned revision, empty context, `lin_rec709_scene`
source, default `srgb_rec709_display` display/view, `LookMode::Bypass` with empty looks, and the
constant quality/semantics/packing) and compares the resulting canonical bytes to the handle's own
identity bytes, in addition to the processor cache ID, OCIO version, and display/view provenance.
The identity parser validates canonical records, but the exact expected values are enforced by this
byte comparison, so a canonically valid but incompatible source/context/look/packing record is
rejected. Qualification then verifies `isBoundTo`, and executes real native
`begin/poll/readback` against real `produceBloomNeutralDisplayFrame` at the production 65536-pixel
chunk. Parity holds to the frozen contract (RGB within one straight-RGBA8 code, alpha exact) over
1 px, an odd 257-pixel tail, alpha endpoints and all quantization-adjacent samples, and signed/HDR
and tiny-normal values. A nonzero subnormal frame is measured as whole-frame shader rejection and
remains a per-frame CPU fallback, never a parity failure or a claimed supported domain. Timing
measures one warmup plus three alternating pairs at 256x144, 640x360, 1280x720, 1920x1080, and
3840x2160; the eligible interval is the contiguous faster suffix ending at 4K, so an unmeasured or
slower 4K leaves the operation CPU-only. The report carries the exact device identity/generation,
shader digest, config revision, processor cache ID, numeric contract, fixture digest, timing
samples, and eligible interval, and its construction is private to the qualification function. The
outcome is `PreviewOnly`; final output is unchanged and stays on CPU.

Locally verified on Linux: real bootstrap/create/destroy and a tiny VMA host allocation on an
NVIDIA GeForce RTX 5080 (driver 615.71.9.0, API 1.4); the Neutral v1 display dispatch matching the
independent CPU OCIO oracle within the documented one-code RGB tolerance and exact alpha across
1 px, 257 px, special values, the full alpha-quantization boundary sweep, 1280x720, and 1920x1080;
and the bounded qualification reaching `PreviewOnly` with native faster than CPU at all five
measured sizes (about 0.06/0.35/1.27/2.83/11.25 ms native versus 0.66/4.19/17.0/37.8/152.8 ms CPU).
The CPU-unavailable stub, the existing CPU render tests, and a Qt-free CPU application that links no
Vulkan loader also pass. Actual device/driver facts from a run are diagnostic evidence, not
qualification.

The product and the dedicated runtime service now exist on top of that qualification. The product
(`bloom/runtime/gpu_preview_display_product.hpp`) wraps one successful native packed readback into
the existing display-only preview frame with `PreviewDisplayProvider::GpuNeutral` provenance,
retaining the dispatch's exact immutable qualification report and re-checking the one authoritative
`gpuNeutralDisplayStageIsEligible` predicate, so the service's pre-dispatch choice and the finalizer
cannot drift apart; it retains no Float32 process image and introduces no fourth frame variant. The
runtime service (`bloom/runtime/gpu_preview_display_service.hpp`,
`gpu_preview_display_service.cpp`, `gpu_preview_display_service_jobs.cpp`) owns one dedicated service
thread and the native device/pipeline created and used only on it, attaches the scheduler GPU
executor, and tracks device bootstrap, the exact embedded neutral CPU processor build, and
cancellation-aware qualification as a scheduler GPU startup task, so `isQuiescent()` cannot report
true while initialization is running. It returns one stable final `submitGpu` handle per request:
the CPU stage child compiles/evaluates/selects under a distinct coalescing key, and only when the
report's measured eligible interval and the overhead-adjusted comparison
(`native_full_ms + two active-poll handoffs < cpu_full_ms`) both admit it does the service run one
bounded native dispatch; otherwise (non-neutral, obviously tiny, unsupported, oversize, or
post-stage ineligible) the same evaluated stage is mapped by the CPU display fallback and is never
compiled or evaluated twice. Parent cancellation, supersession, and group cancellation cancel
children and mark native discard while retaining completion and admission until children are
terminal and the native submission is retired; a native begin/poll/readback failure, or expiry of
the bounded per-dispatch deadline, drains/quarantines the native pipeline on its owner thread,
disables the GPU, and takes the same-frame CPU fallback. `beginShutdown()` is non-blocking, the
destructor drains before releasing the lease, and requests that race shutdown are rejected rather
than turned into new work. A device-gated service test drives real provenance and pixel parity,
the same-stage CPU fallback, and the bounded-deadline retirement.

The application now routes its preview display submissions through that service. `apps/bloom`
constructs the service after the compiler/evaluator/qualified provider/plan and frame caches and
before the three preview controllers, injects one submitter into the foreground, RAM, and
background controllers, and calls the non-blocking `beginShutdown()` from the existing shutdown
coordinator; the controllers keep their own preparation function for viewer analysis and probes.
When a validated qualified-prefix Vulkan loader is bundled beside the executable the service is
enabled with that app-relative loader; otherwise (macOS, Windows, CPU-stub, developer-system, or
a missing loader) every request takes the explicit CPU path, with no GPU preference or user
setting, and no ambient/host loader fallback. The viewer color chip reports `GPU display` only
for a frame whose provenance is `GpuNeutral`. This milestone is display preview only: the fixed
operation stays `PreviewOnly` (no reference-parity claim), and GPU compositing, a resident GPU
viewer buffer, WSI/swapchain presentation, and a whole-application benchmark remain pending.
Verified locally: the device-free controller-routing test; the real-service application pipeline
through the foreground controller at 1080p with actual `GpuNeutral` provenance, CPU-oracle parity
within one RGB code and exact alpha, the missing-loader CPU fallback with one evaluation per
request, cached re-requests with zero extra preparation, and a warm-cache paired timing (about
4.7 ms service versus 25.5 ms CPU median, no presentation claim); the affected preview, RAM,
background, direct-manipulation, shutdown, and viewer/status tests; the bundled loader hash
matching the prefix file with no Vulkan DT_NEEDED and no ambient loader.

The render layer now also owns the first GPU-resident compositing primitives. Their public headers are
Qt-free and Vulkan-free, with portable CPU-unavailable stubs: `GpuImage` (opaque move-only
device-resident RGBA32F ownership), `GpuSolid` (the SolidV1 compute operation producing a resident
image), `GpuImageUpload` (one already-decoded host `Rgba32fImage` staged into a resident `GpuImage`,
with no media decode on the GPU thread), and `GpuResidentDisplay` (resident RGBA32F copied
device-to-device through the accepted embedded Neutral V1 shader into a resident packed RGBA8 image,
with only the 4-byte status word read back on the normal path). They follow the existing bounded
policy: owner-thread operations, explicit byte budgets checked against actual allocator sizes and the
peak temporary/retained use, cancellation, device-binding checks, and a bounded teardown that
quarantines an unproved submission rather than destroying it in flight. Verified locally on a real
device with the pinned loader: the SolidV1 CPU-oracle parity and the embedded-SPIR-V digest pin, the
resident Neutral display parity (RGB within one code, alpha exact) with subnormal-status rejection
and budget refusal, and bit-exact host-upload readback; the CPU stubs compile clean under the same
strict flags. These are rendering primitives only: no scene evaluator selects them yet, they are not
wired into `GpuPreviewDisplayService`, not composed per layer, and not presented through a viewer, so
no performance claim is made here.

The presentation lane now has a governed Wayland bootstrap and a swapchain lifecycle, still without
any viewer/service wiring. The qualified Linux loader is rebuilt with `BUILD_WSI_WAYLAND_SUPPORT=ON`
alone (XCB/Xlib/Xrandr and DirectFB stay `OFF`; the Wayland branch adds no pkg-config or `DT_NEEDED`
entry), so the loader exports exactly `vkCreateWaylandSurfaceKHR` and
`vkGetPhysicalDeviceWaylandPresentationSupportKHR` over the previous `libm`/`libc` dependencies. The
device bootstrap (`gpu_device.hpp`/`gpu_presentation_types.hpp`) adds an opt-in presentation request
that enables `VK_KHR_surface`/`VK_KHR_wayland_surface` and `VK_KHR_swapchain` only when actually
advertised, records whether the accepted `VK_EXT/KHR_swapchain_maintenance1` present fence or
`VK_KHR_present_wait` retirement mechanism is genuinely enabled, and borrows the instance to a
caller-minted surface through integer handle bits plus a per-device epoch; presentation prefers one
combined graphics+compute queue so resident images stay exclusive, and reports `Unavailable` (CPU
fallback) when the compute family cannot present. The target (`gpu_presentation_target.hpp`) owns the
swapchain, per-image semaphores/fences, and the clear-and-present command, with the clear offered only
for encoded BGRA/RGBA `SRGB_NONLINEAR` formats whose surface advertises the requested usage flags; it
never uses `vkQueueWaitIdle`/`vkDeviceWaitIdle`, treats an unproven or failed present conservatively
(retaining the image until the presentation-engine signal actually proves retirement), propagates
device loss, and guards destruction/move: a foreign-thread or unproven teardown quarantines the whole
native generation under a bounded process fuse (`teardownDrainIncomplete`) instead of destroying it in
flight. Verified locally on the real Wayland session (Qt 6.11.2, `QT_VULKAN_LIB` set to the bundled
loader before the first `QVulkanInstance`): a real surface is acquired, cleared, presented, recreated
at a new positive extent, and closed with an acquired-but-unpresented image; stale-epoch and
wrong-owner calls are rejected; the default compute device and the existing GPU display application
are unchanged. This is bootstrap and lifecycle only: no service/viewer/image-present wiring, no
checkerboard/channel/overlay present, no performance or reference-parity claim, and XCB/Xlib/Xrandr
presentation remains deferred to a reviewed intake.

The render layer now owns the two GPU compositing operations and the runtime owns a bounded
owner-thread content cache, still without any scene evaluator, service, or viewer wiring.
`GpuComposite` (`gpu_composite.hpp`, with a portable CPU-unavailable stub) runs
`TranslationOpacityBilinearV1` and `SourceOverV1`, each writing a new resident RGBA32F `GpuImage`;
both inputs are `shared_ptr<const GpuImage>` retained for the job and never mutated, source-over reads
its destination as a read-only backdrop, and the translation output preserves the source display
window and pixel aspect while `outputWindow` is the data window only. The sample point is prepared on
the host in Float64 (`prepareTranslationAxis`, O(width+height) axis buffers) so the kernel needs no
`shaderFloat64`; dispatch is one invocation per pixel. The two shaders are offline artifacts pinned by
SHA-256 with manifest source binding and a configure-time glslangValidator/spirv-val regeneration
check against the embedded SPIR-V digest, so the source -> SPIR-V -> embedded-array relationship is
closed. Budgets are enforced on the ACTUAL VMA allocation sizes (allocator rounding included) plus the
per-call transient staging peak, with overflow-safe arithmetic; each input's `impl->state` is compared
to this device generation before any driver resource is created or bound, so a real image from a second
device is rejected `InvalidArgument` with no job started. `GpuSceneCache`
(`bloom/runtime/gpu_scene_cache.hpp`) maps an already-computed semantic digest to an already-computed
resident image, charged by `GpuImage::allocationBytes()`, runs only on the device owner thread, rejects
a foreign-device image by ownership identity, and never replaces or evicts an image still pinned by an
external `shared_ptr` (a pinned replacement is refused and the original entry preserved). Two minimal
seams support the cache: `GpuDevice::isOwnerThread()` (native owner compare; stub false) and
`GpuImage::allocationBytes()` (VMA size; stub 0). Verified locally on a real device with the pinned
loader: the Solid -> Translation -> SourceOver resident chain with 4K fractional translations
(3840-wide), nonzero/differing origins, odd extents, opacity and alpha endpoints, HDR/negative RGB,
and 4:3 pixel aspect, each within the documented per-finite-component 2e-6 absolute-or-relative gate
against the retained CPU primitives; immutable-input readback; actual foreign-input rejection followed
by a valid same-device job; the actual-allocation peak refusal; the cache's semantic-digest hits,
pinned-replacement refusal, owner gate, and foreign-identity rejection; the CPU-only kernel test
(SPIR-V pins and CPU-derived fixtures) and the strict stub syntax of the touched APIs. No performance
claim is made.

The covered-Solid primitive and the CPU-side scene preparation foundation are now implemented, still
without an executor, service, or viewer. `GpuSolid::beginCovered` (operation `CoveredSolidV1`, the
CPU fractional-Solid vector-coverage arm) takes the exact immutable R8 coverage the CPU
`PathRaster::coverageRow` produced plus the layer's separate Float32 opacity, builds a 256-entry
premultiplied RGBA32F palette on the host with the EXISTING `coverageSolidRow()` primitive and the
exact separate-opacity multiply, and dispatches a new offline `solid_covered.comp` (pinned by
SHA-256, manifest-bound, regeneration-checked) that only selects a stored palette entry; the resident
readback is bit-exact to the CPU arm with no per-pixel CPU render and no GPU Float64. The ordinary
unmasked `begin()` path is unchanged, and the actual-VMA allocation budget, retained-until-retired
mask/palette buffers, owner-thread, cancellation, and quarantine policy are shared through one
extracted private impl. `bloom::runtime::CpuGpuSceneBuilder` (`prepared_gpu_scene.hpp`) turns a REAL
`CompiledCompositionPlan` plus an `EvaluationRequest` into ordered, immutable
`GpuSceneCommand`s -- solid, unparented translation-only layer, Normal merge, composition output --
using the evaluator's REAL six-argument preflight resolution and, for a fractional translation-only
solid, the SAME CPU `PathRaster` R8 coverage; it allocates no full RGBA CPU image and fails closed
`Unsupported` for every out-of-subset reachable operation. Command semantic keys carry the resolved
operands plus the pinned render shader SPIR-V digests, including the actual `CoveredSolidV1` digest,
and never node/layer IDs, operation indexes, or the revision; execution-order indexes are not
identity. Time activation is the exact CPU `[inPoint, outPoint)` range test, the request pixel
allowance bounds every command output and each unique coverage raster with overflow-safe arithmetic,
cancellation is checked per command and per raster row, and `GpuSceneCoverageCache` is a bounded,
transactional LRU keyed on raster geometry alone. Verified locally with a genuine uncached
`CpuCompositionEvaluator` oracle: identity, bounds, output descriptor, and bit-exact pixels for
fractional/integer translations, proxy and non-square PAR, animated opacity/position, multiple
layers, time-activation range/empty/mute-solo, cache reuse, budget refusal, and the unsupported
branches; plus the covered native coverage matrix, fractional `PathRaster` pattern, rejections,
lifetime, and performance, and the ordinary Solid/composite, CPU image-primitive, and CPU
composition-evaluator regressions. The user-facing application is unwired: no service or scheduler
selects a command, so no performance claim is made.

Pending and unchanged: the GPU scene executor that dispatches these prepared commands and the service
selection that consumes them, per-layer GPU compositing selection, resident GPU viewer buffers,
WSI/swapchain presentation, a whole-application benchmark, the full per-operation qualification
fixtures for a future `ReferenceParity` profile (the qualified display transform remains
`PreviewOnly`; scene operations have no runtime qualification yet), general graph
execution, presentation/swapchain integration, the cross-platform Linux/macOS/Windows parity spike,
shader compilation of generated OCIO programs, and Windows/macOS GPU support. The qualified Linux
prefix manifest remains pending, so this direction stays `working`.

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

The public capability boundary is Qt- and Vulkan-free. Conceptually it contains:

- `GpuQualification`: `Unavailable`, `PreviewOnly`, or `ReferenceParity`, stored on each
  operation-and-precision capability;
- `GpuDeviceState`: `Initializing`, `Ready`, `Lost`, `Recovering`, `Unavailable`, or
  `ShuttingDown`, followed by terminal `Stopped`;
- backend/build/shader/compiler identities and the selected physical-device/driver identity;
- checked limits, memory-budget support, floating-point controls, portability-subset facts, and
  presentation capabilities; and
- one `GpuOperationCapability` per operation ID and precision, with required features, tested
  implementation revision, numeric contract, fixture-set digest, qualification outcome, and ordered
  structured diagnostics.

The report is immutable and generation-scoped. `Ready` means the device passed the bootstrap
requirements needed to probe and submit work; it grants no operation a qualification outcome.
A resource, pipeline, or frame created under one device generation cannot be published or looked up
under another. No `Vk*` type, raw handle, extension flag, or MoltenVK object appears in these Bloom
public values.

## Execution And Threading

One long-lived GPU service owns a logical device and its queue submission state. Command recording
may later use worker threads, but submission ordering and resource retirement remain centralized.
The UI submits a render intent and receives either a completed-frame notification or a structured
failure. It never polls a fence.

The Qt- and Vulkan-free runtime now implements the asynchronous `TaskExecutor::Gpu` scheduler
contract; it does not reuse the synchronous `TaskFunction`, which becomes terminal when its call
returns. The scheduler owns bounded admission and state machines but no GPU worker. One exclusive
generation-scoped lease lets the render-owned service thread pull a starter. Dispatch creates its
move-only `GpuTaskCompletion` token and leaves the task `Running` after the starter returns. A later
fence/timeline completion, device-loss event, or cancellation cleanup consumes the token exactly
once and publishes the terminal result through the ordinary task mailbox. Dropping or overwriting
an unconsumed token is an internal failure and terminalizes the task; it never leaves a permanently
running record.

The token carries `TaskId`, GPU service generation, attachment identity, cancellation state, and a
weak scheduler completion sink, but no Qt or Vulkan handle. Only the GPU service may consume it. The
service owns device and queue submission; ordinary CPU or blocking-I/O workers prepare immutable
inputs but never submit or wait for GPU work. Separate caps bound admitted state machines, queued
command bytes, live tokens, and request-owned bytes before the scheduler retains or queues a
request. Callers must preflight those declared sizes before constructing large request-owned
products; this slice does not expose a reservation API. Queue exhaustion applies back-pressure at
admission.
This continuation path, exactly-once terminalization, strict service-thread completion, independent
admission caps, generation loss/recovery, and scheduler shutdown fallback are implemented with a
fake service. The Vulkan device service, fence integration, resource retirement, and cross-platform
qualification spike remain pending; a synchronous fence wait hidden inside a task function is not a
valid interim implementation.

### CPU Composition Seam

The composition preview's CPU half is now split from the display product applied to it, so a later
GPU display stage can reuse the evaluated frame instead of recomputing the graph:

- `bloom::runtime::PreviewCpuStage` (`src/runtime/include/bloom/runtime/preview_cpu_stage.hpp`) is
  the immutable result of compiling, evaluating, and selecting a CPU display processor for one
  request: the evaluated `ProcessFrame`, the selected
  `color::PreparedCpuDisplayProcessorHandle` (null on the reference/unqualified startup path), the
  request identity, the display byte budget, and the compile/evaluation diagnostics. It introduces
  no new document, plan, or color type.
- `PreviewCpuStageFunction` runs the compile/evaluate/selection half and returns an explicit
  `Evaluated` stage or an explicit `Unsupported` outcome; cancellation and genuine failure stay
  terminal `TaskResult` states, never a fabricated empty frame.
- `PreviewCpuDisplayFallback` applies the reference or qualified display product to the stage's own
  `ProcessFrame` and never compiles or re-evaluates. The stage owns the identity and budget, so the
  fallback reads them from the stage rather than taking duplicate parameters.
- `bloom::ui::makeCompositionPreviewPipeline()` is now a thin composition of the two factories in
  `src/ui/composition_preview_cpu_stage.cpp`; the public `PreviewPreparationFunction` alias and all
  callers are unchanged. The qualified provider's Ready/Pending/Failed behavior, ACES and
  non-default display/view selection, view adjustments, overrides, ROI, progress, and diagnostics
  are preserved.

This seam is CPU-only. No GPU service, device, or pipeline is activated by it, and the renderworker's
`GpuNeutralDisplayPipeline` is not yet consumed here.

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
- Staged shutdown closes GPU admission, requests cancellation on every live completion token, and
  lets the GPU event loop consume device completions or device-loss cleanup until every admitted
  token is terminal. It then destroys active device resources on the GPU thread and only afterward
  exits and joins. A qualified shutdown has a bounded device-loss fallback that terminalizes all
  remaining tokens before service destruction. The application never destroys the device from the
  UI thread after its owner has exited.

No subsystem may call `vkDeviceWaitIdle` or `vkQueueWaitIdle` as part of an interactive frame loop.
Those operations are limited to controlled shutdown or exceptional recovery paths, outside the UI
thread.

## Capability And Qualification Policy

Bloom probes the physical device, queue families, limits, per-format features, memory budget, and
extensions at startup. The result is stored as a runtime capability report, not project state.

The device bootstrap baseline should require:

- Vulkan 1.2, a compute-capable queue, storage buffers, sampled images, and transfer operations;
- timeline semaphore support from the Vulkan 1.2 feature set; and
- the queue, synchronization, allocation, and limit-query behavior needed to run bounded probes.

Format sampling/transfer/storage support, storage-image versus storage-buffer implementations,
required 2D extent or tiling, precision controls, and memory budgets are checked on each operation
capability. `VK_KHR_swapchain` and surface support belong only to presentation capabilities. Failure
of one such check makes that operation/precision/presentation entry unavailable; it does not erase
valid capabilities or make a successfully initialized headless device globally unavailable.

On a portability implementation, Bloom enables `VK_KHR_portability_enumeration`, creates the
instance with `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`, and enables
`VK_KHR_portability_subset` when the selected device advertises it. Every required capability is
still checked directly. Optional extensions may improve throughput but cannot add authoring or
render semantics that have no portable implementation.

MoltenVK support is never inferred from a macOS version, Apple GPU family, advertised Vulkan
version, or successful device creation. The exact portability-subset restrictions and enabled
features are recorded in the report and exercised by the same operation fixtures as native Vulkan.

Capabilities are grouped by operation rather than a marketing tier. A node declares its CPU
implementation, GPU implementation, required formats/features, and parity tolerance. The scheduler
may form CPU and GPU regions, but it includes transfer cost when choosing a path. Repeated CPU/GPU
ping-pong is a planning failure to report and optimize, not an invisible default.

Large images must not be silently clamped. If an image exceeds a device limit or memory budget,
Bloom either uses a tested tiled implementation, evaluates on CPU, or reports an actionable error.

Qualification outcomes have precise meaning:

- `Unavailable` means no safe implementation is admitted. Bloom uses CPU or fails the operation.
- `PreviewOnly` means the operation passed safety and declared preview tests but not the reference
  precision/parity gate. Preview status names the degradation; final output cannot select it.
- `ReferenceParity` means the exact backend, operation, precision, shader and compiler revisions,
  device capability profile, and fixture-set digest passed the frozen CPU comparison gate. Only
  this outcome may become eligible for strict final work in a later accepted policy.

For a requested operation set and requested precision, the scheduler derives an aggregate result as
the least-qualified required operation. This is a request-planning result, not a mutable field on the
device report. A missing operation/precision entry is `Unavailable`; unsupported operations do not
inherit another operation's outcome, and an `RGBA16F` result never qualifies an `RGBA32F` request.
Capability reports are diagnostic inputs and cache identity, not project authoring state.

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
change results. The normative value, precision, color, alpha, and primitive-version contracts are
owned by [`evaluation-primitives.md`](evaluation-primitives.md).

The first reference-parity profile uses `RGBA32F`. Its numeric gates are:

- solid fill, clear, transparent borders, zero/one opacity endpoints, and alpha endpoints are
  bit-exact to the CPU fixture;
- Translation/Opacity Bilinear v1 and Source-Over v1 use per-finite-component absolute and relative
  error limits of `2e-6`, with the comparison accepting a component only when either limit passes;
- no successful finite CPU result may become NaN or infinity; negative and HDR RGB are preserved;
- subnormal and signed-zero cases follow the primitive fixture expectation. A device that flushes or
  rewrites a required case does not earn `ReferenceParity` for that operation;
- qualified packed display RGB differs from the CPU result by at most one 8-bit code value per
  channel, while alpha is exact; and
- any fixture outside these limits is a capability failure. A tolerance is not widened after seeing
  a device result without a reviewed primitive/qualification version change.

`RGBA16F` remains a distinct `PreviewOnly` profile until it has its own explicit precision and image
quality contract. Passing an `RGBA16F` preview gate says nothing about `RGBA32F` reference parity.

Until a later decision admits a qualified GPU operation profile for final output, deterministic
final export uses the CPU reference path. Enabling GPU final rendering is a deliberate policy in
addition to `ReferenceParity`, not an automatic consequence of having a GPU.

GPU process cache identity includes the process request identity, primitive and backend semantics,
backend/shader/SPIR-V/compiler revisions and options, texture/storage precision, operation
capability-profile digest, and device/driver identity whenever any of them may alter pixels. Display
cache identity additionally includes the qualified OCIO display processor identity from
[`color-management.md`](color-management.md). Request generation, cancellation, queue depth, and
budget do not affect pixel identity. Resource-generation identity is separate and prevents a cache
object from surviving device recreation even when its pixels would be equivalent.

## Shaders And OpenColorIO

Bloom-authored kernels use one reviewed Vulkan GLSL source path. Shipped shaders are compiled to
SPIR-V at build time, validated, reflected into explicit bindings, and packaged with a source and
compiler version. Runtime specialization should prefer constants and pipeline variants over
unbounded source generation.

OpenColorIO currently exposes `GPU_LANGUAGE_GLSL_VK_4_6` as a GPU shader target. Bloom uses an OCIO
`GPUProcessor` and `GpuShaderDesc` to obtain shader text, uniforms, and LUT textures. Because those
products are derived from project-selected configuration and external resources, Bloom treats the
generated shader and LUT payloads as bounded untrusted-derived input rather than trusted code.
OCIO config, processor cache ID, shader text and resource digests, compiler revision, and
device-relevant pipeline state participate in the cache key.
[OpenColorIO GPU language API](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/main/include/OpenColorIO/OpenColorTypes.h),
[OpenColorIO shader API](https://opencolorio.readthedocs.io/en/latest/api/shaders.html)

The durable config revision, explicit context, source/output Color Interop IDs, display/view, looks,
and missing-config policy come from [`color-management.md`](color-management.md). The GPU adapter
does not read an ambient config or create a second display identity. CPU and GPU OCIO fixtures use
the same qualified processor intent; processor or resource mismatch is a capability failure, not a
reason to substitute the temporary reference mapper.

Before compilation or upload, the adapter enforces checked limits on shader bytes, tokens, entry
points, uniforms, texture count, LUT dimensions, aggregate LUT bytes, and compile time; validates LUT
shape and finite samples; and rejects unsupported OCIO resource forms. Shader compilation runs off
the UI and GPU service threads in a killable, resource-limited helper process. Bloom accepts its
output only after SPIR-V structural validation and reflection exactly match the declared entry point,
bindings, descriptor types, dimensions, and limits. Pipeline creation remains on the GPU service
thread and is covered by device-loss containment. A timeout, helper crash, validation mismatch,
driver failure, or budget breach produces an operation-scoped diagnostic, publishes no partial
pipeline or LUT, and cannot poison an existing qualified cache entry.

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

## Device Lifecycle, Failure, And Fallback

The device service has one explicit lifecycle:

```text
Initializing -----------------------------> Ready
      |                                       |
      +-----------------> Unavailable         v
                                         Lost -> Recovering -> Ready
                                           |         |
                                           +---------+-----> Unavailable

Initializing/Ready/Lost/Recovering/Unavailable -> ShuttingDown -> Stopped
```

Initialization failure publishes its diagnostic and enters `Unavailable`; CPU execution remains
available. Shutdown may begin from every non-terminal state, closes admission before draining, and
wins over initialization or recovery. Once `ShuttingDown`, the service cannot return to `Ready` or
start another recovery. `Stopped` owns no live device, queue, pipeline, resource, or completion that
could later be published.

Device-loss injection points cover device creation, allocation, shader/pipeline creation, submit,
completion/fence observation, readback, and presentation. On the first loss signal the service:

1. atomically changes the generation state to `Lost` and stops new GPU admission;
2. completes outstanding GPU task results with a `DeviceLost` failure diagnostic, using cancellation
   only for work whose cancellation was independently requested;
3. invalidates all resources, pipeline/cache entries, and prepared frames from that generation;
4. exposes a visible CPU-preview fallback without publishing a stale GPU result as current;
5. attempts at most one controlled asynchronous device recreation during the process lifetime; and
6. resumes with a new generation and freshly measured capability report, or remains `Unavailable`.

Final output continues on CPU only when its immutable request and policy allow the CPU equivalent;
otherwise it fails. It never resumes partway through an output with mixed hidden semantics.

- No compatible GPU: start normally with the CPU renderer and show one persistent, actionable
  performance diagnostic.
- Unsupported node or format: schedule a CPU region when parity and transfer rules allow it; never
  substitute a different visual operation.
- Out of device memory: evict rebuildable GPU caches, retry within a bounded policy, then fall back
  or fail the request explicitly.
- Device loss: follow the bounded state machine above; repeated loss does not enter an unbounded
  recreate loop.
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

1. Run the exact closed operation vocabulary offscreen with `RGBA32F` and apply the numeric gates in
   this document. Fixtures include an empty stack; one and two solids with reorder; integer and
   fractional translation; opacity and all alpha endpoints; negative/HDR, signed-zero, and subnormal
   values; odd, proxy, 4K, and 8K extents; and matrix, 1D-LUT, and 3D-LUT OCIO display processors.
2. Run the same SPIR-V assets and backend code on native Linux/Windows Vulkan and Apple Silicon
   MoltenVK. Record the immutable capability report, exact fixture digest, comparison summary, and
   every failure. No platform-specific tolerance adjustment is allowed.
3. Present qualified output in the Qt Widgets Viewer, exercise resize, panel replacement,
   detach/reattach, rapid request supersession, cancellation barriers, cache pressure, and resource
   generation invalidation. The normal path performs no full-frame CPU readback.
4. Trace cold OCIO shader compile, pipeline creation, frame evaluation, queue/fence delay, and image
   upload while UI thread sentinels run. Predeclare latency and memory budgets for the test profile;
   report measurements rather than substituting subjective visual review.
5. Run Vulkan validation cleanly; capture GPU timings and a platform-native frame trace; serialize
   and reject incompatible pipeline caches. Inject allocation, shader, pipeline, submit, completion,
   readback, presentation, and device-loss failures and verify the lifecycle contract.
6. Stress bounded queue admission, supersession, staged shutdown, repeated loss, and CPU fallback.
   The Viewer stays responsive, last-good frames remain marked stale, and no cancelled or
   old-generation frame becomes current.
7. Qualify and lock the Vulkan loader/headers, Vulkan-Hpp, VMA, shader compiler, MoltenVK, and every
   bundled transitive component through [`dependency-intake.md`](dependency-intake.md), including
   offline rebuild, SBOM, shipped-file verification, and notices.

Before production acceptance, expand parity CI or lab coverage beyond one GPU per operating system
to include at least AMD, Intel, NVIDIA, and Apple GPU families that Bloom claims to support. Results
are machine-readable capability evidence; screenshots or “looks correct” approval do not replace
the numeric fixtures.

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
- Qt: community packages dynamically link the pinned LGPLv3 Qt distribution under accepted
  [ADR 0014](../decisions/0014-apache-license-and-qt-distribution.md). Depending on a private Qt
  target does not provide an ABI promise and increases upgrade/build coupling.

[Vulkan-Hpp project](https://github.com/KhronosGroup/Vulkan-Hpp),
[Vulkan Memory Allocator project](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
[MoltenVK licensing](https://github.com/KhronosGroup/MoltenVK#licensing)
