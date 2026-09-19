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

The RAM preview controller now fills its range with a bounded **two-deep** pipeline
(`bloom/ui/ram_preview_pipeline.hpp`), so the next frame's CPU preparation overlaps the previous
frame's display stage instead of running strictly after it. The helper owns exactly the bound
(never more than two frames in flight), the pixels-identity duplicate guard that stops a range
rebase resubmitting a frame, the out-of-order collection of landed results, and cancel-all; the
controller keeps the range cursor, the per-time evaluation snapshot resolution, and the
out-of-order validation against the *current* per-time frame key. No coalescing key is carried, so
the scheduler can never cancel one in-flight frame of the run on account of another. Both in-flight
frames are detached by `cancel()`, `beginShutdown()`, every composition/display/resolution change,
and the bounded budget stop; the run finishes only once every valid result has landed, and a
memory-budget stop keeps the prefix that fits without churning its own beginning. This sits on the
existing `PreviewPreparationSubmitter` seam, so on an enabled device the RAM submissions go through
the same `GpuPreviewDisplayService` the foreground preview uses, and on the CPU path the legacy
scheduler submission is unchanged. Verified locally: the focused pipeline suite (two-worker barrier
proving preparation 2 starts before preparation 1 finishes, out-of-order completion, cancel/shutdown
of both slots, mid-run work-area shrink/expand, finite-edit rescan, and the bounded budget stop) and
the real-service suite, which with the pinned loader reaches `Ready` and proves two CPU stages
overlap under the service's own admission (peak two, qualified processor selected on every admitted
stage) while reporting the gate as pending rather than failing when no device is present. No
whole-application throughput claim is made from the concurrency fixture alone.

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
strict flags. These are rendering primitives only: no scene evaluator selects them here, they are not
composed per layer, and not presented through a viewer, so no performance claim is made here. (A
later slice wires the solid/display primitives into the opt-in `GpuPreviewDisplayService` resident
route described below; that route is not activated by any application surface.)

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

The presentation lane now samples the resident display image into the acquired swapchain. The
Qt-free `bloom/render/gpu_present_image.hpp` describes the display-space background/checkerboard,
the viewer channel remap, an explicit affine destination/source mapping, and an optional
premultiplied RGBA8 overlay; `GpuPresentationTarget::presentImage` takes the strong
`shared_ptr<const GpuDisplayImage>` and presents it, with `renderResidentIntoAcquired` supplying the
fixed sampler pipeline (checked caller parameters rejected `InvalidArgument` before any command is
recorded, same compute/present queue required, UNORM-only attachment, no readback or full-frame
upload). The fragment shader remaps per texel and premultiplies each tap before the bilinear filter,
matching QPainter's `SmoothPixmapTransform`; the attachment format owns channel packing, so no manual
BGRA swizzle remains. Both shaders are pinned by SHA-256 with a manifest binding and a
configure-time glslangValidator/spirv-val regeneration check against the embedded SPIR-V digest. The
presenter caches views/framebuffers against a monotonic swapchain-generation counter rather than raw
handles; the whole presenter is dropped once retirement is proven and before a resize replaces the
swapchain, and the resident input pin and committed view are updated together so a failed
preparation can never leave a view without its strong source pin. A partial device-resource creation
failure never marks the pipeline ready, and a later attempt rebuilds safely. The native offscreen
proof runs on a real device against an independent QPainter oracle and the CPU OCIO oracle (every
pixel for 257x19 and 1280x720, RGBA8/BGRA8, channel/checker/crop/PAR/premultiplied-overlay/
failed-overlay-pin/partial-ready cases), and the real Wayland fixture now drives a resident image
present through a resize-and-two-recreate before presenting and retiring. The CPU-unavailable stub
provides the same API. This remains render-side only: no viewer/service selection, no
whole-application benchmark, and no reference-parity or performance claim.

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
`GpuSceneCommand`s -- solid, unparented translation-only layer, image/video upload, Normal merge,
composition output --
using the evaluator's REAL six-argument preflight resolution and, for a fractional translation-only
solid, the SAME CPU `PathRaster` R8 coverage; it allocates no full RGBA CPU image and fails closed
`Unsupported` for every out-of-subset reachable operation. Command semantic keys carry the resolved
operands plus the pinned render shader SPIR-V digests, including the actual `CoveredSolidV1` digest,
and never node/layer IDs, operation indexes, or the revision; execution-order indexes are not
identity. Time activation is the exact CPU `[inPoint, outPoint)` range test, the request pixel
allowance bounds every command output and each unique coverage raster with overflow-safe arithmetic,
cancellation is checked per command and per raster row, and `GpuSceneCoverageCache` is a bounded,
transactional LRU keyed on raster geometry alone. An `ImageSource`/`VideoSource` leaf resolves and
colour-converts its source on the CPU task thread through the evaluator's own
`selectImageSource`/`evaluateImageSource` and `selectVideoSource`/`videoToSceneLinear` entry points and
publishes a `GpuSceneUploadCommand` carrying the frozen converted image plus a source semantic key
that never contains a node id, plan index, frame time, or layer transform; a reachable unsupported
layer is screened before any decode, and a `GpuPreparedUploadCache` (bounded LRU, 128 MiB / 4096
entries default, zero disables) reuses a converted source across a transform-only change. Media
counters are per-build on the result (or the failure diagnostic), never a shared mutable aggregate,
and an explicit or interactive bypass never reads or writes the disk cache. Verified locally with a
genuine uncached
`CpuCompositionEvaluator` oracle: identity, bounds, output descriptor, and bit-exact pixels for
fractional/integer translations, proxy and non-square PAR, animated opacity/position, multiple
layers, time-activation range/empty/mute-solo, cache reuse, budget refusal, and the unsupported
branches; plus the covered native coverage matrix, fractional `PathRaster` pattern, rejections,
lifetime, and performance, and the ordinary Solid/composite, CPU image-primitive, and CPU
composition-evaluator regressions. The user-facing application is unwired: no service or scheduler
selects a command, so no performance claim is made.

