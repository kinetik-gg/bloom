# GPU Backend Architecture

Status: working

Updated: 2026-10-09

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
  select it. The render capability report's `GpuOperationId` vocabulary is `SolidV1`,
  `TranslationOpacityBilinearV1`, `SourceOverV1`, `OcioDisplayV1`, and `PackedDisplayV1`; the report
  advertises no operation until its frozen fixtures pass. The shared scene executor's additional
  typed operations (covered solid and native coverage, affine, typed blend, point resample, OCIO
  effect) are gated by the runtime operation/route coverage contract and their own native parity
  fixtures rather than by a capability-report entry.

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
compute operation. `GpuPathCoverage` is the vector coverage producer: the CPU builds the immutable
`PathRasterCoverageGeometry` (bounded integer scanline sample spans, O(edges x rows), with no
per-pixel mask and the exact `coverageRow` boundary comparison preserved through a bounded binary
search over the real quarter-sample positions), and one pure-integer compute dispatch turns those
spans into the exact 8-bit coverage mask `PathRaster::coverageRow` produces, retained device-resident
as a packed little-endian R8 storage buffer. `GpuSolid::beginCoveredResident` then binds that resident
buffer directly for the covered fill, with no download or re-upload; the coverage buffer is
co-owned until the consuming submission is proved retired, and the consume path validates owner
thread, exact device identity, and generation before readiness or dimensions. In-flight submissions
are serialized by one process-wide bounded reservation claimed before any native allocation: a
proven retirement returns it, an unproven one moves the exact submission into an owner-only
quarantine, and admission is refused while occupied. The failed producer keeps its exact
reservation token and device identity: `hasUnretiredSubmission()` remains true until owner polling
proves that quarantine retired. Failed polling never publishes a mask, and a stale token cannot
retire a later job. Native access also checks the exact owner thread lifetime. Destroying a
quarantined producer orphans its existing resident slot so descriptors and pipeline outlive the
submission; non-blocking owner drain releases that slot only after real fence completion or actual
driver device loss. Injected loss diagnostics require real fence completion before cleanup. The
quarantine fuse records historical unproved retirement, independently of current slot occupancy.
Every producer native resource set (pipeline
plus resident mask) holds one of a fixed number of process-wide resident slots, acquired before its
first native allocation and held until owner-thread retirement; creation is lazy, so idle producers
allocate nothing. A producer destroyed on a foreign thread only marks its already-owned slot
orphaned (ownership is preserved, never destroyed off-thread); the owner drain retires orphaned
residents allocation-free and returns their slots, and it gates every fence query and teardown on
the exact owner thread and generation, so one device thread can never touch another's orphan. It
therefore refuses cleanly when the pool is full and recovers once the rightful owner drains. The
dispatch is a flattened 2D grid bounded by
the real device workgroup-count limits, so a capacity-valid large geometry is not refused by a 1D
grid. The kernel requires no Float64/Int64 capability, and a configure-time disassembly rejects
either. Every shipped Bloom-authored kernel is an offline artifact under `tools/gpu-shaders`, pinned
by SHA-256 with a manifest binding and a configure-time `glslangValidator`/`spirv-val` regeneration
check against the embedded SPIR-V digest, so the source -> SPIR-V -> embedded-array relationship is
closed and no runtime code loads or compiles a Bloom kernel. OCIO-generated shader text is the
separate runtime-compiled path, described under "Shaders And OpenColorIO". `GpuAffine` runs
`AffineBilinearV1`, a complete composed affine (rotation, uniform/nonuniform/negative scale, anchor,
translation, and any precomposed parent matrix) over one resident image; the host uploads one compact
O(1) affine map and the kernel reconstructs each source-local coordinate on the device with portable
split-precision arithmetic that does not require a device float64 or 64-bit integer capability, held
to the documented per-finite-component 2e-6 RGB parity tolerance with bit-exact alpha at the 0 and 1
endpoints rather than a universal exactness claim, so no O(width*height) host metadata loop and no
host pixel resampling occurs. `GpuBlend` runs `BlendV1`
for any `core::BlendMode` over two resident images, and `GpuPointResample` runs `PointResampleV1`,
the bit-exact nearest-neighbour gather the media-image proxy path uses. `GpuProcessReadback` and
`GpuOutputColorReadback` are the single bounded, cancellable device-to-host transfers reserved for
the final export boundary, never the preview path. All primitives share one bounded policy:
owner-thread operations, explicit byte budgets checked against actual VMA allocation sizes and the
transient staging peak with overflow-safe arithmetic, cancellation, device-binding checks, and a
bounded teardown that quarantines an unproved submission rather than destroying it in flight.

Initialization failure cleanup (G2A). The shared Solid base pipeline and Upload command resources
reset native children before their descriptor/command pools, matching reverse member declaration
order. Composite, Affine, and Blend already follow that order. Their private native regression
fixtures now fail before every Solid/Upload initialization API and after the complete native set
exists, run the actual lazy `begin` cleanup, and retry the same owner successfully. The fixtures
assert exact native-object creation/destruction and slot returns, no submitted work or published
image on failure, and baseline live VMA allocation count/bytes after failure and retry destruction.
The existing bounded pool, foreign-owner isolation, unproven-fence retention, and cancellation
fixtures remain in place. Linux native core/synchronization validation and Solid, CoveredSolid,
Upload, Composite, Affine, and Blend parity pass locally; this does not qualify Windows/macOS GPU
execution. Native-object counts and live VMA allocations do not measure driver-reserved memory or
reusable committed allocator blocks. Metadata staging lifetime, foreign-thread image-handle release,
and broader submission retirement remain subsequent G2 work.

Qualification. `bloom/runtime/gpu_neutral_display_qualification.hpp` runs the native device/pipeline
on the owner thread, refuses any processor other than the exact default Bloom Neutral v1 handle
(canonical identity byte comparison plus processor cache ID, OCIO version, and display/view
provenance), and executes real `begin`/`poll`/`readback` at the production 65536-pixel chunk. Parity
holds to the frozen contract (RGB within one straight-RGBA8 code, alpha exact) over 1 px, an odd
257-pixel tail, alpha endpoints, quantization-adjacent samples, and signed/HDR and tiny-normal
values; a nonzero subnormal frame is measured as whole-frame shader rejection and remains a per-frame
CPU fallback, never a parity failure or a claimed supported domain. Timing measures a warmup plus
alternating pairs, and the eligible interval is the contiguous faster suffix ending at 4K, so an
unmeasured or slower 4K leaves the operation CPU-only, and no measured speedup is claimed. The
qualification outcome is `PreviewOnly`; it is not selected for final output.
`gpu_resident_preview_qualification.hpp` reuses the caller's already-created typed
pipelines, refuses any pipeline not bound to the exact actual device (`GpuDevice::ownershipEpoch()`
is the unique ownership identity), and compares every pixel against the CPU primitives and the
independent OCIO oracle; its report is constructed only by its private factory and is `PreviewOnly`
only.

Scene preparation, caches, and executor. `CpuGpuSceneBuilder` (`prepared_gpu_scene.hpp`) turns a real
`CompiledCompositionPlan` plus an `EvaluationRequest` into ordered immutable `GpuSceneCommand`s
(solid, covered solid and native vector coverage, translation, affine layer transform, source-over
and typed blend, OCIO colour/effect transform, point-resample proxy gather, image/video upload,
nested composition, merge, and composition output) using the evaluator's real preflight resolution
and the exact resolved operands; it allocates no full RGBA CPU image and fails closed `Unsupported`
for every reachable operation it cannot prepare. A vector source emits the
immutable bounded `PathRasterCoverageGeometry` (integer scanline spans, never a host per-pixel mask)
that the executor rasterizes through the native `GpuPathCoverage` producer; scene integration keeps
that producer's device provenance and dispatch counters, so the Required
`feature.geometry.vector_coverage` axis is carried by real GPU work rather than a host mask. Command
semantic keys carry the resolved operands plus the pinned render SPIR-V digests and never node/layer
IDs, operation indexes, or the revision. `ImageSource`/`VideoSource` leaves decode raw on the CPU
task thread through the evaluator's own entry points and publish a frozen upload command; when the
shared GPU colour context is available, a real input-to-working OCIO transform and a fractional
proxy point-resample then run as GPU commands over that upload, so the decoded samples are not
colour-converted on the host. Without that context the leaf keeps the connected CPU conversion path
as the fallback. The upload/source semantic key contains no node id, plan index, frame time, or
layer transform; a reachable unsupported layer is screened before any decode. `GpuSceneCoverageCache` is a bounded transactional LRU keyed on raster
geometry alone. Rooted `GpuPreparedUploadCache` now attaches to the existing bound operation/source
retention pool; numeric standalone byte/count controls remain a legacy path. The bounded shared
retention contract below has qualified bounded edited-source reuse; broader G5 work is postponed. `GpuSceneExecutor` (`gpu_scene_executor.hpp`) drives
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
geometry (native vector coverage), region-of-interest, layer, colour, and display feature axes and
render routes. GPU
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
transformation that is never an opt-out. Per-pixel vector coverage rasterization is likewise a
pixel transformation, not host preparation: a `CoveredSolidV1` fill from a host-built
`PathRaster::coverageRow` mask does not satisfy the Required `feature.geometry.vector_coverage`
axis. The resident `GpuPathCoverage` producer performs that rasterization from bounded CPU scanline
geometry, and the shared scene builder/executor now emit and dispatch it while retaining its
device provenance and dispatch counters; the native coverage fixture owns that evidence. A requested
region of interest is likewise a Required route (`feature.roi`): the prepared scene clips its native
output to the requested data window exactly as the CPU evaluator does, preserving native windows and
pixel aspect, with a native ROI fixture and missed-GPU detection.

Preview and final rendering are both required. Interactive viewer preview, RAM preview fill and
playback, still-frame export, sequence/range export, video export, and headless/scripted render are
required routes through the actual production evaluation paths, with no preview-only exemption. A
final render may read the single composited image back once at the CPU codec/file boundary; the GPU
route's one combined readback transfers the process payload plus, when present, the encoded output in
a single submission, and per-node or per-operation full-frame roundtrips are not a GPU
implementation. Native acceptance is a distinct
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
used only on it, the scheduler GPU executor, and one outstanding native job at a time. It exposes the following
routes on that same owner thread and scheduler lease:

- The packed-readback route (default constructor) composes the accepted CPU stage with the display
  fallback and, when the report's measured eligible interval and the overhead-adjusted comparison
  admit it, runs one bounded native `OcioDisplayV1` dispatch producing packed RGBA8 with
  `PreviewDisplayProvider::GpuNeutral` provenance; otherwise the same evaluated stage is mapped by
  the CPU display fallback and is never compiled or evaluated twice.
