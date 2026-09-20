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
claim. This section is the current ownership/behavior/gate summary.

Capability surface and device bootstrap. A Qt-free and Vulkan-free `bloom::render` public surface
(`bloom/render/gpu_device.hpp`) exposes typed device state, per-operation-and-precision
`GpuQualification`, ordered structured diagnostics, an immutable generation-scoped capability report,
and opaque move-only buffer ownership; no `Vk*` type, native handle, or catch-all backend interface is
public. An optional Vulkan bootstrap resolves `Vulkan::Headers` and
`GPUOpen::VulkanMemoryAllocator` only from the validated dependency prefix in `qualified` mode;
absent Vulkan, or when disabled, the same API is a CPU-unavailable stub that reports a typed
`BackendNotBuilt` diagnostic, with no Vulkan header reaching the stub, the desktop application, or a
public consumer header. Device probing checks Vulkan 1.2, `timelineSemaphore`, and a compute-capable
queue family before use; a missing loader, device, or entry point produces a typed `Unavailable`
diagnostic instead of a startup crash. A privately opened dynamic loader (platform loader name only)
and a Vulkan-Hpp RAII owner host one bounded host-visible allocation path (64 MiB ceiling) that proves
the allocator is live; outstanding allocations co-own the allocator generation, and creation,
exposed operations, and destruction are owner-thread-bound. The capability report advertises no
operation: every operation/precision stays `Unavailable` until the frozen fixtures pass, and an
unsupported entry is never inherited from another operation.

Native render primitives. Their public headers are Qt-free and Vulkan-free with portable
CPU-unavailable stubs. `GpuSolid` runs `SolidV1` and the covered arm `CoveredSolidV1` (a host-built
premultiplied palette from the existing CPU coverage primitive, bit-exact on readback). `GpuImage`
owns a device-resident RGBA32F image; `GpuImageUpload` stages one already-decoded host `Rgba32fImage`
with no media decode on the GPU thread; `GpuComposite` runs `TranslationOpacityBilinearV1` and
`SourceOverV1`, each writing a new resident image; `GpuResidentDisplay` copies a resident RGBA32F
image device-to-device through the embedded Bloom Neutral V1 shader into a resident packed RGBA8
image, reading back only the 4-byte status word; `GpuNeutralDisplay` runs the fixed `OcioDisplayV1`
compute operation. Every shipped shader is an offline artifact under `tools/gpu-shaders`, pinned by
SHA-256 with a manifest binding and a configure-time `glslangValidator`/`spirv-val` regeneration check
against the embedded SPIR-V digest, so the source -> SPIR-V -> embedded-array relationship is closed
and no runtime code loads or compiles a shader. All primitives share one bounded policy: owner-thread
operations, explicit byte budgets checked against actual VMA allocation sizes and the transient
staging peak with overflow-safe arithmetic, cancellation, device-binding checks, and a bounded
teardown that quarantines an unproved submission rather than destroying it in flight.

Qualification. `bloom/runtime/gpu_neutral_display_qualification.hpp` runs the native device/pipeline
on the owner thread, refuses any processor other than the exact default Bloom Neutral v1 handle
(canonical identity byte comparison plus processor cache ID, OCIO version, and display/view
provenance), and executes real `begin`/`poll`/`readback` at the production 65536-pixel chunk. Parity
holds to the frozen contract (RGB within one straight-RGBA8 code, alpha exact) over 1 px, an odd
257-pixel tail, alpha endpoints, quantization-adjacent samples, and signed/HDR and tiny-normal
values; a nonzero subnormal frame is measured as whole-frame shader rejection and remains a per-frame
CPU fallback, never a parity failure or a claimed supported domain. Timing measures a warmup plus
alternating pairs, and the eligible interval is the contiguous faster suffix ending at 4K, so an
unmeasured or slower 4K leaves the operation CPU-only. The outcome is `PreviewOnly`; final output
stays on CPU. `gpu_resident_preview_qualification.hpp` reuses the caller's already-created typed
pipelines, refuses any pipeline not bound to the exact actual device (`GpuDevice::ownershipEpoch()`
is the unique ownership identity), and compares every pixel against the CPU primitives and the
independent OCIO oracle; its report is constructed only by its private factory and is `PreviewOnly`
only.