The owner-thread non-blocking GPU scene executor is now implemented on top of that preparation
foundation, still without service, product, viewer, or UI selection. `GpuSceneExecutor`
(`bloom/runtime/gpu_scene_executor.hpp`, `gpu_scene_executor.cpp` with a planner and an execution
translation unit plus a private header) takes one immutable `PreparedGpuScene` and drives a bounded
sequence of already-typed native operations on an existing `GpuDevice`: `GpuSolid::begin`,
`GpuSolid::beginCovered`, `GpuImageUpload::begin` (one media source leaf, uploaded once per builder
semantic source key and reused when only the layer transform changes),
`GpuComposite::beginTranslation`, and `GpuComposite::beginSourceOver`.
`begin` validates the whole scene and flattens the reachable DAG, consulting `GpuSceneCache` first so a
warm unchanged output performs zero native dispatches and a fractional covered solid's superseded
solid command is unreachable; `poll` is non-blocking and starts at most one native dispatch per call.
The request byte budget bounds the actual LIVE unique pinned image bytes (deduped aliases, cached
inputs, and the output; the cache keeps its own separate ledger) and releases each charge at the last
dependency, so a long sequential graph whose peak live set fits is accepted even when its cumulative
allocation exceeds the budget. Each native op's real retained allocation comes from a new additive
`GpuSolid`/`GpuComposite`/`GpuImageUpload` `hasUnretiredSubmission()` / `lastJobAllocationBytes()`
accessor pair (stub `false`/0) instead of a requested extent, and only a native result that PROVES
fence retirement may release pins: a `DeviceUnavailable` with the submission still outstanding fails
`NativeUnproven`, retains every pin, and refuses reuse until the owner polls the submission to
retirement or destroys the executor. Cancellation, a per-job deadline, wrong-thread use, and device
loss fail closed with no published image; `DeviceLost` is terminal. Destruction is owner-thread and
native-first: the owned `GpuSolid`/`GpuComposite`/`GpuImageUpload` pipelines are reset before the
scene and executor pins, so the pipeline's bounded drain or whole-`Impl` quarantine strongly owns the
in-flight inputs. The executor owns no
thread, service, Qt surface, viewer, media decode, or readback path -- it owns exactly one
`GpuImageUpload` pipeline -- and `SourceOverV1` now
preserves the destination pixel aspect so a non-square-PAR merge does not lose PAR. Verified locally
with the pinned loader on a real device against a genuine uncached `CpuCompositionEvaluator` oracle
for every output pixel and the actual returned native descriptor: merged solids, fractional +0.3/-0.3
covered coverage, integer translation, HDR signed alpha, odd/nonzero-origin windows, proxy with
non-square PAR, opacity endpoints, empty/inactive time, byte-exact covered fill, warm zero-dispatch
cache reuse, retained lower subtrees, live-peak budgets with refusal/reuse and alias accounting,
cancellation, wrong-thread/foreign-device and mismatched-descriptor-cache rejection, native ownership
across teardown, real still/EXR-sequence/ProRes-video uploads with every-pixel parity and a
transform-only warm reuse of the resident source, and -- in a test-only fault-instrumented native
build -- the stalled-deadline,
unknown-fence, proven-cancellation, and device-loss retirement contracts. This is a tested checkpoint
only: no service, product, presentation, viewer, or application-benchmark selection is
activated, and no `ReferenceParity` claim is made.