- The opt-in resident route (additive constructor) runs a genuine startup `qualifyResidentPreview()`,
  prepares the shared GPU scene on a CPU child, drives it through `GpuSceneExecutor` ->
  `GpuResidentDisplay` -> the resident product factory, and publishes an opaque owner-bound
  `GpuResidentFrameLease` into the service's presentation registry with zero full-frame readback. The
  prepared scene now covers every current compiled pixel operation: solid and covered-solid (native
  vector coverage), translation, affine layer transforms, `SourceOver` and typed `BlendV1`, OCIO
  colour/effect transforms, point-resample proxy gathers, media upload, merge, nested composition,
  composition output, and a requested region of interest, plus the default Bloom Neutral display. A
  request the builder refuses (for example non-`Reference` quality, or a working space reaching an
  operation without verified working-space semantics) takes the full original CPU path on the same
  snapshot/identity/overrides rather than mis-rendering.

A stage whose failure, deadline, cancellation, or lost generation leaves a native submission
unretired enters an explicit `Retiring` phase that retains the stage, its completion token, and the
native pins until retirement is actually proven. Healthy registry budget pressure is a temporary
refusal that falls back to the CPU without invalidating live leases or pins; only an actual unproven
retention or allocation failure latches the process-wide fuse. The bounded counter snapshot exposes
`fullFrameReadbacks` (zero on the native resident route; scene readbacks plus final packed
transfers on the packed route), `displayStatusReads` (the 4-byte status word),
cache hits/misses, fallbacks, and refusals, so a test can prove no full-frame readback occurred.
`beginShutdown()` is non-blocking; the destructor joins the service thread and drains child/native
ownership before releasing the lease.

GPU-prepared packed service foundation (2026-10-09). The explicit `GpuPreparedPacked` product
mode runs the genuine GPU scene and selected Neutral or general OCIO display processing, then
copies only the final RGBA8 data window into host storage. It works without a WSI target. This is
separate from the older CPU-stage/`GpuNeutral` display route above. The process identity remains
`EvaluationProvider::GpuResident`; the display provider explicitly records transferred host pixels.
Ordinary application preview now selects this packed product for QWidget painting, with explicit
GPU-prepared/CPU-painted status. The native service route remains available and required.

The owner submits and polls the real fence without a GUI wait; host materialization also stays off
the UI thread. Admission validates exact device control-block identity, allocator generation,
owner thread and lifetime token. Numeric allocator generation alone is not device identity.
Completed immutable products retain sealed fence evidence and weak exact-device provenance,
not a native image, lease or Float32 process frame. Full `displayWindow` maps the composition;
`dataWindow` determines packed ROI extent, row addressing and transferred bytes. Consumers must
place that rectangle within the display mapping and paint ordinary background outside it.
Display-only restamps share bytes and proof while preserving exact request/display identity.

Actual source allocation, rounded staging allocation and host vector capacity are distinct from
conservative admitted reservation ceilings. Retained product cost includes pixel/bounds capacity,
proof, outer envelope and both owned request identity strings, with overflow refusal. Successful
`take` moves host storage once and clears transfer retention; product/mailbox/cache ownership is
then charged separately. One bounded active transfer and queued newest request cannot publish a
superseded result. Cancelled or unproved work remains charged until actual fence retirement.
Bounded shutdown quarantines only the final image/device/fence/staging, releases unrelated scene
and host context, and preserves its independent reservation through scheduler/service destruction.
An exited owner is explicit; future recovery is not promised.

Private Linux strict core/synchronization checks qualify WSI-disabled Neutral/general Full, Half,
ROI and adjusted output against CPU pixels with zero CPU fallback. Full 1920×1080 measured source
8,847,360 B, staging 8,294,400 B and host 8,294,400 B. Shutdown retained the source/staging sum
17,141,760 B with host zero after service/Core/scheduler destruction; its approximately 1 GiB
reservation was an admission ceiling, not actual allocated memory. Two hundred restamps reused
bytes/proof without extra preparation, dispatch or transfer. This is core reuse evidence, not
normal Viewer/cache-100 playback, clipping, sustained performance or other-platform qualification.

Resident capacity and host/device separation. The resident route sizes its one pool from the
resolved device allocation budget, never from host RAM alone. On the device owner thread
`GpuDevice::availableAllocationBudget()` reports either a live `VK_EXT_memory_budget` figure or a
conservative nominal `DEVICE_LOCAL` estimate, and the runtime partitions that one pool into the
frame-cache, lease-registry, scene-cache, and per-request sub-budgets with bounded cache headroom,
clamped so the total can never overcommit the device and never raised above the artist's configured
ceiling. Host limits and device limits are separate: a host decode/pixel-storage ceiling larger than
the device share is clamped only for the device stage while the host ceiling is preserved for
decoding and the CPU fallback, and a zero device ceiling is the honest no-device-admission signal
that takes the CPU path. A genuinely unresolved device uses a small safe fallback pool; a known-zero
device is never turned into that fallback.

Within that pool the lease-registry/frame-cache, scene-cache, and per-request ledgers are
partitioned 50/20/30. The per-request ledger is the only one that holds RGBA32F working images, and
a single source-over or translation step simultaneously holds the merge accumulator, the foreground
input, and its own new output, plus the native operation's actual VMA allocation, status buffer, and
staging peak. The request share therefore has to cover roughly three full-size float frames, not the
two a preview-only reading suggests; a smaller share refuses an ordinary full-resolution composition
that the device could actually run. Because the partition divides the SAME pool, giving the request
ledger that room leaves fewer RGBA8 resident frame-cache entries, which is the deliberate trade-off.
The pool itself is still the resolved device budget clamped to the artist's ceiling, so a
composition whose real working set exceeds that capacity is refused honestly and falls back rather
than being claimed as supported.

Resident metadata admission is independent of pixel extent. The lease registry retains its finite
4096-entry ceiling; the cache uses at most 3584 slots, reserving 512 for in-flight, presented and
tombstoned leases. Entries allocate lazily. A positive byte pool no longer derives a cache count by
dividing by an assumed 8 MiB frame, and a zero pool remains disabled. Actual native allocation bytes,
retained metadata and the owner-resolved device clamp remain authoritative. Full and Half retain
separate resolution/policy keys: 1920×1080 and 960×540 resident RGBA8 display products respectively.
Working images remain RGBA32F; this count-policy correction changes neither processing precision
nor the existing lease/scene/request byte split. One hundred packed Half display images occupy
about 198 MiB before native alignment and metadata; the actual device charges must still fit the
cache and lease ledgers. Metadata headroom is a finite allowance, not a guarantee that arbitrary
concurrent requests fit either byte or entry admission.

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
fixed process quarantine reservation store.

The clear/discard layout transition starts at `TRANSFER`, matching the acquire semaphore wait
stage; discarding contents with `UNDEFINED` does not remove the acquire execution dependency.
Present-fence and present-id scalar backing storage stays alive through `vkQueuePresentKHR`.
Target acquire defaults to zero-timeout `Poll`; the explicit owner-only `BoundedProgress` mode
requests a fixed 1 ms maximum driver wait. `VK_NOT_READY` and `VK_TIMEOUT` remain temporary
`NotReady` results, without an acquired index or acknowledgement. After an ordinary poll returns
`NotReady`, the coordinator admits at most one such probe across the entire owner pump, rotating
traversal after the consumed target. All targets still receive retirement driving. Locked admission
rejects newly closed, resizing, retiring, or shutdown targets; no mailbox lock spans the driver call.
An already admitted finite call may finish after a concurrent close request. This bounds aggregate
requested driver wait to 1 ms per pump, independent of target count, rather than promising a hard
wall-clock deadline or adding UI waits.

Isolated native evidence on 2026-10-09 reproduced the clear acquire/layout synchronization hazard.
Zero-timeout and a controlled 1 ns acquire stalled after initial buffers; server release messages
were sent but the client did not process them. A controlled 1 ms acquire restored progress.
The final typed policy passes repeated clear/image reuse, resize, acquired-unpresented retirement,
and eight simultaneous three-target reuse rounds with core and synchronization validation on the
isolated Vulkan and GL compositors. These are lifecycle/progress checks, not viewer performance or visible-intent
qualification. Qt before/after present hooks remain a separate integration gap assigned to G4D,
after completion notification work: fixture-only hooks did not resolve the observed stall, and the
Wayland before hook can wait for frame synchronization. That integration needs a bounded UI-thread
design and its own qualification.

`bloom::ui::ViewerGpuPresenter` adopts the coordinator
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
resident service overload (GPU scene stage + CPU stage + display fallback), derives the configured
resident lease/scene-cache/request budgets from the artist's UI frame-cache ceiling, requests Wayland
presentation only for a bundled loader on a genuine Wayland session, shares the coverage and
prepared-upload caches and the one shared OCIO resolver/context, and builds a session-refreshing GPU
scene stage so relative media follows the live session base directory.
`bloom::ui::GpuViewerBootstrap` caches the service's presentation client/availability and installs
the owner-resolved, capacity-clamped resident plan on the shared frame cache through the existing
status poll, then hands every current and future `ViewerEditor` a typed `ViewerGpuDependencies`;
`ViewerEditor` presents the
resident arm through `ViewerGpuResidentController`, which owns the same-request CPU fallback, the
native CPU cover, and off-thread overlay rasterization, and forwards native window input back through
the real event handlers. The embedded window's logical geometry belongs to its Qt window container;
device-pixel extents are passed separately to the presentation service. The CPU cover is an
output-only raster `QWindow` with its own public `QBackingStore`, owned through an alien wrapper and
host. Its persistent alpha-capable child is established under the presenter's GPU window before
first presentation and stays mapped until genuine target retirement. Concealment paints transparent
pixels; a required cover paints opaque Canvas even when its snapshot is unavailable. This preserves
the Wayland subsurface role: creating a new role while an idle/refused GPU parent never commits again
cannot make the cover visible. Before first GPU application, one temporary top-level child shares
the same snapshot, then is destroyed on genuine application. Steady state has one raster backing
store. Every genuinely new presenter establishes its first-attach cover again, including neutral
recovery after CPU adjustment retirement; previous CPU concealment cannot suppress the new child.
Initial attach has at most two raster backing
stores. Qt Wayland bounds each platform pool to five buffers, so this
is a retained-pixel inventory, not an aggregate-budget claim. Snapshot/resize/expose updates coalesce
through Qt, and unchanged content/geometry/cover state skips painting. This avoids Qt Wayland's
decorated-parent native-widget backing-store crop and undefined overlapping container stacking,
while keeping editor siblings alien. It uses no private Qt APIs, decoration-offset compensation,
native-decoration bypass or extra parent/GPU commits. A blank frame or project replacement
retires the previous target before unmounting it;
completion is queued onto the UI event loop before destroying the presenter. Resizing retains a
pending presentation of the current frame until the new extent is ready, and an older acknowledgement
cannot complete that pending presentation.