Scene preparation, caches, and executor. `CpuGpuSceneBuilder` (`prepared_gpu_scene.hpp`) turns a real
`CompiledCompositionPlan` plus an `EvaluationRequest` into ordered immutable `GpuSceneCommand`s
(solid, covered solid, unparented translation-only layer, image/video upload, Normal merge,
composition output) using the evaluator's real preflight resolution and, for a fractional
translation-only solid, the same CPU coverage raster; it allocates no full RGBA CPU image and fails
closed `Unsupported` for every out-of-subset reachable operation. That host-built coverage mask is a
known gap, not a GPU vector-coverage implementation: `feature.geometry.vector_coverage` stays
Required and RED until a native GPU coverage producer replaces it. Command semantic keys carry the
resolved operands plus the pinned render SPIR-V digests and never node/layer IDs, operation indexes,
or the revision. `ImageSource`/`VideoSource` leaves resolve and colour-convert on the CPU task thread
through the evaluator's own entry points and publish a frozen upload command whose source semantic key
contains no node id, plan index, frame time, or layer transform; a reachable unsupported layer is
screened before any decode. `GpuSceneCoverageCache` is a bounded transactional LRU keyed on raster
geometry alone; `GpuPreparedUploadCache` (128 MiB / 4096 entries default, zero disables) reuses a
converted source across a transform-only change. `GpuSceneExecutor` (`gpu_scene_executor.hpp`) drives
one immutable `PreparedGpuScene` as a bounded sequence of already-typed native operations on an
existing `GpuDevice`; `begin` validates and flattens the reachable DAG consulting `GpuSceneCache`
first, so a warm unchanged output performs zero native dispatches, `poll` is non-blocking and starts
at most one native dispatch per call, and the request byte budget bounds the actual LIVE unique pinned
bytes with each charge released at the last dependency. Each native op's real retained allocation
comes from an additive `hasUnretiredSubmission()`/`lastJobAllocationBytes()` accessor pair, and only a
native result that proves fence retirement may release pins: a failure with a submission still
outstanding retains every pin and refuses reuse. `GpuSceneCache` maps an already-computed semantic
digest to a resident image, charges `GpuImage::allocationBytes()`, runs only on the device owner
thread, rejects a foreign-device image by ownership identity, and never evicts an image still pinned
by an external `shared_ptr`. `GpuResidentFrameLeaseRegistry` takes strong native ownership of one
device-bound `GpuDisplayImage`, charges the actual VMA allocation bytes, and returns a move-only
owner-thread `GpuResidentFramePin`; there is deliberately no accessor returning a strong image outside
that pin.

Coverage contract and final render. `bloom/runtime/gpu_coverage_contract.hpp` is the exhaustive,
compile-time-checked registry of every `CompiledOperation`/`ImageEffectKernel` alternative, every
image-producing authoring lowering, every built-in pixel node type, and the required blend, shape,
geometry (native vector coverage), layer, colour, and display feature axes and render routes. GPU
production preparation is required by
default; adding an alternative, lowering, or pixel node type without classifying and fixturing it is
a compile failure or a missing-fixture failure, and an unclassified or unfixtured required id keeps
the coverage gate RED by name rather than warning. A fixture passes only with a genuine prepared GPU
scene (or, native-only, real native execution with parity and cache evidence); there is no
admitted-but-not-prepared pass, and an invalid plan or missing media is never a coverage pass.