The runtime now also owns a bounded, owner-thread GPU-resident frame lease registry
(`bloom/runtime/gpu_resident_frame_lease.hpp`, with `gpu_resident_frame_lease.cpp`), still without
service, product, cache, or viewer wiring. A copyable opaque `GpuResidentFrameLease` token carries
only immutable geometry metadata (extent, display window, pixel aspect, actual allocation bytes), its
lease id, the registry epoch, and one atomic validity flag; it holds no native image, device, or
Vulkan object, so copying, reading, or dropping it on the UI thread never touches Vulkan or frees
native memory. The `GpuResidentFrameLeaseRegistry` binds to exactly one actual `GpuDevice` on its
owner thread (`create()` refuses a foreign thread or a non-`Ready`/moved-from/stub device before any
allocation or native call), takes strong native ownership of one device-bound `GpuDisplayImage` in
`publish()`, charges the actual VMA allocation bytes reported by the new
`GpuDisplayImage::allocationBytes()` accessor (stub 0), and returns a move-only owner-thread
`GpuResidentFramePin` for a native present. There is deliberately no accessor that returns a strong
`shared_ptr<const GpuDisplayImage>` outside that pin, and the documented alias-safe contract keeps
the pin alive through the presentation fence. Refusals (foreign device, wrong thread, over-budget,
metadata cap, stale or foreign-registry token) do not disturb active leases; pinned tombstones stay
charged until their pin is released; the metadata cap defaults to a finite 4096 and the actual byte
budget is authoritative. `invalidateAll()` is the explicit lease-loss path, and a token that outlives
its registry reports invalid. Verified locally with the pinned loader on a real device: the SolidV1
-> `GpuResidentDisplay` chain feeding the lease, the UI-thread last-token release with owner
collection, the pinned-tombstone charge, foreign-device/registry and stale/wrong-thread refusals, and
a 1000-round concurrent release-vs-collect stress. The lease layer itself activates no
service/product/cache/viewer selection and makes no performance or ReferenceParity claim; the
resident display product arm below consumes it.

The runtime now also owns a bounded, owner-thread resident-preview qualification
(`bloom/runtime/gpu_resident_preview_qualification.hpp`, with Vulkan-free orchestration, operation,
fixture, and display translation units). It reuses the caller's already-created typed pipelines
(`GpuSolid`, `GpuImageUpload`, `GpuComposite`, `GpuResidentDisplay`) and refuses any pipeline not
bound to the exact actual device before any dispatch, so one device's results can never be labelled
with another's report. `GpuDevice::ownershipEpoch()` -- the existing per-device presentation epoch
for native, zero for the stub, no native handle exposed -- is the unique ownership identity because
the bootstrap capability generation is always 1; `eligibleFor()` re-checks that epoch, the
generation, the full identity, and the canonical Bloom Neutral v1 processor, so a second device on
the same physical GPU and every non-default processor are rejected. The report is constructed only
by `qualifyResidentPreview()` (private factory, no arbitrary success), is `PreviewOnly` only, makes
no `ReferenceParity` or final-render claim, and pins the shader/SPIR-V digests,
primitive/covered/dispatch semantics, fixture digest, processor identity, and measured timings.
Parity compares every pixel against the existing CPU primitives and the independent OCIO oracle
(resident display RGB within one code and alpha exact; translation/source-over within 2e-6
absolute-or-relative; nonzero subnormal frames rejected whole-frame and never published). The
eligible interval is measured only over the resident upload+display interval versus the same CPU OCIO
work (cold/warm alternating, 256x144..4K); it is not a whole-graph cost or application-FPS claim.
On cancellation, a bounded per-dispatch deadline, or any budget refusal, ownership stays with the
caller, which must destroy or drain the failed pipeline on the owner thread before reuse. This is
runtime qualification only: no service dispatch, product, viewer, or UI activation.