Coherent outline publication (G3B1). The controller captures a complete immutable image/parameters/
overlay-recording pair, with one active raster and one newest request. A changed required overlay
never causes an outline-free replacement: the last complete pair remains visible, and a completed
older coherent pair may advance the display while newer geometry is still being prepared. Its exact
owner-applied identity remains stale until the newest pair applies. Project/composition, surface,
selection/style, removal, and viewport extent domains prevent late completions from resurrecting
removed handles or crossing a replacement. Reuse compares the actual recorded commands and logical/
device extent, rather than trusting a hash or request generation; direct UI comparison is capped at
64 KiB of commands, and larger recordings conservatively rasterize on the existing worker. Raster
failure retains the last complete pair only when its requested safety domain still matches, with a
concrete diagnostic; otherwise it raises the CPU cover/fallback. There is no scheduler-cadence
change or UI wait.

Publication admission is coalesced to one owner-outstanding update plus one newest complete update.
The controller retains at most six distinct immutable frame handles across applied, two submission
correlations, ready, active, and newest slots; the command-stream reuse key retains no frame, and
the 32 trace correlations retain identity only. An exact consumed-owner `rejectedSequence` releases
the admission without inventing an applied acknowledgement.
Native owner acknowledgement proves submitted presentation, not compositor-visible pixels; external
bounded fixture captures are the separate visible oracle.

Actual displayed intent (G3B2B). Desired viewer controls, a complete prepared candidate, mailbox
admission, an exact owner-applied native descriptor, and a painted CPU/cover descriptor are separate
states. Missing or failed adjusted preparation retains a same-project/composition last-good image
visibly stale; it does not retire the native target or substitute neutral pixels as the requested
adjustment. Identical failed adjustments are not retried on polling. Cancellation completion may
schedule only the newest still-live desired identity. Shared Failed/Unsupported configuration
diagnostics remain authoritative. A ready candidate without actual publication has no displayed
frame and reports presentation pending.

Native availability and currentness are distinct. A mapped, exposed previous publication remains
the actual displayed descriptor, labelled Stale while a changed overlay or retirement is pending.
Currentness and safe retention additionally require Active and the exact requested domain. Hidden
or unexposed Viewer/native/active-cover output has no actual descriptor; an unavailable live native
surface cannot expose an old CPU-painted descriptor underneath it. Native exposure is a necessary
availability check, not an independent compositor pixel acknowledgement.

Resize completion has its own `completedResizeSequence`, advanced only after successful native
recreation. Image `appliedSequence` continues to describe a submitted image publication. The
presenter coalesces newest requests during attach/resize and keeps admission closed until that
resize sequence completes, including A→B→A where matching extent alone cannot prove completion.
Retirement takes precedence. These lifecycle checks do not solve Qt container clipping: external
private captures still reproduce spill on held resize and top-level shrink. Native-widget viewport
and geometry-admission experiments were rejected before production integration. The next bounded
repair is an independently reviewed asynchronous final GPU RGBA8 transfer into ordinary QWidget
painting; it must preserve GPU scene preparation, exact provenance/identity, budgeted host storage,
safe cancellation/retirement and the required native rendering route.

The correlated native descriptor captures frame, geometry, destination, channel/background,
selection/removal and surface domains, logical container rectangle, and DPR before enqueue. Its
metadata remains readable when the lease stops admitting work; it never grants reuse permission.
Refusal retains native pixels only in the same project/composition, selection/removal, live surface,
logical/device extent and DPR domain. An unsafe refusal, resize or domain clear requires an opaque
CPU cover or explicit unavailable display. Late old-domain acknowledgements cannot uncover it or
restore currentness. CPU cover metadata describes the exact completed capture, including blank or
budget-refused capture; it never inherits the newest candidate's identity. Project/composition
cleanup is keyed to each distinct domain and precedes the retirement early return. A second domain
change refreshes or blanks the installed cover while preserving the mapped target and the single
pending retirement. Failure notifications carry concrete diagnostics without synthesizing present
acknowledgements.
An explicit document rebind invalidates all viewer-local retained frames, probe/cache ownership and
publication domains even when numeric project/composition/revision IDs collide. Cancelled adjustment,
fallback and probe handles are detached; an old completion cannot publish into the new owner.

Passive hit mapping and evaluated bounds use the same actual frame and captured destination. They
reject foreign project/composition/time and pending selection domains before retained-layer remapping.
An ongoing gesture freezes its actual mapping and validates real time/domain/format/viewport/
transform/selection/lifecycle changes, rather than cancelling on preparation lag or lease admission
expiry. GPU Auto resolution stays stable while a per-viewer adjustment is pending. The pixel probe
is unavailable for another displayed intent or channel; it cannot sample the newest prepared frame
and label it as painted pixels.

The applied descriptor aliases the existing applied frame; immutable geometry adds no distinct
frame. Each viewer adds two bounded CPU descriptor slots (paint and cover), which may retain older
frames than its current preparation/fallback slots. The temporary capture descriptor aliases the
capture source and is reset after installation. These metadata slots add no pixel copies or GPU
readbacks; the existing bounded cover pixmap remains the only cover capture. The independent raster
surface replaces the former child crop/copy from the parent's shared QWidget backing store and still
paints into its own platform backing-store buffers. Qt Wayland 6.11.2 bounds each pool to five
buffers: at most ten during temporary first attach, and five in steady state. This is retained
platform pixel storage for the G6 inventory, not an aggregate-budget repair or a claim of one
platform buffer. Foreign CPU handles are cleared on replacement, and native
descriptors/correlations are cleared after real retirement when the presenter resumes.

The owner pins a replacement lease before acquiring a swapchain image. Rejection releases only
that replacement's pending overlay charge and publishes its exact rejected sequence, retaining
the previous native image and active overlay charge. Acquire/retry/retirement proof is unchanged.

G3B remains incomplete until its independent visible-intent and presentation-integration gates are
qualified. G3B2B covers actual display identity, last-good/refusal policy, failed/non-neutral
adjustment, passive/frozen gesture mapping, and selected-viewer status. G3B2A repairs the underlying
clear/discard dependency and bounded WSI progress; that bounded slice is accepted after independent
source review and native reruns, CPU checks, a Vulkan-disabled render build and source-quality checks.
Metadata/identity and lifecycle checks do not alone qualify compositor-visible pixels or every
refresh during interaction. This does not claim cross-platform native GPU qualification; the explicit CPU
fallback remains supported on macOS and Windows.

The RAM preview controller fills its range with a bounded two-deep pipeline
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
claim, no measured speedup claim, and no universal or cross-platform qualification claim, and the
resident-preview outcome remains `PreviewOnly`. The final-render route now evaluates through the
same application-owned GPU provider for still, range, video, and headless export: the general
output-colour production context resolves the OCIO display/effect program on the blocking CPU stage,
the shared scene composes and reads the single combined process-plus-encoded result back once at the
codec/file boundary, and a 6000x4000 media+CST+text export has locally verified native execution.
The CPU reference evaluator remains the correctness oracle and the explicit supported fallback. GPU
execution is locally exercised on Linux only; macOS and Windows have an explicit supported CPU
fallback, not a GPU parity claim.

Genuinely unimplemented / future work. A whole-application benchmark; the full per-operation
qualification fixtures for a future `ReferenceParity` profile (the qualified display transform and
the resident scene route remain `PreviewOnly`; no operation reaches `ReferenceParity`); the
cross-platform Linux/macOS/Windows parity spike; Windows/macOS GPU support; and a reviewed
XCB/Xlib/Xrandr presentation intake. The scene evaluator now prepares every current compiled pixel
operation through one shared scene, including a requested region of interest. The qualified Linux
prefix manifest remains pending, so this direction stays `working`.

Audit checkpoint (2026-10-08). A source review of the merged implementation identified native
buffer-usage/visibility and failure-retirement defects, repeated source verification before warm
reuse, production scheduling delays, incomplete aggregate memory ownership, and a gap between
prepared/cache progress and actually presented frames. Earlier pixel-parity and route evidence
does not establish native validation cleanliness, sustained playback, or whole-application memory
stability. The [GPU reliability and preview plan](../plans/gpu-reliability-and-preview.md) records
the evidence, its limits, and scoped repair/qualification packages. Its redesign choices remain
proposed; this checkpoint does not adopt them or weaken the existing coverage contract.

G1 native-validity repair (2026-10-08; source reviewed and locally qualified on Linux).
Resident Neutral display buffers now declare their image-copy transfer usages, and its mixed
host/transfer input dependency names both source stages. CompositeBuffer initialization flushes
both direct mappings and metadata staging through allocation-relative VMA ranges; staged metadata
also records transfer-write to shader-read visibility. Composite translation/source-over, Affine,
and Blend make shader-written status visible to the host before their fences and invalidate the
status word before CPU inspection. A missing mapping or failed flush/invalidate rejects output.
General OCIO status uses the same compute-to-host dependency. Final process/combined-output
readbacks and full/sparse pixel-oracle readbacks record transfer-to-host dependencies covering the
exact copied destination bytes before fence retirement and invalidate. Shader identities, CPU
semantics, single final-readback policy, and coverage requirements are unchanged.

G2B metadata lifetime repair (2026-10-08; implementation qualification recorded in the reliability
plan). Buffer preparation no longer submits or waits on a private queue operation. Translation's
X/Y metadata and PointResample's axis initialization copies execute in the owning compute command
buffer, with transfer-write to compute visibility after flushed staging writes. Staging shares the
real allocator owner and moves with its destination through the existing job/quarantine lifetime.
Both Translation axes remain alive concurrently until the same completion proof; actual VMA
staging allocation sizes are summed in admission and live-byte accounting. Proven completion frees
staging before a successful image remains cached. Timeout and cancellation never authorize early
release; injected device-loss diagnostics first prove the real submission retired. Direct mapped
Affine/status initialization retains the flush contract and rejects a missing mapping.

Presentation enables swapchain maintenance only with the matching EXT/KHR surface-maintenance
instance extension and its surface-capabilities2 prerequisite. Without them, existing present-wait
or unsupported-presentation behavior remains available. The present render pass explicitly chains
its initial layout transition to the acquire semaphore's color-attachment-output wait stage.
Composite's descriptor pool now reserves five storage-image descriptors for its two actual layouts.

The local G1 native run reported zero core/synchronization validation errors for Neutral/general
display, Composite/Affine/Blend CPU parity, PointResample, the production general display service,
full-resolution viewer and original interaction fixtures, genuine RAM fill/replay, and the mixed
media/CST/text final-render route including 6000×4000. The RAM fixture retained and replayed four
frames with zero warm dispatches, zero CPU fallbacks, and zero production full-frame readbacks.
Instrumented native tests call real VMA on success, check mapped allocation-relative ranges in both
initialization paths and actual operator consumers, and inject flush/invalidate failures. Observed
mappings were host-visible, coherent, and cached; no noncoherent memory execution was exercised.
Present fences and present-wait were available locally; unavailable-extension fallback combinations
and macOS/Windows GPU execution remain unqualified. These bounded results do not qualify sustained
playback, whole-application memory stability, or G2 failure/retirement behavior. The visibility
sequence follows the [Khronos synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html).

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