The only permitted exceptions are typed and narrow: an actual pixel operation, feature, or route
exception requires a non-empty id, a rationale, and an owning document or accepted decision
reference, and no such exception exists today. Host-preparation declarations -- media container and
sample I/O and decompression, font load/shaping, parameter/curve/geometry resolution, and one final
readback -- are a separate audited list and are not a route to exempt a pixel operation, feature, or
route. Approval identifiers are never fabricated, and working-space colour conversion is a pixel
transformation that is never an opt-out. Per-pixel vector coverage rasterization (the CPU
`PathRaster::coverageRow` mask) is likewise a pixel transformation, not host preparation: a
`CoveredSolidV1` fill from a host-built mask does not satisfy the Required
`feature.geometry.vector_coverage` axis, which needs a native GPU coverage producer with real
device provenance and dispatch counters.

Preview and final rendering are both required. Interactive viewer preview, RAM preview fill and
playback, still-frame export, sequence/range export, video export, and headless/scripted render are
required routes through the actual production evaluation paths, with no preview-only exemption. A
final render may read the single composited image back once at the CPU codec/file boundary; per-node
or per-operation full-frame roundtrips are not a GPU implementation. Native acceptance is a distinct
gate: on a device it must execute the routes and assert native dispatch, resident provenance, a pixel
oracle, cold/warm cache state, and no silent CPU whole-frame render for an ordinary operation; with
no device it reports an explicit skip, never a pass, and `--require-device` fails. An unavailable
device must publish explicit Unsupported/CPU-fallback provenance, never a CPU frame labelled GPU.

Resident preview product and service. The closed `PreparedPreviewFrame` display variant has a fourth,
GPU-resident arm, `PreviewResidentDisplayFrame`, built only by the validating owner-thread factory in
`gpu_resident_preview_product.hpp`. Its only pixel storage is the opaque, owner-bound
`GpuResidentFrameLease`; it retains no CPU pixel vector, no native object, and no Vulkan handle, and
performs no readback. Alongside the lease it retains the genuine immutable resident qualification
report, the request/process identity, the evaluated bounds, and explicit
`EvaluationProvider::GpuResident`/`CpuReference` provenance. The factory validates the report's
eligibility against the exact device ownership epoch and processor, the registry binding and owner
thread, the trusted expected descriptor, the native display geometry against that descriptor, the
measured eligible interval, and the actual native allocation plus geometry inside the request budget.
`GpuPreviewDisplayService` owns one dedicated service thread, the native device/pipeline created and
used only on it, the scheduler GPU executor, and one outstanding native job at a time. It exposes two
routes on that same owner thread and scheduler lease:

- The packed-readback route (default constructor) composes the accepted CPU stage with the display
  fallback and, when the report's measured eligible interval and the overhead-adjusted comparison
  admit it, runs one bounded native `OcioDisplayV1` dispatch producing packed RGBA8 with
  `PreviewDisplayProvider::GpuNeutral` provenance; otherwise the same evaluated stage is mapped by
  the CPU display fallback and is never compiled or evaluated twice.
- The opt-in resident route (additive constructor) runs a genuine startup `qualifyResidentPreview()`,
  prepares the GPU scene on a CPU child, drives it through `GpuSceneExecutor` -> `GpuResidentDisplay`
  -> the resident product factory, and publishes an opaque owner-bound `GpuResidentFrameLease` into
  the service's presentation registry with zero full-frame readback. It supports the qualified
  Solid/covered-solid, translation/opacity, `SourceOver`, and media-upload operations plus the default
  Bloom Neutral display; anything else takes the full original CPU path on the same
  snapshot/identity/overrides.

A stage whose failure, deadline, cancellation, or lost generation leaves a native submission
unretired enters an explicit `Retiring` phase that retains the stage, its completion token, and the
native pins until retirement is actually proven. Healthy registry budget pressure is a temporary
refusal that falls back to the CPU without invalidating live leases or pins; only an actual unproven
retention or allocation failure latches the process-wide fuse. The bounded counter snapshot exposes
`fullFrameReadbacks` (always zero by construction), `displayStatusReads` (the 4-byte status word),
cache hits/misses, fallbacks, and refusals, so a test can prove no full-frame readback occurred.
`beginShutdown()` is non-blocking; the destructor joins the service thread and drains child/native
ownership before releasing the lease.