The closed `PreparedPreviewFrame` display variant now has a fourth, GPU-resident arm:
`PreviewResidentDisplayFrame`, built only by the validating owner-thread factory in
`bloom/runtime/gpu_resident_preview_product.hpp` / `gpu_resident_preview_product.cpp`. Its only
pixel storage is the opaque, owner-bound `GpuResidentFrameLease`; it retains no CPU pixel vector, no
native object, no Vulkan handle, and performs no readback. Alongside the lease it retains the genuine
immutable `GpuResidentPreviewQualificationReport` from the real `qualifyResidentPreview()` run, the
request and process identity, the evaluated bounds, and an explicit process-origin provenance
(`EvaluationProvider::GpuResident` for a GPU-evaluated scene, `CpuReference` for a CPU-evaluated one,
never inferred from the other). `retainedByteCost()` is the actual native allocation the lease
charges plus geometry/metadata, and `createResident()` re-stamps a retained frame by sharing the same
lease with no pixel or allocation copy. The factory validates the report's `eligible()`/`eligibleFor()`
against the exact device ownership epoch and processor, the registry binding and owner thread, the
canonical processor identity, the request/process identity and plan, the trusted expected descriptor
the request carries (the actual immutable prepared-scene output descriptor on the GPU path or the
actual CPU process image descriptor), the native display dimensions/data/display window/pixel aspect
against that trusted descriptor, the measured eligible interval, and the actual native allocation plus
geometry inside the request budget before publishing the lease; the registry publish is the proof the
token belongs to it. A reduced resolution (explicit proxy, Half, or Quarter) is validated against the
actual reduced descriptor and is never silently forced back to the full composition format, geometry
is never inferred from the returned image, and a full-format-but-wrong-proxy or one-pixel mismatch is
rejected. On the UI side, `PreviewFrameCache` retains the arm by shared pointer, charges
`retainedByteCost()`, re-stamps cheaply, and refuses an invalidated lease in
`take`/`contains`/`timesFor`/`insert`; `composition_preview_result.cpp` accepts a resident frame as a
Ready product through `isDisplayValid()`, branching on the honest provenance rather than the
resident-only accessor. Verified locally with the pinned loader on a real device: the real
qualification plus a real Solid -> `GpuResidentDisplay` -> owner-registry lease, accepted
full/proxy/half/quarter/PAR/ROI geometry inside the measured interval, and the rejection gates
(foreign registry/device/report, full-format descriptor under a proxy request, one-pixel mismatch,
wrong plan/identity, over-budget, null descriptor, unqualified processor, invalidated lease) with no
skipped gates; the cache's retained cost, shared-lease re-stamp, and invalidated-lease
miss/hide/refusal; and the existing preview-frame-cache, lease, and CPU-stage regressions. This arm
is consumable but deliberately not activated: no service, controller, viewer, or application route
paints it, no whole-image readback happens on the product path, and no final-render or
`ReferenceParity` claim is made.

The composition preview's CPU preparation half now also has a GPU-scene analogue.
`bloom::runtime::PreviewGpuSceneStage` / `PreviewGpuSceneStageFunction`
(`src/runtime/include/bloom/runtime/preview_gpu_scene_stage.hpp`) take the same immutable snapshot,
request identity, byte allowance, parameter overrides, and task context as `PreviewCpuStageFunction`,
but run the stateless `CpuGpuSceneBuilder::build()` to produce an immutable `PreparedGpuScene` of
resolved operands and geometry and select the CPU display processor, instead of evaluating a full
CPU `ProcessFrame`; it never allocates a full CPU scene image for a supported solid graph and never
fabricates an empty frame. The UI factory lives in
`bloom/ui/composition_preview_gpu_scene_stage.hpp` / `composition_preview_gpu_scene_stage.cpp`
because the compiled-plan cache is UI-owned, and the compile/validate/request-build/processor-selection
half is shared with the CPU stage through the private `composition_preview_stage_shared.*` so the two
cannot drift (same `CompiledPlanCache` reuse and one-gesture override rule, same `EvaluationRequest`,
same Pending window, same ACES/named-view/view-adjust selection). A semantic compile rejection is the
distinct `Unsupported`, while a composition that compiles but is outside the prepared GPU subset
(including a future media-unavailable code) is `UnsupportedGpuSubset`, a succeeded outcome the caller
turns into the full original CPU path; cancellation and genuine builder failures stay terminal
`TaskResult` states. The builder is constructed by the caller and injected, so a media-capable builder
supplies its decoder/context through its own constructor and no alternative decoder is invented.
Verified through the real compiler, builder, provider, and scheduler against a genuine
`CpuCompositionEvaluator` oracle: prepared identity, per-operation bounds and layer IDs, and
bit-identical output pixels via CPU replay of the prepared commands, plus text/rotation subset
fallback, the Pending reference window, a failed provider failing closed, the override plan-cache
rule, and pre-cancellation; the existing CPU-stage test object also re-links against the shared
helper and passes unchanged. This seam activates no service, device, product, viewer, or controller.