The seam itself creates no device, service, or thread; it separates CPU preparation from the display
product. The application now consumes it alongside the GPU scene stage: `apps/bloom/main.cpp` builds
the resident service overload, so an evaluated `ProcessFrame` can flow into the shared GPU executor
and display product instead of only the CPU fallback. The CPU stage and `PreviewCpuDisplayFallback`
remain the correctness oracle and the explicit unavailable path.

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

The final-render route now evaluates through the application-owned GPU provider described in
[`frame-output.md`](frame-output.md), with the single final CPU readback at the codec/file boundary
and the CPU reference evaluator as the correctness oracle and explicit `Disabled`/unavailable
fallback. That is a production preparation/execution route, not a `ReferenceParity` claim: any
stricter final-output policy remains an explicit decision rather than an automatic consequence of
having a GPU.

GPU process cache identity includes the process request identity, primitive and backend semantics,
backend/shader/SPIR-V/compiler revisions and options, texture/storage precision, operation
capability-profile digest, and device/driver identity whenever any of them may alter pixels. Display
cache identity additionally includes the qualified OCIO display processor identity from
[`color-management.md`](color-management.md). Request generation, cancellation, queue depth, and
budget do not affect pixel identity. Resource-generation identity is separate and prevents a cache
object from surviving device recreation even when its pixels would be equivalent.

## Shaders And OpenColorIO

Bloom-authored kernels use one reviewed Vulkan GLSL source path. Shipped Bloom shaders are compiled
to SPIR-V at build time, validated, reflected into explicit bindings, and packaged with a source and
compiler version; runtime code never compiles or loads a Bloom kernel. OCIO-generated shader text is
the one runtime-compiled path: it is extracted from the project's resolved config, wrapped, and
compiled to SPIR-V on a CPU worker using the packaged, pinned `glslangValidator`/`spirv-val` tools.
Runtime specialization prefers constants and pipeline variants over unbounded source generation.

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
the UI and GPU service threads, and off the native GPU owner thread, as a bounded, cancellable child
process of the packaged tools with no shell involved; the shared `GpuOcioContextResolver` qualifies
the executable-relative package once on a CPU worker and publishes one `GpuOcioProgramPreparer`.
Bloom accepts the output only after SPIR-V structural validation and reflection exactly match the
declared entry point, bindings, descriptor types, dimensions, and limits. Pipeline creation remains
on the GPU service thread and is covered by device-loss containment. A timeout, tool crash,
validation mismatch, driver failure, or budget breach produces an operation-scoped diagnostic,
publishes no partial pipeline or LUT, and cannot poison an existing qualified cache entry.

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
falling back to a host tool, and the CPU reference path remains the supported outcome. The staged
tools are the runtime consumer's only compiler: the OCIO program preparer invokes them through the
platform process supervisor with explicit source/SPIR-V/diagnostic byte ceilings, an address-space
and open-file limit, a deadline, and cancellation, never a shell or an ambient lookup. The lock
records `shippingRoles: ["executable", "license"]` for both components and the two license
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

The native route targets a same-device GPU copy or sample into each viewer swapchain. Multiple
native panels share the render device where surface support permits and own independent swapchains.
Measured held resize and top-level shrink reject this embedded route as the current clipping repair:
197,225 neighbor pixels and 89,125 exterior pixels escaped their intended boundaries. Native-host
and geometry-admission experiments were rejected, not installed.

The [bounded ADR revisit](../decisions/0006-gpu-backend.md) therefore authorizes a final GPU RGBA8
transfer followed by ordinary QWidget painting with explicit GPU-prepared/CPU-painted provenance.
Stage 2 now adopts this route for ordinary application preview. Painting borrows immutable packed
storage and maps the true ROI data window inside the full display window, painting background outside.
An older coherent image, bounds and channel remain explicitly Stale until replacement; collapsed or
offscreen content advertises no displayed pixels. The cache retains only the sealed host product,
not a native image, lease or Float32 process frame. Cold preparation costs staging, host storage
and one final transfer; fitting warm reuse adds none of those preparation steps.

The channel presentation scan runs in a cancellable bounded CPU worker, with explicit CPU channel
provenance. One completed derivative, one active task and one newest intent bound publication;
admission includes held source/derivative/active allocations and retained legacy process capacity.
Rebind and cancellation reject late old-owner results. This is a responsiveness repair, not required
GPU channel execution or an aggregate memory broker. Exposure/gamma/OCIO remain genuine GPU work.

Private Linux external pixels qualify held panel/top-level shrink, same-size ancestor movement,
ROI, eight handles and collapse without new image spill. Animated cold playback displays a coherent
older frame as Stale while the newest intent remains pending, then pause converges to Current.
Controlled Full/Half 100-frame EXRs retain 829,620,400/207,540,400 host bytes respectively under a
1 GiB host cache, with zero native retained products. Two hundred Current readiness observations
(199 changed-time seeks) add zero scene evaluation, dispatch, transfer or cache growth; they do not
prove 200 forced paints. The native route and final GPU routes remain required. Linux samples do
not establish equivalent macOS/Windows GPU execution or sustained performance; explicit CPU fallback
remains supported. Aggregate/process-limit accounting, remaining channel processing and broader
cache policy qualification stay separate work.

### Controlled mounted-footage A (bounded Full/Half evidence)

The 2026-10-10 private Linux check uses 100 strongly hashed contiguous EXR members, numbered
1001–1100, through the production compiler, GPU packed preparation, RAM controller/cache and Viewer.
The 1920x804 RGB HALF source is centered over authored opaque black in a 1920x1080 composition at
an explicit controlled 24 fps; the separately recorded source rate is 24000/1001. Full output is
1920x1080 with 138 black rows above and below; Half is 960x540 with 69. Every retained frame matches
an independent expected-member still-image CPU plan, including geometry, black margins and
nonuniform source pixels. EXR Auto assumes `lin_rec709_scene` because chromaticities are absent;
parity qualifies that application interpretation, not independent source-color intent.

Both runs retain 100 genuine GPU-prepared products: an initial foreground frame plus 99 RAM refill
products. Full owns 829,440,000 payload bytes plus 6,400 exact retirement-header bytes; Half owns
207,360,000 plus 6,400. Separate cache retention costs are 829,653,200 and 207,573,200 bytes and
overlap the owned payload, so these measures must not be added. Two hundred warm forward/reverse
Current observations add no preparation, graph work, dispatch, transfer or owned allocation.
Final phase and owned claims return to zero before actual retirement exit and collection; relevant
core/synchronization validation is clean. Fill intervals are 21.610 s Full and 21.533 s Half;
complete helper runs are 68.949 s and 65.022 s. These are bounded preparation/parity/reuse checks,
with largest-child peak RSS 1,945,460/1,203,524 KiB and separately reported cgroup peaks 2G/1.3G.
RSS, cgroup usage and rooted payload claims are distinct, not physical GPU-memory totals. These
checks do not establish painted FPS, sustained playback or SMB/server-cold throughput. Source read/decode bytes remain
unmeasured, and digest/header verification warms file caches.

Earlier attempts remain failed evidence: a missing timing utility prevented launch; malformed
external file URIs yielded CPU output with 40/100 retained frames rather than a GPU capacity result; incoherent
stage-only fixture sampling produced AwaitingCapacity despite ample bytes. Correct file URIs and
the existing production off-UI sampler repair the fixture, without changing production policy.
The successful Half run precedes only a fixture declaration-order correction for exceptional
fallback teardown; final Full and the clean one-TU strict check use that corrected order.
Repository quality passes with 1,459 format checks. Only two test files and owning evidence docs
change; production binaries are unchanged. Larger native-resolution footage, native allocation
ownership, media reuse, width two and sustained/platform qualification remain pending.

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

On a device-loss or unavailable fallback, final output continues on the CPU reference path only when
its immutable request and policy allow the CPU equivalent; otherwise it fails. It never resumes
partway through an output with mixed hidden semantics.

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

### Conservative processing-phase reservations (first G6B slice)

The first G6B slice introduces a neutral coordinator explicitly injected by application, export
and scripting composition roots. Owners sharing an allowance must receive the same coordinator;
independent roots do not implicitly aggregate their budgets. Missing roots refuse before heavy
work. Host preparation, native execution and final transfer hold checked host/device byte envelopes
and request-count leases before entering each phase. Shared allowance never grants access to
another native owner, device generation or cache. Mutable frame-context width remains one.

These are conservative phase ceilings, not unique allocation charges. Preparation accounts for
bounded source/coverage quotas and nested borrowing; final output accounts for process, encoded
output and readback costs. Budget refusal is typed and cannot trigger an unadmitted CPU path.
Disabled or unavailable GPU providers still require host admission before CPU evaluation. Existing
GPU coverage, parity and strict final-output behavior remain required under the same policy.

Final-evaluator producer budgets distinguish defaults from explicit overrides. An explicit
`executorBudgets` value is honored verbatim. A rooted default uses `requestByteBudget` as its
producer image upper ceiling, rather than capturing the startup reusable-cache split; each request
is still constrained by its admitted executor allowance, live pins and native resource limits.
Unrooted legacy defaults remain unchanged. This does not enlarge an admitted phase or guarantee
that a composition fits.

#### Preview default propagation follow-on

The implemented follow-on distinguishes omitted preview limits from explicit finite ceilings.
`GpuPreviewDisplayServiceOptions::residentExecutorBudgets` is optional: an explicit value preserves
its byte and structural limits verbatim. An omitted rooted producer uses the configured request
ceiling, or the device-owner ceiling when the request default is delivered live, rather than a cold
reusable-cache snapshot. Actual execution still clamps each request to its admitted phase, unique
live pins and native resource limits. Omitted unrooted producer budgets retain legacy behavior.

The application explicitly shares one neutral `TaskBudgetView` between Tasks and its processing
policy. The existing off-UI sampler delivers host/process state; the root resolves configured
preview/operation shares against the delivered live processing allowance, then publishes an
immutable numeric snapshot after quota locks are released. Reusable-cache credit is not processing
headroom. Omitted scheduler aggregate admission, service request allowance, controller host-pixel
limit and preparation executor envelope read these delivered defaults instead of latching startup
values. Each request remains subject to real phase and device admission; a snapshot is neither
allocation ownership nor a new independent allowance.