Presentation lane. The qualified Linux loader is rebuilt with `BUILD_WSI_WAYLAND_SUPPORT=ON` alone
(XCB/Xlib/Xrandr and DirectFB stay `OFF`; the Wayland branch adds no pkg-config or `DT_NEEDED` entry).
Device bootstrap adds an opt-in presentation request that enables `VK_KHR_surface`,
`VK_KHR_wayland_surface`, and `VK_KHR_swapchain` only when actually advertised, records whether the
accepted `VK_EXT/KHR_swapchain_maintenance1` present fence or `VK_KHR_present_wait` retirement
mechanism is genuinely enabled, borrows the instance to a caller-minted surface through integer
handle bits plus a per-device epoch, prefers one combined graphics+compute queue so resident images
stay exclusive, and reports `Unavailable` (CPU fallback) when the compute family cannot present.
`GpuPresentationTarget` owns the swapchain, per-image semaphores/fences, and the clear-and-present
command; it never calls `vkQueueWaitIdle`/`vkDeviceWaitIdle`, retains an image until the
presentation-engine signal actually proves retirement, propagates device loss, and quarantines a
foreign-thread or unproven teardown under a bounded process fuse. `gpu_present_image.hpp` and
`presentImage` sample the resident display image into the acquired swapchain with an explicit affine
destination/source mapping, viewer channel remap, background/checkerboard, and an optional
premultiplied RGBA8 overlay; both shaders are SHA-256 pinned with a manifest binding and a
configure-time regeneration check. `GpuPresentationCoordinator` (with the Qt-free, Vulkan-free
`GpuPresentationClient`) is a coordinator, not a thread, service, device, or message bus: it is
constructed on the existing service owner thread, does no work until its `pump()` is called, crosses a
UI-created surface as integer handle bits, reuses the target acquire/present/retire path and the
strong-lease-pin contract, publishes `Retired` only after presentation-engine proof, and bounds
retained records with `maxRetainedTargets`, an explicit `forget()` terminal acknowledgement, and a
fixed process quarantine reservation store. `bloom::ui::ViewerGpuPresenter` adopts the coordinator
client's borrowed instance through `QVulkanInstance`, creates one `QWindow`/container on the UI
thread, forwards actual Qt input, and presents only through the opaque lease/params/overlay port; it
creates no device, pipeline, queue, service, or thread and performs no native work on the UI thread.
It never reparents, hides, or destroys the container while a target is live: the host must call
`prepareForMutation()` and wait for `SafeToMutate`.

Host gating and application integration (current source). A typed optional `EditorNativeSurface`
lifecycle interface lets a host ask an editor's presenter to retire a live native target and wait for
a genuine `SafeToMutate`. `NativeSurfaceRetirementGate` makes every affected mutation all-or-nothing;
`EditorArea` defers a picker replacement of a live-native editor and reverts the picker until the
rebuild actually applies; `WorkspaceHost` retires the whole conservative tree before
split/close/collapse/root-replace/restore, reports `Deferred` rather than a premature `Restored`, and
resumes only targets still attached to the live root; `ApplicationShutdownCoordinator` emits
`shutdownQuiescent` only once BOTH task quiescence and native-surface retirement are observed, and the
application begins service shutdown on `shutdownQuiescent`. `apps/bloom/main.cpp` now constructs the
resident service overload (GPU scene stage + CPU stage + display fallback), derives bounded resident
lease/scene-cache budgets from the artist's UI frame-cache budget, requests Wayland presentation only
for a bundled loader on a genuine Wayland session, shares the coverage and prepared-upload caches, and
builds a session-refreshing GPU scene stage so relative media follows the live session base directory.
`bloom::ui::GpuViewerBootstrap` caches the service's presentation client/availability and hands every
current and future `ViewerEditor` a typed `ViewerGpuDependencies`; `ViewerEditor` presents the
resident arm through `ViewerGpuResidentController`, which owns the same-request CPU fallback, the
native CPU cover, and off-thread overlay rasterization, and forwards native window input back through
the real event handlers. The RAM preview controller fills its range with a bounded two-deep pipeline
so the next frame's CPU preparation overlaps the previous frame's display stage, with the
pixels-identity duplicate guard, out-of-order collection, cancel-all, and no cross-frame coalescing
key.