The runtime now also owns a bounded owner-thread presentation coordinator and the runtime already owns
the lease and native present path, but nothing in the running application selects them yet.
`bloom::runtime::GpuPresentationCoordinator`
(`bloom/runtime/gpu_presentation_coordinator.hpp`, with `gpu_presentation_coordinator.cpp`, a pump
translation unit, private transport/state headers, and `gpu_presentation_client.cpp`) is a coordinator,
not a thread, service, device, or message bus: it is constructed with the existing `GpuDevice` and
`GpuResidentFrameLeaseRegistry` on the existing service owner thread, owns no second thread, and does
no work until its `pump()` is called. The client (`GpuPresentationClient`) is Qt-free and Vulkan-free
and carries only typed requests and statuses plus an opaque borrowed-instance view/epoch; a
UI-created surface crosses as integer handle bits, and every native check and driver call stays on
the owner thread. It reuses the native `GpuPresentationTarget` acquire/present/retire path and the
strong-lease-pin contract, publishes `Retired` only after presentation-engine proof, publishes
`surfaceSafeToDestroy == false` for every refused or unproven retirement, and bounds retained records
with `maxRetainedTargets`, an explicit `forget()` terminal acknowledgement, and a fixed process
quarantine reservation store taken before native creation. Capacity pressure on that store refuses a
new attach temporarily and recovers when a healthy reservation is released; only an actual unproven
retention or allocation failure latches the process-wide fuse. The test-only quarantine reset refuses
to drop a committed generation that still owns a real target or lease alias.

The UI adapter is prepared but inert. `bloom::ui::ViewerGpuPresenter`
(`viewer_gpu_presenter.hpp`, with the private `viewer_gpu_presenter_port` seam and the
`viewer_gpu_presenter_input` translation helper) adopts the coordinator client's borrowed instance
through `QVulkanInstance`, creates one `QWindow`/`createWindowContainer` on the UI thread, forwards
actual Qt input, and presents only through the existing opaque lease/params/overlay port. It creates
no device, pipeline, queue, service, or thread, and it performs no native work on the UI thread. On
the measured Qt 6 Wayland hazard (a reparent silently recreates the `VkSurfaceKHR` with no
`SurfaceAboutToBeDestroyed`, and container destruction frees the surface immediately) it never
reparents, hides, or destroys the container while a target is live: the host must call
`prepareForMutation()` and wait for `SafeToMutate` before mutating the widget tree. The destructor
requires a proven terminal; a contract-violating destruction with a live target is not made safe and
emits a truthful release diagnostic instead of faking a handoff. The adapter reclaims proven-terminal
records through the runtime `forget()` after preserving the terminal diagnostic, and never forgets a
live, unproven, or duplicate-surface target. In a Vulkan-free UI build the same adapter source
compiles a truthful `Unsupported` fallback, so a portable desktop build needs no `<vulkan/vulkan.h>`
and the runtime presentation types stay Qt-free and Vulkan-free.

This lane is verified on the current native closure with real Wayland fixtures: the runtime CPU-only
port/bounds/forget gates; the real coordinator attach/present/overlay/coalesce/cancel/resize with two
distinct surfaces, duplicate-surface refusal, retire, and reattach; and the adapter's real embedded
`QWindow` attach, present, resize, gated retire, reparent-after-`SafeToMutate`, reattach, two viewers
sharing one lease, real `QTest` input, and repeated retire/reattach beyond `maxRetainedTargets` with
`forget()`, plus a CPU-only fake-port refusal/forget test. It is deliberately not activated: no
`ViewerEditor`, `MainWindow`, application controller, product, or service route consumes it, no live
application resident-present path exists, and there is no application-FPS or `ReferenceParity` claim.