Cold, failed, future-dated or stale default delivery closes new default admission with typed
AwaitingCapacity. A fresh sample epoch can retry the newest eligible request through existing
completion/sample notifications. Its sample serial is distinct from the phase release revision;
own cleanup cannot manufacture a new sample or create a retry loop. Explicit finite limits remain
explicit and are never enlarged by default delivery. Owners without a view retain legacy defaults.
Already admitted bytes and live pins remain held when capacity falls; new work must fit the reduced
capacity. Saved settings, requested shares and effective constraints remain distinct.

The packed transfer ceiling remains 320 MiB for concurrent staging plus host capacity. Actual native
source allocation is deducted separately from request headroom before transfer admission; the
transfer ceiling is not a Float32 source/executor limit. This follow-on adds no native allocation,
UMA, whole-process or OOM guarantee, batching or additional frame context.

A terminal executor `Failure` with `OverBudget` may now supply positive retirement proof after an
earlier native prefix finishes: no device loss, unretired submission or required owner drain may
remain. Preview additionally checks actual Neutral/general display and packed transfer submissions;
final rendering checks output-colour work. Only this proved path releases the touched phase before
fallback/recovery. Unknown work and the device's independent unproven latch remain charged; budget
refusal, cancellation or logical terminality alone does not clear them.
An independent legacy Neutral service post-submit fault confirms that one phase and one unproven
claim remain held until allocator-generation teardown. It does not execute the new resident
OverBudget branch; the explicit-cap refusal/recovery witness and reviewed downstream guards cover
that proved-retirement path separately.

Configured prior 23-member B Full/Half witnesses and explicit-cap refusal/recovery are recorded in
the [owning plan](../plans/gpu-reliability-and-preview.md). They use explicit fixture allowances and
do not prove ordinary omitted defaults. Separate ordinary-default Full/Half witnesses pass through
the application's saved-settings helper and sampled numeric delivery without request/scheduler/
producer overrides, retaining explicit 2 GiB cache and 1 GiB oracle allowances. Affected owning,
246-unit strict, desktop build, 20 Linux no-Vulkan fallback and final repository-quality gates pass.
This qualifies the bounded default-propagation and proved-retirement companion; native allocation
ownership, full-GUI/sustained performance and broader platform execution remain open. The accepted
first phase checkpoint and mounted-footage A evidence are unchanged.

Native work keeps its lease until exact retirement, including cancellation, publication failure
and retained/quarantined submission ownership. A logical terminal result or timeout is not fence
proof. A final executor's terminal `Failure`/`OverBudget` may release its phase only when no device
loss, unretired executor submission, owner drain or output-colour submission remains. The device's
independent unproven-submission latch still prevents release; an unknown failure supplies no such
proof. Capacity updates preserve held leases/counts; measured zero refuses new admission. Bounded
readiness and external-capacity-release retries retain one active/newest intent, avoid retrying on
own disposal, cancel safely on shutdown, and preserve warm exact-cache reuse when no new phase is
needed. They do not create a polling loop or silently move GPU work to CPU.

The sampled host policy may release a previously observed constraint only using fresh complete
paired data in the same verified visible view and explicit removal for every remembered finite
domain. It does not prove hidden limits absent; the exact proof and reusable-retention recovery
remain owned by [evaluation primitives](evaluation-primitives.md).

Independent source/lifetime reviews and integrated validation accept this bounded first slice.
Required native coverage passes 56/56 with no missing fixtures and six production route proofs.
The final native retry witness, full 6000x4000 output parity, explicit-cap refusal/retirement and
sticky unknown-retirement checks pass. Affected strict checks close 195 translation units through
193 initial passes and two repaired fixture reruns; application quality and Linux CPU-only builds
and tests also pass. The reliability plan distinguishes initial failures from corrected evidence.
Published
products, cache aliases, external pins, later presentation, UMA backing, opaque metadata and driver
residuals still need allocation-owned accounting. This slice establishes no aggregate OOM guarantee,
width-two execution, sustained throughput, or native macOS/Windows qualification. The
[GPU reliability plan](../plans/gpu-reliability-and-preview.md) owns the remaining gates.

### Rooted native image suballocation ownership (bounded two-family slice)

The implemented native follow-on covers explicitly rooted backing created by `createResidentImage`
and `createDisplayImage`: native RGBA32F process images and RGBA8 display images. Each opaque backing
implementation contains one move-only `NativeImageStorageToken`; aliases share that token. Legacy
unrooted creation remains unowned by this ledger. No new image/frame cap or reclamation queue is
introduced; existing owner-slot and native resource bounds remain in force.

Creation reserves its existing owner slot and creates an unbound image, then queries that image's
requirements and dedicated-allocation flags. VMA ranks compatible types using the existing
AUTO_PREFER_DEVICE preferences. Each admitted allocation uses UNKNOWN with one pinned type bit;
failed candidate allocation clears that bit and retries the remaining ranking, stopping at mask
zero or the physical memory-type count. Size/type/heap must reconcile downward to the actual
allocation before bind/view; there is no allocate-first top-up. Bind/view/reconciliation failure
destroys actual backing before claim undo rather than silently trying another type.
Allocation errors alone prove neither device health nor retirement of other submitted work.

Under one neutral root lock, the token transfers requirements bytes from the phase's remaining
native debit. A heap without DEVICE_LOCAL additionally acquires a separately bounded host backing
claim against delivered host capacity and the immutable native envelope, without borrowing the
source/packed-host grant. Both domains admit before a VMA backing attempt. HOST_VISIBLE on a
DEVICE_LOCAL BAR heap alone does not imply this host claim; UMA physical overlap remains unknown.
The runtime supplies the original sample's validity deadline. Expired delivery closes new
supplemental host admission without erasing held backing, and fresh same-size delivery can reopen
readiness. A new timestamp cannot manufacture freshness for an old sample.

`ownedNativeImageBytes`/`ownedNativeImages` count covered suballocations once;
`ownedNonlocalNativeHostBytes` is an overlapping host subset. `totalDeviceClaimBytes` and
`totalHostClaimBytes` include their respective phase remainders and covered owners. These are
admission constraints, not additive physical totals. Actual `VmaAllocationInfo::size` describes a
suballocation, not committed VMA blocks, driver/RSS, opaque metadata or allocator overhead. OCIO
texture arrays, WSI images and other allocation families remain outside this ownership slice.

Cache/output retention promotes only the retained image. Promotion drops its strong phase alias
outside the lock and keeps a weak origin for turnover: actual backing destruction may replenish
that still-live preadmitted envelope, while an expired origin releases credit normally. Aliases do
not pin the request indefinitely. Failed-prefix foldback requires proof of destroyed backing,
exact origin and the last internal alias with no external/cache holder. It cannot fabricate an
independent release revision or hide a genuine independent release. Ordinary published disposal
retains its release revision; this does not add an immediate UI wake channel. Typed refusals preserve
exact phase/domain and current refusal watermark. The genuine general-controller witness preserves
last-good output after an actual native allocation refusal, ignores unchanged-revision pulses and
recovers the newest intent once after an independent native cache release. Its ordinary retained-frame
return adds no preparation, graph, dispatch or native owned bytes/count. Final transfer/identity and
rooted general-display/Viewer precision checks also pass: cached return preserves exact identity
and adds no packed transfer, the general display matrix matches its CPU pixel oracle, and actual
Viewer/cache ownership retires covered native backing to zero.

Tokens survive existing foreign orphan and unknown quarantine paths with their exact allocator
generation. Actual native destruction precedes release; owner close or enqueue is not destruction.
The host reclaimer is not a native owner. The real UnknownFence witness retains covered token
bytes/count through actual fence/owner retirement while holding its admitted phase lease.
Independently, existing unproven phase protections stay sticky where engaged; this witness does not
assert those latch counters. A private unproved-flag control is distinct from submitted-work evidence.

Incremental neutral, genuine two-family allocation/nonlocal/fault/alias, submitted-fence and OCIO
pixel/cache witnesses pass, as detailed in the
[owning plan](../plans/gpu-reliability-and-preview.md). Corrected whole general-display and packed
helpers also pass with explicit root/retirer/phase-lease prerequisites; their earlier fixture
failures remain separate from production behavior. Required-route and all 323 owned desktop strict
checks are covered with their split-run history. Linux CPU-stub fallback covers 16 distinct strict
consumers and 17 passing owning checks plus one honest unavailable-tools skip; actual GPU routes
are qualified separately. Final application/CLI/media identity and dependency checks and repository
quality pass. Verified packaging and normal-app replacement follow this checkpoint. This does not
qualify all native allocations, block commitment, whole-process/UMA/OOM accounting, sustained
performance or native macOS/Windows execution.

### Rooted OCIO sampled-LUT image ownership (bounded LUT slice)

The OCIO follow-on extends the existing native image token to explicitly rooted sampled 1D, 2D and
3D LUT images. Program creation receives the exact phase lease already retained by the device,
before job begin; an equal numeric phase id from another root is insufficient. Immutable neutral
root identity is recorded even for zero-LUT programs without pinning a phase or native owner.
Both runtime program caches check that root, device epoch and native allocator affinity before
cold creation or warm reuse. Unproven authority is refused, with no second phase or quota grant.

Each move-only sampled resource carries the same token from temporary image ownership through
program retention or upload quarantine. Actual same-image requirements and dedicated flags feed
the existing ranked, pinned VMA allocation path. Native bytes transfer from the admitted phase;
actual nonlocal heap backing also claims the bounded, fresh same-root host subset before allocation.
HOST_VISIBLE device-local BAR memory is not classified as nonlocal. Actual VMA size/type/heap
reconcile before bind/view, and destruction precedes credit. These LUT images contribute to the
existing native-image counters; staging, uniform/status/packed buffers, sample vectors, samplers,
views and driver/allocator-block overhead remain outside allocation-owned image bytes.

Both runtime caches promote sampled storage only after successful cache insertion and bookkeeping.
Failed insertion or controlled partial promotion destroys the exact backing and folds the attempt's
own prefix back without fabricating an independent release revision. Warm reuse under a new phase
retains the same published image charge, rather than pinning the original phase. Rootless legacy
programs remain outside this accounting.

An intrusive program registration precedes native resources. Foreign final release marks its
preowned node without allocating or calling the driver. The exact allocator generation and native
owner drain after actual fence proof, outside the registry lock; native destruction precedes
unregistration. Live aliases and exact-generation upload quarantine keep preview/final exit
incomplete. Closing or timing out an owner is not retirement. The packed-host worker is not this
native owner, and no autonomous cleanup tail is introduced.