Honest outcome: the production application source wires the resident display route end to end, and a
main-linked vertical acceptance now passes over a genuine OpenEXR document (a real `AddImageLayer`
image source compiled by the real `SnapshotCompiler` and the unmodified stage factories) through the
resident frame cache into the actual `ViewerEditor`: fit, zoom-pan, and Red-channel captures match the
CPU reference with zero byte difference, and the four viewer background modes (Solid/Canvas, Black,
White, Checkerboard) each match the CPU surround, including transparent composition pixels, so the
GPU and CPU display paths agree. Warm cache reuse adds no native dispatches, the unsupported path
returns the CPU fallback, and shutdown drains cleanly. This document makes no full-application FPS
claim and no universal qualification claim. The fixed operation remains `PreviewOnly`, final output
stays on CPU, and nothing here is a Windows or macOS GPU claim; Linux Wayland is the only locally
exercised resident-present platform.

Genuinely unimplemented / future work. Per-layer GPU compositing selection by the scene evaluator;
general (non-subset) graph execution; a whole-application benchmark; the full per-operation
qualification fixtures for a future `ReferenceParity` profile (the qualified display transform and
the resident scene route remain `PreviewOnly`; no operation reaches `ReferenceParity`); runtime
compilation of generated OCIO shader programs; the cross-platform Linux/macOS/Windows parity spike;
Windows/macOS GPU support; and a reviewed XCB/Xlib/Xrandr presentation intake. The qualified Linux
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
admission caps, generation loss/recovery, and scheduler shutdown fallback are implemented and
locally exercised on Linux. The Vulkan device service, fence integration, and resource retirement
are implemented; the cross-platform Linux/macOS/Windows qualification spike remains pending, and a
synchronous fence wait hidden inside a task function is not a valid interim implementation.

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

The pinned `glslangValidator` 16.4.0 and `spirv-val` (SPIRV-Tools `b707790a`) now ship as runtime
tooling so GPU colour compilation works out of the box for the desktop, CLI, and MCP lanes.
`cmake/BloomGpuShaderTools.cmake` resolves each executable only from the qualified dependency prefix
(never the ambient PATH), verifies it stays under that prefix, and stages it and its reviewed license
and generated inventory evidence into an executable-relative private `bloom-gpu-tools/` directory for
both the build tree and install (inside `Contents/MacOS` for a macOS `.app` bundle; `bin/bloom-gpu-tools`
on Linux and Windows, with the `.exe` suffix). Installed tools carry only an `$ORIGIN` rpath when the
configure-time ELF inspection actually finds a private prefix runtime library; the operating system's
C/C++ runtime is left to the supported platform floor and is never bundled. Each app target receives
typed compile definitions for that relative layout (`BLOOM_GPU_TOOLS_AVAILABLE`,
`BLOOM_GPU_TOOLS_DIR`, `BLOOM_GPU_TOOLS_GLSLANG_NAME`, `BLOOM_GPU_TOOLS_SPIRV_VAL_NAME`, and the
inventory name); no absolute build path is embedded. An unqualified mode, a CPU-stub build, a missing
prefix, or a prefix without the pinned tools clears the capability with a typed reason instead of
falling back to a host tool, and the CPU reference path remains the supported outcome. This slice
stages tools only; it introduces no process invocation, resource limit, or runtime compiler consumer.
The lock records `shippingRoles: ["executable", "license"]` for both components and the two license
reviews were extended from their earlier build-only form accordingly.

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