Host-integration gating of the widget-tree mutations and application shutdown is now implemented
without activating any viewer. A typed optional `EditorNativeSurface` lifecycle interface (probed
with one `dynamic_cast`, exactly like `EditorChromeProvider`) lets a host ask an editor's presenter to
retire a live native target and wait for a genuine `SafeToMutate`. `NativeSurfaceRetirementGate` makes
every affected mutation all-or-nothing: a CPU-only subtree commits synchronously and unchanged, every
live target must report safe before the commit runs exactly once, a refusal runs no mutation and
resumes already-retired survivors, duplicate/inline/reentrant completions are rejected, and a required
receiver destroyed mid-retirement refuses rather than claiming the mutation succeeded. Every
completion holds a `weak_ptr` lifetime token and proves the gate is still alive before dereferencing
it, so a completion that arrives after gate destruction is inert instead of a use-after-free.
`EditorArea` defers a picker replacement of a live-native editor and reverts the picker until the
rebuild actually applies; `WorkspaceHost` retires the whole (conservative) tree before
split/close/collapse/root-replace/restore, reports `WorkspaceLayoutRestoreResult::Deferred` instead of
a premature `Restored`, and resumes only targets still attached to the live root so a `deleteLater`'d
outgoing subtree is never reattached. `ApplicationShutdownCoordinator` emits `shutdownQuiescent` only
once BOTH task quiescence and native-surface retirement are observed, and the application begins
`GpuPreviewDisplayService` shutdown on `shutdownQuiescent` (not `shutdownStarted`) so the service
keeps pumping until the surfaces it owns are genuinely retired. No `ViewerEditor`, application
viewer/controller, product, or service route implements or consumes the interface yet; CPU-only
platforms never see it and their synchronous behavior is unchanged.

The existing `GpuPreviewDisplayService` now additionally owns an opt-in resident dispatch route: a
genuine startup `qualifyResidentPreview()` report (independent of the packed readback qualification), a
prepared GPU scene driven through the existing `GpuSceneExecutor` -> `GpuResidentDisplay` -> resident
product factory, and an opaque owner-bound `GpuResidentFrameLease` published into the service's own
presentation registry -- all on the service's single owner thread, device, and scheduler GPU lease,
with zero full-frame readback. The default constructor is unchanged; the resident route is an additive
constructor overload selected only for an eligible device/processor, a usable presentation generation,
a neutral request, and a trusted output descriptor that fits the measured eligible interval. Anything
else takes the full original CPU path on the same snapshot/identity/overrides. A stage whose
failure/deadline/cancellation/lost generation leaves a native submission unretired enters an explicit
`Retiring` phase that retains the stage, its `GpuTaskCompletion`, and the native pins until retirement
is actually proven; `GpuSceneExecutor::hasUnretiredSubmission()` and
`GpuResidentDisplay::hasUnretiredSubmission()` are the authoritative tests (the executor's
`ownerDrainRequired()` is only the reuse gate latched by an unproven failure, and a logical display
`Failure` may still hold a queued submission), so ordinary cancellation and logical failure cannot
release the parent admission early. Healthy registry budget pressure is a temporary refusal that falls
back to the CPU without invalidating live leases or pins. Verified on the current native closure with
the pinned loader: the real enabled Wayland acceptance (genuine resident qualification, a real solid
prepared frame as `GpuResident` display and process provenance with no CPU buffer and no process
frame, a valid lease presented through a real `QWindow`/`VkSurfaceKHR`, warm-identical reuse with zero
additional native operations, an unsupported subset taking the full CPU path, cancellation during the
CPU stage, and host-ordered shutdown), plus the portable estimate/status regression and the existing
CPU/packed service suites. This is not activated: no application/`ViewerEditor`/controller/host
constructs the resident overload, the service constructor remains unused until that app wiring lands,
and the broader media `SourceOver` route and a presented-swapchain pixel oracle remain deferred
vertical-acceptance work, so no application-FPS or `ReferenceParity` claim is made. The startup
resident qualification time is a display-route microbenchmark only.

Pending and unchanged: the application usage of the service resident overload, per-layer GPU
compositing selection, resident GPU viewer buffers,
the service/viewer activation that consumes the render-side image-present path, a whole-application benchmark, the full per-operation qualification
fixtures for a future `ReferenceParity` profile (the qualified display transform and the resident
scene route remain `PreviewOnly`; no operation reaches `ReferenceParity`), general graph
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