The authority and registry checkpoints pass separately from actual LUT backing evidence. Genuine
native fixtures now cover dimensional/heap identity, pre-backing refusal and faults, compatible
nonlocal freshness, forced RGBA format compatibility with unchanged HDR/3D CPU pixel oracles,
required two-texture prefix cleanup, aliases/foreign drain and both textured runtime caches.
Submitted display work with a fault-overridden unknown fence observation retains charges until
actual owner fence retirement; this does not demonstrate an actual GPU hang. Submitted upload
cancellation is a separate quarantine witness. Their exact attribution is recorded in the
[owning plan](../plans/gpu-reliability-and-preview.md). Nine remaining selected owners pass
separately from those direct native/cache proofs. All six required production writers and their
fourteen-check route gate pass; all 324 desktop owned-TU strict checks close through a split prefix
and disjoint remainder. The fresh Linux CPU-stub build and selected fallback tests pass with two
explicit unavailable checks skipped. All fifteen distinct fallback strict consumers pass without
diagnostics. Final repository quality passes after the six-file formatting-only repair, and the
source-aligned application/CLI/media identity readback has zero missing dependencies. Exact
integrated review, selective commit and verified packaging follow this bounded checkpoint.
No buffer/WSI/external,
whole-native, block/physical/UMA/OOM, sustained-performance or other-platform qualification is
adopted by this incremental checkpoint.

### Rooted display-transfer staging buffer ownership (bounded staging slice)

The first buffer follow-on adopts only the transient staging backing of `GpuDisplayTransfer`.
`NativeBufferStorageToken` moves with that backing into the existing native transfer slot and
quarantine. `ownedNativeBufferBytes`, `ownedNativeBuffers` and the overlapping
`ownedNonlocalNativeBufferHostBytes` distinguish these suballocations from owned images and packed
host payloads. They do not measure committed VMA blocks, driver overhead or process physical memory.

The rooted path creates one actual unbound buffer, queries its requirements and dedicated flags,
and preserves the original compatible-type restrictions while VMA ranks a shrinking candidate mask.
A pinned allocation retains the original random-access/mapped flags and explicitly requires
HOST_VISIBLE. Actual allocation size, type, heap, allocator epoch and mapped pointer are checked
before submission. HOST_VISIBLE on a DEVICE_LOCAL heap is not classified as nonlocal host backing;
physical overlap on UMA remains unknown. Unrooted buffers retain their legacy unowned path.

Native bytes and, for a nonlocal heap, already-planned host staging bytes transfer atomically from
the exact retained phase. Both total claims remain unchanged. This is conversion of an admitted
working grant, not the image token's additive nonlocal host claim or a new sample-dependent grant.
The declared staging-plus-output transfer allowance remains independent of the source image grant.
Insufficient paired debit refuses before a VMA backing attempt, although the unbound buffer may
already exist. Actual backing destruction precedes token foldback; its final phase alias drops
outside the ledger mutex. No cache promotion, extra queue or allocation/frame-count cap is added.

Foreign final release of a Pending transfer hands its preowned resources, token and pixels to the same slot
without allocation or driver work. A live Reserved slot is not reclaimable. Quarantined and Retiring
states remain pending for the exact generation through actual fence polling and destruction outside
the handoff lock; wrong-owner ticks do nothing. A closed owner's late handoff remains incomplete
rather than being reported as reclaimed. Preview and final owner exit now check this pending
transfer before acknowledging device/report retirement. Proven buffer refusal metadata uses the
existing typed phase-release retry boundary; an error code alone is not retirement proof.

Actual native identity, fault, nonlocal conversion, foreign handoff and same-generation recovery
checks pass. The production submitted-sticky witness separately retains buffer, phase and unproven
charges through an incomplete shutdown wait, then reaches covered zero through actual owner and
generation retirement. These privately overridden observations are not driver hangs, and the
independent unknown-report latch is not cleared. Neutral/General Full/Half/ROI CPU pixel parity,
retained aliases and cancellation/detach checks also pass. The actual device helper, three UI
preparation/control owners and genuine packed QWidget Viewer pass separately; UI controls alone
are not native pixel proof. All six required production writers pass the fresh fourteen-check
route gate. All 325 desktop owned-TU strict checks pass without diagnostics or repairs through
five prefix units and 320 disjoint consumers. The fresh Linux CPU-stub application/CLI/media and
eight selected fallback owners build and pass with explicit native-unavailable provenance. All
sixteen distinct fallback strict checks pass without diagnostics. The first final-quality run
passes all repository checks, and the source-aligned application/CLI/media identities pass readback
with zero missing dependencies. Exact integrated review, selective commit and verified packaging
follow the bounded qualification recorded in the
[owning plan](../plans/gpu-reliability-and-preview.md). Other upload, process, final/readback and
staging buffer creators remain unowned by this slice. No whole-buffer/native, physical/UMA/OOM,
performance or other-platform execution qualification is adopted here.

### Rooted combined final-readback staging (bounded final-output slice)

The final-output increment adopts the transient staging buffer used by
`GpuOutputColorReadback` for the process payload and, optionally, one encoded RGBA32F or RGBA8
payload. One submission still transfers the composited output at the final host boundary. The
existing `FinalProcessingPhase` lease now travels explicitly through the output-colour stage and
readback begin; it must match the exact proven phase retained by the device, root and generation.
An omitted lease is not authority to allocate against an unrelated rooted device phase.

The same buffer helper and `NativeBufferStorageToken` convert remaining admitted native and
already-planned nonlocal host staging debit once. Source images, CPU vectors and builder overlap
remain separately reserved. Actual requirements, mapped type/heap/size and the staging-plus-host
transfer ceiling still constrain allocation; insufficient paired debit refuses before a VMA backing
attempt, without adding a grant. The token follows staging through the existing readback slot;
actual backing destruction precedes foldback. These bytes use the existing buffer counters, not
CPU output ownership or a second native-image count.

Foreign final release of a Pending readback hands its existing resources to the native owner without
allocating or calling the driver. Reserved remains live; exact-generation Retiring remains pending
through fence proof and destruction outside the handoff mutex. Preview/final exit includes this
readback pending state. Proven budget refusal drops attempt-owned aliases before phase retirement,
allowing a second request at capacity one; loss or unknown work is not that proof. No timeout or
closed-owner state is reported as reclaimed success.

Focused native helper and complete evaluator evidence qualifies this behavior independently.
The evaluator's real submitted work with a privately overridden unknown observation retains buffer,
phase and unproven charges through an incomplete shutdown wait; actual native/generation teardown
reaches covered zero while the independent unknown device-report latch remains untouched. This is
not an actual GPU hang. The six-unit focused strict closure includes one fixture-reporting repair.
The [owning plan](../plans/gpu-reliability-and-preview.md) records exact attribution. The focused
readback checkpoint did not own CPU output allocations; the final-host follow-on below adopts them
separately. Upload staging remains excluded. These increments share one final-output union
qualification, commit and package; neither is a separately deployed milestone.
Other buffers, legacy/unrooted storage, committed blocks/driver/process/UMA accounting, OOM,
performance and other-platform qualification remain outside this scope.

### Rooted final CPU output ownership (bounded final-output slice)

`FinalPixelStorage<Rgba32f>` and `FinalPixelStorage<Rgba8>` distinguish explicitly rooted final
payloads from legacy vector storage. Process readback, the distinct process-image builder copy,
immutable image freeze and encoded output moves preserve their exact allocation authority.
`ownedFinalProcessHostBytes`, `ownedFinalEncodedHostBytes`, `ownedFinalHostMetadataBytes` and
`ownedFinalHostAllocations` record allocator-requested capacity and the admitted intrusive header,
not malloc/driver overhead. Old CPU builders and decoded/cache storage remain explicitly unowned;
a rooted allocation refusal never switches silently to legacy storage.

The existing final phase's conservative `48 * pixels + 2 * encodedBytes` overlap and metadata
reservation fund these allocations without a second allowance. Readback staging, CPU vectors and
builder copies have separate live charges while they overlap. Staging admission also leaves room
for the host headers. Freezing moves the same builder backing; internal takes remain provisional.
Checked direct publication or the complete evaluator's process-plus-encoded terminal-success
handoff validates both authorities before promoting either. Promotion precedes mailbox acceptance;
later cancellation or abandoned delivery may still discard the complete published product.
Changing a domain/root during moves or publishing only one half of a failed product is refused.

Published process and encoded storage detach from the originating phase and outlive native device
teardown. Their final release uses the existing explicit per-root host worker: allocation-free
handoff retains the payload/header charges until actual worker free, without a new queue, frame cap
or detached cleanup tail. Provisional cleanup remains synchronous on the supported worker/owner
path; foreign Pending native handoff is separate from provisional Ready disposal. Host shutdown
remains incomplete while final output pins survive and requires actual worker exit then collection.

Neutral allocation and actual production consumers pass their focused checks. A real encoded GPU
output witness preserves every-pixel process/encoded oracles, retains the published host products
with phase count zero through native generation teardown, then reaches covered zero after foreign
last-owner release and actual host-worker free. Exact gates and earlier failed consumer builds are
recorded in the [owning plan](../plans/gpu-reliability-and-preview.md). Twenty remaining owning tests
and a fresh fourteen-test required-route chain pass; the earlier still/video fixture shutdown
failures remain separately recorded. The 385 affected desktop strict checks close through reusable
prefixes, clean disjoint checks and one exact fixture arithmetic repair; original failures are
preserved. Fresh Linux CPU-stub tests and sixteen distinct fallback strict checks pass. Final
repository quality and source-aligned application/CLI/media artifact checks pass. Exact independent
source/doc freeze review precedes selective commit and immutable packaging.
This covers rooted final process/encoded payload and header
ownership only; upload, legacy CPU/decode/cache storage, other buffers/WSI, allocator
blocks/driver/process/UMA accounting, OOM,
performance and other-platform execution qualification remain separate.

### Rooted image-upload staging (bounded staging slice)

The next buffer increment covers only `GpuImageUpload` transient mapped staging. The existing
phase estimate stays `F + 3L`: fixed output overlap F, protected retained-source estimate L,
scratch estimate L and separately occupied upload allowance U=L. Ordinary owned host output uses
F only. `NativeBufferStorageToken::reserveUpload` consumes remaining U and native debit atomically
on every heap, including device-local/BAR staging. Nonlocal backing converts the same already-
planned host subset once; local backing leaves that conservative host estimate reserved. Actual
requirements beyond U refuse before a VMA backing attempt. Reconciliation and actual destruction
return the exact originating occupancy without adding a grant or manufacturing a release wake.
The source/scratch terms remain estimates, not ownership of decoded/prepared legacy cache images.

The token moves with `UploadStagingBuffer`, preserving sequential-write, mapped, HOST_VISIBLE
requirements and actual allocation/type/heap identity. Native destruction precedes token credit.
The existing native owner slot retains foreign or unproven work, rather than passing it to the
host-pixel retirement worker or publishing transient staging into a cache. Native pending-generation
exit wiring is part of this increment. The complete production evaluator submits a real upload,
then a controlled status observation retains 96 staging bytes, one allocation, the same nonlocal
subset and one phase/unproven charge through a 50 ms incomplete shutdown. Removing only that
observation override permits actual owner fence/backing/generation retirement and covered zero.
The independent unknown device-report latch is neither cleared nor required to reach zero.

Focused neutral and primitive checks pass. Real mapped upload, flush, paired refusal, controlled
faults, foreign owner drain and same-generation reuse are qualified independently from the whole
executor's cache/phase-detachment and capacity-one recovery checks. A manually unproven submitted
primitive control is not production evaluator shutdown evidence or a driver hang. The owning plan
records exact attribution and earlier fixture failures. The held 28-source checkpoint adds only
a Viewer readiness fixture correction to the accepted upload increment. Fresh required routes pass.
The first private Viewer whole run failed gesture/mapping/document assertions while shutdown
assertions passed; a corrected whole run preserves those assertions and passes after an actual
mapping-ready setup wait. This qualifies post-readiness functionality, not rapid selection-to-drag
latency. The 372 affected desktop strict checks close without diagnostics through eight representative
checks, 363 unchanged consumers and the final exact Viewer check. Fresh Linux CPU-stub tests
pass 10/10 with explicit unavailable provenance, and sixteen distinct fallback strict checks pass
without diagnostics. Final repository quality passes on its first attempt, including 1,477 format
checks and all unchanged owning checks. Source-aligned application, CLI and media-worker hashes/
BuildIDs are held with no missing dependencies or linked Vulkan. The 28 source paths and two
owning docs form this bounded checkpoint; exact independent freeze review precedes selective
commit and immutable packaging. Prepared-
upload retention defaults are unchanged and need the next media/cache milestone. Other buffers,
legacy storage, WSI, committed blocks/driver/process/UMA accounting, OOM, sustained performance
and other-platform execution are outside this proof.

### Shared prepared HOST retention (bounded source reuse)

`GpuPreparedUploadCache` holds HOST `Rgba32fImage` references, not native GPU resources. Its rooted
facade now uses the existing `OperationCache` registration and combined LRU with a distinct
`PreparedUpload` key namespace. It owns no second image LRU or independent budget. Prepared pixel
payload and per-key metadata are charged once per key in the combined retention total; distinct
keys aliasing one image may conservatively charge it more than once. This is logical retention,
not new allocator ownership or physical deduplication. Existing video-memory retention stays a
separate participant; Tasks processing allowance is not persistent-cache capacity.

Omitted shared limits follow the same ledger's delivered reusable assignment rather than a cold
unrelated default or fixed one-sixteenth share. The shared entry count is memory-only, with
metadata admitted; explicit finite/zero byte and count caps remain additional subset ceilings,
and numeric standalone legacy controls retain their fixed behavior. First binding fixes the exact
retirement owner and subset policy; compatible default facades attach without changing it.
Cold, stale, failed or backward delivery refuses new optional stores, while existing validated
hits remain usable. Stores enforce the accepted 15-second retention deadline without taking the
ledger lock under the cache lock. Shrink/zero selects victims using projected retirement but keeps
all live and pending bytes charged until callback reference release. Same-key/same-image stores
preserve the retained allocation rather than replacing it.

Each prepared entry registers a preowned generic callback with the existing explicitly injected
host retirement owner. Eviction detaches and enqueues without large destruction or a new worker;
that owner releases references outside cache/ledger/UI locks before returning the logical charge.
External legacy image aliases remain valid, so releasing a cache reference is not proof of physical
backing free. `closePreparedUploads()` irreversibly rejects future stores. GUI preview/final contexts
share the common pool/owner and close prepared production before host shutdown. `fromEvaluator`
attaches only an already bound pool; an unbound evaluator renders without optional prepared stores.
Owned headless providers use a private sampled ledger and pool whose deleter retains that ledger
through unregistration. Context callbacks retain the bundle without a provider-State cycle;
borrowed policies keep the application-owned pool usable. Transactional provider initialization
constructs throwing state before publication, so a failed first setup can retry safely.

Independent focused review accepts the six-path core and eighteen-path consumer watermarks, not
completion of G5. Core owning checks pass 2/2 in 0.17 s; final strict coverage and actual
ASAN/UBSAN/LSAN pass, including a 4,100-entry memory-only default regression. The corrected projected
victim test preserves unrelated warm entries while pending refs stay charged. Consumer whole
native checks pass in 477 ms with a unit-reported 71.4 MiB peak, and final eight strict owners close
through six equivalent checks plus final fixture/provider reruns. Disabled evaluator sessions
prove HOST lifecycle only; enabled mismatch/recovery separately exercises GPU export. Initializer, over-eviction and wrong-loader failures remain distinct history. The intentional
first-context-throw followed by refresh/retry is a passing transactional control, not a failed run.
The source fixture qualifies the first, middle and last of 23 read-only 4608 × 3164 still members.
Isolated transform, text and Full/Half edits change output pixels and dispatch work while weak
HOST and native-source identities remain equal, prepared conversion attempts stay zero and native
source uploads stay zero after cold preparation. Every edited output matches an independent CPU
oracle. A changed production final evaluation uses the same HOST source and passes pixel parity;
its separate native cache is not evidence of final native-upload reuse.

The normal continuously sampled reusable assignment was 19,448,985,600 bytes, without a diagnostic
memory cap. All 23 HOST sources retained 5,365,334,016 payload bytes plus 15,456 per-key metadata
bytes, and survived edited weak-pointer hits. This is HOST retention evidence only; native execution
of all 23 members was not run. Generated controls prove populated-cache purge/repopulation,
bypass/read-only behavior, actual final stale-digest rejection and fresh-digest recovery, and
uncached parity under zero and 128-byte subset caps. These explicit controls are separate from the
normal assignment. Linux sampling reported no finite visible process limit, not a zero allowance.

The current union retains independent source and binary receipts, including the historical B
binary and its unused helper-header change. B has no validation-layer claim; generated native
controls and required routes are checked separately with core/synchronization validation. Earlier
missing-base-directory and hidden-text fixture failures remain recorded, alongside corrected runs.
Final union closure passes twelve owning suites, five refreshed formatted-body owners, the final
guarded generated native fixture, all six required production route writers and a subsequent route
I/O verifier. Fresh Linux Vulkan-disabled qualification passes eight selected suites; the native
source fixture is explicitly Skipped (77). Strict closure covers 345 distinct contexts: 241 desktop
and 104 fresh fallback, with 109 fallback contexts reused only after exact equivalence. Desktop
attribution is 233 unchanged-dependency contexts plus eight repaired owners; both guarded-fixture
contexts are rerun. Historical twenty-then-four optional diagnostics remain recorded. Repository
formatting, hygiene, architecture, UI grammar, schemas and dependency-artifact checks pass. Product
artifacts retain the accepted eighteen-path source snapshot; the additional nine paths are tests
and helpers, with source/binary receipts preserved separately from historical B proof.

Refreshed sanitizer compilation instruments eleven actual core/consumer/test bodies; remaining
linked archives are not claimed as instrumented. Core ASAN/UBSAN/LSAN passes. Native host functional
assertions, core/synchronization validation and ASAN access/UBSAN checks pass, but unsuppressed
LSAN exits nonzero with 2,468 bytes in six allocations, also reproduced without a session bus.
Reported stacks contain libdbus and unknown unloaded callers, with no Bloom frame. External
attribution is plausible but unproven; this unresolved leak result is explicitly accepted as a
scoped shipping limitation, not clean native LSAN.

The narrow source union is the shipping cut; broader G5 and memory/performance work is postponed.

A prepared hit is separate from native-source and finished-output hits. Still source selection
continues to probe/hash before lookup; a raw still hit bypasses the decode call but does not prove
zero verification reads. Video selection may obtain a decoded frame before prepared lookup.
Full/Half may retain the same full-resolution raw source while downstream work changes. Source
identity, bypass/read-only behavior and actual final revalidation remain required; this slice
claims no zero-I/O speedup, sustained throughput, other-platform execution or whole-memory/OOM
accounting. Native image/buffer ownership and their retirement are unchanged separate domains.

### Rooted packed-host payload ownership (bounded ownership slice)

This narrow G6B slice tracks allocator-requested RGBA8 pixel payloads for explicitly rooted
packed preview storage. It does not replace the accepted phase-reservation contract above.
`PackedRgba8Storage` distinguishes its legacy `std::vector` arm from a bounded allocator arm;
`PackedRgba8Ownership` reports `LegacyUnowned`, `RootedProvisional` or `RootedPublished`.
`PreparedReferenceDisplayBuffer::pixelOwnership()` exposes that distinction. Legacy storage remains
unowned by this accounting even when its cache retention cost is known.

The rooted allocator admits each actual allocator request before allocating the payload. During
construction, the allocation keeps its originating phase alive and moves covered bytes from the
phase estimate into the owned-payload count under the same budget lock. Failed or discarded
provisional storage folds that debit back atomically, without manufacturing a capacity-release
revision or a preview retry. The complete product's terminal-success handoff promotes its payload's
lifetime ownership independently of the origin phase, before sink or mailbox acceptance. Later
cancellation, forced terminal delivery or abandonment can still discard that promoted storage.
Aliases, mailbox delivery, cache retention and later external pins retain the same allocation
charge until actual backing storage is destroyed;
dropping one alias is not release proof. The locked ownership handoff leaves the total claim and
admission revision unchanged. Dropping the retained origin outside that lock may release the last
phase alias; that releases its remaining bytes/count and advances the revision normally.

`hostReservedBytes` continues to mean the remaining phase estimate. `ownedPackedHostBytes` and
`ownedPackedHostAllocations` describe only explicitly rooted allocator-requested pixel payloads.
`totalHostClaimBytes` combines that remaining estimate with owned payload bytes for admission,
without counting the covered bytes twice. These counters exclude allocation headers, debug proxies,
malloc bookkeeping, geometry/proof/envelope metadata, legacy vectors and native/driver allocations.
They are neither RSS nor committed allocator blocks, VRAM or UMA accounting. Cache-held product cost
remains a separate retention measure and may include metadata.

Independent review accepts ownership, conservation and off-UI production allocation within this
scope. Neutral core tests and sanitizers, final native Neutral/general Full/Half/ROI parity,
200 warm aliases, allocation/diagnostic double-fault terminal cleanup and cancellation retirement
pass. Actual Viewer cached return adds no GPU work, transfer or allocation; purge and external pins
preserve one charge until the last backing owner releases it. Native service-owner teardown does
not prove policy-root destruction because that fixture retains root aliases; neutral core tests
cover root destruction separately. Controller no-spin/recovery evidence is CPU/control evidence
using existing notification/sample pulses, with five-second sampling as the late-release backstop.
It does not establish a new immediate final-alias release wake.

The 51 affected strict translation units close through 47 initial passes and four corrected owning
reruns, plus a final RAM fixture rerun already in that map. Owning tests initially pass 15/16;
the repaired tiny RAM fixture asserts CPU OCIO/CpuReference and zero native dispatch. Linux
no-Vulkan tests initially pass 11/13; the two Qt-theme startup failures pass isolated reruns.
Application and repository quality pass. These corrected batches are distinct evidence, recorded
in the reliability plan. Existing required final-render coverage is unchanged; no final-render
host-payload route, unrooted legacy route, native allocation accounting, whole-process guarantee,
width-two execution or sustained/platform qualification is adopted here.

At the accepted `6acad4d4` ownership checkpoint, published backing still freed synchronously on
the final owner's thread, potentially Qt UI. Its Viewer witness proved ordering and conservation,
not off-UI free or large-free latency. The follow-on below implements that required retirement
repair; its affected qualification gates pass within the bounded scope below. The historical checkpoint is not an engineering-rule
waiver or a claim that legacy storage retires off UI.

### Off-UI rooted published-host retirement (bounded retirement slice)

The follow-on covers explicitly rooted published RGBA8 backing and its exact intrusive allocation
header/node. Admission transfers payload and header bytes from the phase estimate before allocation.
`ownedPackedHostBytes` remains payload-only; `ownedPackedHostMetadataBytes` counts the header/node;
`ownedPackedHostAllocations` counts backings. `totalHostClaimBytes` now combines remaining phase,
payload and header claims without duplication. Queue metadata is bounded by admitted bytes, with no
new fixed frame/allocation-count cap. Cache product retention cost remains separate. Debug/authority
proxies, malloc/stdlib/thread/stack overhead, other product metadata, legacy vectors, final RGBA32F
and native/driver/UMA backing remain outside this claim; the counters are not RSS or committed blocks.

Provisional cleanup remains synchronous on the admitted attempt worker, folding payload and header
back together before the final origin-phase release. The complete product's terminal-success
handoff still precedes sink/mailbox acceptance. Dropping the last RootedPublished alias performs
an O(1) intrusive enqueue/notify, without new allocation, backing free or ledger credit. The worker
frees backing outside the queue mutex and only then releases payload/header claims and registration.
Live, queued and in-progress backing stays charged until actual free. This does not move legacy
vector or every host-buffer release off UI, or establish a hard real-time/sustained latency bound.

An explicit joinable per-root `PackedHostRetirementOwner` is created inert; worker-only preparation
registers actual at-thread-exit acknowledgement before admitting backing. Tokens retain neutral
producer state, not the lifecycle owner or device. The frontend retains its owner through producer
stop, pin release, close, actual exit and collection. `beginShutdown()` closes registration/notifies;
live aliases keep shutdown incomplete. `exitAcknowledged()` polls without joining; `collect()` joins
only off an active UI loop or after that loop returns, outside the polling mutex. Destruction neither
joins nor detaches; normal exception/unwind requires the explicit lifecycle guard.

GUI shutdown first reaches task/native/export prerequisites, stops preview producers, releases live
Viewer/current/cache pins and closes the owner. Already-posted outgoing-editor deleteLater pins may
remain; their originating loop destroys them while charge/exit remain pending. Post-loop or unwind
teardown drains only already-posted safe deletion before borrowed dependencies die. Active shutdown
polls actual exit; collection follows event processing. A timeout is not quiescence.

Owned headless providers preserve worker-sampled roots; injected application roots remain borrowed.
Python uses module/interpreter-owned light records, shared-provider root identity and separate
per-Host schedulers. Early GC retains closing provider/native state before Host release; records
retain no Host, Session or Python object. Close rejects new calls, cancels native admissions, joins
Python producers and drops only runtime-owned references while Python is alive. Native/host waits
and collection release the GIL and lifecycle locks. Incomplete shutdown retains provider/owner/
scheduler and forbids finalize or dependency unload. CLI guards borrowed evaluator/scheduler
lifetimes through normal return and unwind. The owning scripting/output documents detail these
boundaries. One interpreter-owned cancellation-aware gate serializes owned native bootstrap only;
borrowed app providers and frame execution remain independent.

Core and GUI increments, actual held-free Viewer/GIL witnesses and the unchanged ordinary mixed-ICD
Python API with three repeats pass their focused gates. The repaired module boundary forwards the
same neutral owner through runtime's public surface, without duplicate state or checker exception.
Final owning tests close via an initial 19/20 plus a separate corrected controller prerequisite;
that controller witness is CPU/control evidence. Linux CPU-only/Python-enabled app, CLI and module builds pass, with all twenty affected
root/frontend suites passing in 9.93 s. Native branches there are unavailable, never GPU passes.
All 103 changed-header production/owning translation units pass the final strict sweep without
diagnostics or repair reruns. The reliability plan preserves exact split-run history.
No native allocation accounting, width-two, aggregate OOM or sustained/platform qualification is
adopted here.

### Resident request cancellation and failure retirement (G2E)

Independently requested cancellation or supersession discards publication, while the admitted stage,
its completion and native pins remain owned until exact submitted-work retirement. Expected
executor/display cancellation does not increment the resident backend-failure counter or replace
its error detail. A concurrent unexpected poll result or native failure still counts and reports
both the actual poll result and the underlying diagnostic. Actual native device loss takes
precedence over cancellation or an expired deadline. Scene, Neutral display and General OCIO
display loss participate in the same terminal-route decision; cancellation reaches all three arms.

A display's logical Failure is not fence proof. The resident service checks its exact outstanding
submission before clearing the active stage or consuming completion. The General OCIO native
program and runtime wrapper continue owner-thread fence polling after logical failure solely to
retire that submission. A successful later fence preserves the original failure and publishes no
pixels; actual driver loss preserves explicit loss truth. Healthy reuse is admitted only after the
outstanding submission clears. Repeated retirement polls do not count another job failure.

Current owner-drain truth is per executor and exact submission. PathCoverage's static historical
teardown fuse remains latched for audit and is not the service's current outstanding-work test.
The unchanged resident terminal policy permits 512 owner pumps before pipeline destruction invokes
its native bounded drain/quarantine. Exact native family ownership can remain retained afterward,
but the G2E checkpoint alone did not transfer its request charge into an aggregate quarantine
reservation. The first G6B phase slice above extends reservation lifetime without claiming unique
allocation-owned retained-byte accounting. Native stores and residual lifetime inventory remain G6
work; historical teardown counters do not establish hard physical reservation ceilings.

EditorArea replacement and workspace mutation continue requiring the existing WSI safe-to-mutate
proof. An unsafe result leaves the outgoing owners and mapped native surface intact. Compute fence
retirement is never substituted for that presentation proof.

### Oracle readback retirement (G2D2)

The full process/display and sparse display pixel oracles share exactly two process-wide native
submission reservations, with at most one reservation per exact allocator owner. Admission precedes
native allocation and refuses recoverably when occupied. This preserves the former aggregate
quarantine ceiling; it adds no production preview readback. Requested transfer bytes and actual VMA
staging allocation bytes must both fit the caller's byte budget, including sparse transfers whose
native allocation can exceed their payload. Private diagnostics charge actual staging bytes until
native destruction; this budget does not describe the subsequently allocated host pixel vector.

An unknown completion retains command resources, staging, allocator ownership, and the exact source
implementation's pre-reserved image slot. A synchronized source pin and a monotonic oracle token
prevent source destruction and stale proof reuse. Only the original allocator and thread lifetime
may query retirement. A real signalled fence clears that exact pin after native command children,
pool, fence and staging destruction; actual driver device loss permits cleanup and invalidates the
source, without publishing pixels. A timeout never proves loss or successful pixels.

Readback retry polls retirement before checking the source pin. The normal image-owner pump also
polls pending oracles before reclaiming released images, including a caller's foreign final release.
An allocator with zero pending oracles avoids scanning or locking the oracle store. Facade teardown
performs this drain before closing its owner lifecycle. Closed owners never reopen or allocate;
unproved submissions from an exited or closed owner remain charged within the same two-slot ceiling.
The asynchronous production final-output transfers keep their separate ownership and cancellation
contracts.

### Image owner reclamation (G2C)

Each native RGBA32F process image and RGBA8 display image reserves one owner-reclamation slot
before its first Vulkan allocation. Each image family has a process-wide ceiling of 4096 slots,
chosen from the existing cache/lease entry ceilings; it does not guarantee that every maximum-size
scene fits and does not increase any byte budget. Exhausted admission refuses image creation
without a native allocation and recovers when the exact owner destroys retiring images.

A foreign-thread final release or move-assignment replacement marks that image's already-owned
slot retiring without allocating. The slot preserves the image implementation, co-owned allocator,
and actual VMA allocation byte count through native destruction. Owner drains use the exact
allocator state and a monotonic thread-lifetime token together with the thread ID, so neither a
second device nor a recycled thread ID can reclaim the image. Preview pumps and teardown, offline
scene polling, and device facade teardown drain these slots; an exact allocator generation with zero pending image releases takes an atomic
fast path, even when unrelated closed generations remain quarantined. The per-image pending marker
is published before its slot becomes retiring and decremented only after native destruction. These image drains require final shared ownership release and retain any explicitly
unproved submission. They do not replace presentation's WSI retirement proof.

Destroying or replacing a device facade closes its reclaim-owner lifecycle after a final drain.
Existing images remain valid and co-own their allocator; their original owner may still release
them normally. New allocation through that closed generation is refused. A subsequent foreign
final release stays in a closed-owner quarantine: it never tears native resources down on another
thread. This is observable through process-wide retiring image counts and native allocation bytes,
even after the facade is gone. Repeated closed generations consume the same fixed stores and
therefore eventually refuse admission instead of growing an unbounded hidden retained set. There
is no automatic recovery once the original owner has stopped pumping. These diagnostics describe
image native allocations only; aggregate resource brokerage remains G6 work.

The synchronous oracle readback global quarantine/fuse and its source-image unproved flag remain
subsequent G2 work. This image slice deliberately preserves such images, charged and bounded,
without clearing an unproved flag from unrelated compute or presentation evidence.

Local native evidence (2026-10-08): both real VMA image families passed moved final ownership,
capacity refusal before allocation and recovery, move replacement, exact owner/device isolation,
thread-lifetime mismatch retention, retained admission/bytes through a deterministic pre-destroy
barrier, production preview pumping, and late closed-generation retention. Unproved-flag checks
assert retention only; source-fence recovery remains the following oracle slice. Composite and
resident-display CPU parity also passed with core and synchronization validation enabled and no
reported errors. These are local native results; cross-platform GPU qualification is not claimed.
Per-device image counts and native allocation bytes are exact; capacity, refusal, and reclaim
counters are process-wide. A bounded capacity refusal retains the existing AllocationFailed
operation result and its distinct cause is observable through the typed admission-refusal counter.

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
