#pragma once

// The one bounded runtime service that owns the GPU half of composition preview display.
//
// It owns a dedicated service thread, a native device/pipeline created on that thread, and one
// outstanding native display job at a time. It never exposes a native/Vulkan/Qt handle and never
// performs media I/O, decoding, evaluation, or rendering on the UI thread. Callers submit an
// ordinary preview request; the service either composes the accepted CPU stage with the display
// fallback on a CPU worker (the reference/unavailable path) or, when qualified, runs the evaluated
// stage through the fixed GPU display operation on the service thread and returns the single
// final submitGpu handle.
//
// Frames are immutable, display-only, and carry GPU provenance; no Float32 process image is
// retained by the GPU product. Completed bytes are CPU packed RGBA8 and make no
// GPU-resident/presentation claim.
//
// Threading and lifetime: the constructor attaches the caller's TaskScheduler GPU executor and
// starts the service thread. The captured PreviewCpuStageFunction, PreviewCpuDisplayFallback, and
// the scheduler MUST outlive the service. beginShutdown() is non-blocking; the destructor joins
// the service thread and drains child/native ownership before releasing the lease.

#include <bloom/render/gpu_allocator_memory.hpp>
#include <bloom/render/gpu_neutral_display.hpp>
#include <bloom/render/gpu_queue_timing.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/runtime/gpu_memory_budget.hpp>
#include <bloom/runtime/gpu_presentation_coordinator.hpp>
#include <bloom/runtime/gpu_preview_resident_capacity.hpp>
#include <bloom/runtime/gpu_resident_frame_lease.hpp>
#include <bloom/runtime/gpu_resident_preview_qualification.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/preview_cpu_stage.hpp>
#include <bloom/runtime/preview_gpu_scene_stage.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::runtime {

class GpuNeutralDisplayQualificationReport;

namespace detail {
struct GpuPreviewDisplayServiceTestAccess;
}

enum class GpuPreviewDisplayServiceState : std::uint8_t {
    // The scheduler GPU startup task (device bootstrap + exact embedded neutral CPU processor
    // build + qualification) has not reached a terminal outcome yet.
    Initializing,
    // Qualified: the pinned Bloom Neutral v1 display operation passed the frozen fixture gate and
    // the eligible pixel interval is populated.
    Ready,
    // Disabled, no loader/device, processor build failed, or qualification/parity failed. Requests
    // take the CPU stage + display-fallback path.
    Unavailable,
    // beginShutdown() has been requested; the service thread is draining.
    Stopping,
    // The service thread has drained and released its assignment.
    Stopped,
};

enum class GpuPreviewDisplayServiceDiagnosticCode : std::uint8_t {
    None,
    Disabled,
    LoaderUnavailable,
    DeviceUnavailable,
    ProcessorUnavailable,
    QualificationUnavailable,
    NativeFailure,
    NativeTimeout,
    AdmissionUnavailable,
    ShuttingDown,
    // The resident preview route was requested but its genuine qualification report is not eligible
    // (processor/device/parity/timing). Requests without a usable resident generation take the full
    // original CPU path.
    ResidentQualificationUnavailable,
    // The resident route qualified, but this service generation has no usable presentation
    // generation (mode Disabled or the Wayland capability is not Ready). Resident frames cannot be
    // presented, so requests take the full CPU path and no blank activation is claimed.
    ResidentPresentationUnavailable,
    // A resident registry publication was refused under byte/count budget pressure. The released
    // tokens are collected on the next owner pump and the same request takes the CPU fallback; the
    // live leases/pins are never invalidated and no permanent GPU poison is latched.
    ResidentLeasePressure,
};

struct GpuPreviewDisplayServiceDiagnostic final {
    GpuPreviewDisplayServiceDiagnosticCode code = GpuPreviewDisplayServiceDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuPreviewDisplayServiceDiagnostic&,
                           const GpuPreviewDisplayServiceDiagnostic&) = default;
};

// Optional owner-thread presentation ownership. The default (Disabled) reproduces the existing
// compute-only service: the device is created without a presentation bootstrap. GPU-prepared packed
// products remain eligible offscreen; NativeResident products require presentation. Wayland asks
// the same service device for the Wayland
// presentation bootstrap; the app selects it explicitly at construction (no hidden activation).
enum class GpuPreviewDisplayServicePresentationMode : std::uint8_t {
    Disabled,
    Wayland,
};

// Cached, end-to-end counters for the resident display route. This is a bounded counter snapshot,
// not a profiling framework: it exists so a concrete integration test can assert the resident arm
// actually dispatched native work, hit the content cache, read nothing back, and fell back to the
// CPU for the unsupported subset. NativeResident performs no full-frame readback; GPU-prepared
// packed products add exactly one final display transfer, counted separately too.
struct GpuPreviewDisplayServiceCounters final {
    // Resident scene graphs admitted to the GpuSceneExecutor. One per prepared GPU scene that
    // entered the resident native route, INCLUDING a warm-cache graph that performs zero native
    // operations. This is deliberately distinct from `nativeDispatches` so "graph jobs" are never
    // confused with "actual native operations".
    std::uint64_t residentGraphJobs = 0;
    // Actual native work operations (dispatches and uploads) executed by GpuSceneExecutor,
    // including every family. Several operations may share one work submission. A warm cache hit
    // that cuts the whole subtree leaves this unchanged (zero for an identical warm request).
    std::uint64_t nativeDispatches = 0;
    // CPU wall stages, including host submission and fence observation/polling delay.
    std::uint64_t uploadSubmitNanoseconds = 0;
    std::uint64_t uploadFenceObservationNanoseconds = 0;
    std::uint64_t processingSubmitNanoseconds = 0;
    std::uint64_t processingFenceObservationNanoseconds = 0;
    std::uint64_t completedStepContinuations = 0;
    std::uint64_t packedTransferNanoseconds = 0;
    // Resident graph jobs that produced a published native or GPU-prepared packed frame.
    std::uint64_t residentCompletions = 0;
    // Resident graph jobs that failed, timed out, or were cancelled after admission.
    std::uint64_t residentFailures = 0;
    // GpuSceneExecutor content-cache observations for the resident route.
    std::uint64_t gpuCacheHits = 0;
    std::uint64_t gpuCacheMisses = 0;
    // Requests that took the full original CPU path (unsupported subset, non-neutral, over budget,
    // unavailable presentation, lease pressure, or a resident-native failure).
    std::uint64_t cpuFallbacks = 0;
    // GPU parent submissions refused by the scheduler's bounded per-request admission (the derived
    // host default or an explicit configured capacity) BEFORE any stage work. Non-zero means a
    // request the display stage could have handled was forced onto the CPU by ADMISSION, not by
    // device capability; this makes an admission mismatch observable instead of a silent
    // cpuFallbacks increase.
    std::uint64_t gpuAdmissionRefusals = 0;
    // Scene-executor readbacks plus final packed transfers. NativeResident keeps this zero; its
    // 4-byte status reads are separate. Packed mode transfers only the final display data window.
    std::uint64_t fullFrameReadbacks = 0;
    // 4-byte resident-display status-word invalidations. One per completed resident display job;
    // this is NOT a full-frame readback.
    std::uint64_t displayStatusReads = 0;
    // GPU-prepared packed mode only: one final RGBA8 copy, separate from native route assertions.
    std::uint64_t packedTransfers = 0;
    std::uint64_t packedTransferredBytes = 0;
    std::uint64_t packedPeakTransferBytes = 0;
    // Conservative admitted transfer ceiling, retained through scheduler detach/quarantine.
    // Actual allocations are reported by the three retained byte fields below.
    std::uint64_t packedReservedBytes = 0;
    std::uint64_t packedRetainedSourceBytes = 0;
    std::uint64_t packedRetainedStagingBytes = 0;
    std::uint64_t packedRetainedHostBytes = 0;
    // Resident registry publications refused under byte/count budget pressure. Live leases and pins
    // are never invalidated by this; the released tokens are collected on the next owner pump.
    std::uint64_t residentLeaseRefusals = 0;
    // Bounded owner-thread teardowns that had to destroy the resident pipelines because a native
    // fence could not be proven retired within the retirement budget (the pipeline destructor then
    // performs its own bounded drain/quarantine). Non-zero means the resident route is disabled for
    // this service generation; it is never a silent permanent poison for healthy budget pressure.
    std::uint64_t retirementUnprovenTeardowns = 0;
    // Wall-clock spent in the genuine startup resident qualification, in microseconds. This is a
    // display-route microbenchmark only; it is never a whole-application FPS claim.
    std::uint64_t residentQualificationMicros = 0;

    friend bool operator==(const GpuPreviewDisplayServiceCounters&,
                           const GpuPreviewDisplayServiceCounters&) = default;
};

// One consistent locked read of the service state: status, eligibility, the immutable
// qualification report, and the structured diagnostic that explains a non-Ready state. It also
// carries the presentation generation: the immutable UI-side port (null when no live presentation
// generation is owned) and the actual owner-published presentation shutdown snapshot. When
// `presentationClient` is null, presentation is not available and the host must take the
// CPU/packed path; it must never claim a blank presentation was activated.
struct GpuPreviewDisplayServiceStatus final {
    GpuPreviewDisplayServiceState state = GpuPreviewDisplayServiceState::Initializing;
    bool gpuAvailable = false;
    std::shared_ptr<const GpuNeutralDisplayQualificationReport> qualification;
    GpuPreviewDisplayServiceDiagnostic diagnostic;
    // The genuine resident-route qualification report, produced independently of the older packed
    // readback qualification. Non-null once the owner startup resident qualification has run; the
    // report's own eligible()/eligibleFor() — not the packed report — decides resident selection.
    std::shared_ptr<const GpuResidentPreviewQualificationReport> residentQualification;
    std::string residentDetail;
    // Immutable, owner-resolved resident-route capacity and its shared partition. Default
    // constructed (Unknown/all-zero) until the owner resolves it after device creation; the UI
    // installs the resolved cache sublimit through the EXISTING status poll and never queries a
    // device itself.
    GpuResidentCapacityPlan residentCapacityPlan;
    // Owner-sampled rooted foreground ceiling. This is free/plannable device space after actual
    // ownership, not another additive part of residentCapacityPlan.poolBytes. Omitted rooted
    // requests still intersect it with the live delivered host allowance and their phase grant.
    std::uint64_t foregroundRequestBytes = 0;
    GpuPreviewDisplayServiceCounters counters;
    // Latest owner-observed diagnostic intervals; these never establish painted-frame identity.
    render::GpuAllocatorMemorySnapshot allocatorMemory;
    std::chrono::steady_clock::time_point allocatorMemorySampledAt{};
    render::GpuQueueTimingSample graphQueueTiming;
    render::GpuQueueTimingSample packedTransferQueueTiming;

    // Presentation capability of this service generation. NotRequested for the default Disabled
    // mode; Unavailable when Wayland was requested but a required capability is missing.
    render::GpuPresentationAvailability presentationAvailability =
        render::GpuPresentationAvailability::NotRequested;
    std::string presentationDetail;
    // Immutable UI-side port owned by the service's presentation coordinator. Holds only a shared
    // mailbox and no native object; safe to call from the UI thread. Null until the owner thread
    // has created the registry + coordinator.
    std::shared_ptr<GpuPresentationClient> presentationClient;
    // Owner-published, thread-safe snapshot of the presentation generation's retirement state. A
    // host reads this after beginShutdown() and must refuse Qt teardown while it is not drained, or
    // while it reports any unproven/quarantined target.
    GpuPresentationShutdownStatus presentationShutdown;
};

// `enabled` is the explicit activation flag; when false the service never touches a device or the
// scheduler GPU executor and every request takes the ordinary CPU path. `loaderPath` is the
// explicit native loader override (empty means the platform loader); the service never hardcodes a
// workspace or build path. `previewByteAllowance` is the full per-request byte allowance reserved
// as GPU request-owned admission (capacity-aware by default via gpuPreviewRequestByteAllowance();
// a 1 GiB scheduler request-owned capacity admits two). `nativeBudgets` bounds the native
// pipeline's persistent buffers separately (160 MiB default). Prepared stages waiting for the
// native job are bounded by the scheduler's GPU admission and their admitted processing phases; a
// long wait is back-pressure and never turns an eligible GPU stage into a CPU render.
enum class GpuPreviewDisplayProductMode : std::uint8_t { NativeResident, GpuPreparedPacked };

struct GpuPreviewDisplayServiceOptions final {
    // Explicit composition-root injection, shared with final rendering; no physical accounting
    // claim.
    std::shared_ptr<GpuProcessingPhasePolicy> processingPhasePolicy = {};
    PreviewCpuStoragePlanner cpuStoragePlanner = {};
    bool enabled = false;
    GpuPreviewDisplayProductMode product = GpuPreviewDisplayProductMode::NativeResident;
    std::filesystem::path loaderPath;
    // Presentation ownership. Default Disabled keeps the compute-only device exactly as before.
    GpuPreviewDisplayServicePresentationMode presentation =
        GpuPreviewDisplayServicePresentationMode::Disabled;
    // Bounded resident-frame lease registry budget (charged in actual native allocation bytes).
    GpuResidentFrameLeaseBudgets residentLeaseBudgets{};
    // Bounded resident content-cache budget (retained intermediate GpuImages, actual VMA bytes).
    // This is a separate ledger from the lease registry and the executor's per-request budget.
    GpuSceneCacheBudgets residentSceneCacheBudgets{};
    // Explicit producer/structural ceilings are honored verbatim, including combined image and
    // staging peaks. A rooted omitted producer limit follows the explicit preview ceiling, or
    // the finite owner-resolved physical request ceiling when a live default view is injected.
    // Each request still applies its delivered/default or explicit allowance, admitted phase,
    // live pins and native resource limits. Unrooted omission retains legacy producer budgets.
    // Diagnostics preserve omitted producer defaults; enabling timing is not a budget override.
    bool collectQueueTimestamps = false;
    std::optional<GpuSceneExecutorBudgets> residentExecutorBudgets = {};
    // Bounded resident display pipeline owned-bytes budget per job.
    render::GpuResidentDisplayBudgets residentDisplayBudgets{};
    // Concurrent packed transfer staging/host capacity, separate from the display's native owner.
    // Composition roots reserve this ceiling in the conservative phase working envelope.
    std::uint64_t packedTransferByteAllowance = 320ULL * 1024 * 1024;
    // Bounded resident qualification native budgets (the qualification reads back only internally).
    GpuResidentPreviewBudgets residentQualificationBudgets{};
    // Bounded presentation coordinator admission/overlay/drain options.
    GpuPresentationCoordinatorOptions presentationCoordinator{};
    // Omitted limits use the explicit root numeric view when injected; no-view owners retain
    // their legacy construction-time default. Explicit finite ceilings are never enlarged.
    std::optional<std::size_t> previewByteAllowance = {};
    render::GpuNeutralDisplayBudgets nativeBudgets{};
    // Bounded per-native-dispatch deadline. Expiry drains/quarantines the native pipeline on
    // the owner thread, disables GPU, and takes the same-frame CPU fallback (or cancels).
    std::chrono::milliseconds nativeDispatchDeadline{10000};
};

class GpuPreviewDisplayService final {
  public:
    GpuPreviewDisplayService(const GpuPreviewDisplayService&) = delete;
    GpuPreviewDisplayService& operator=(const GpuPreviewDisplayService&) = delete;
    GpuPreviewDisplayService(GpuPreviewDisplayService&&) = delete;
    GpuPreviewDisplayService& operator=(GpuPreviewDisplayService&&) = delete;
    ~GpuPreviewDisplayService();

    // Attaches the scheduler GPU executor and starts the service thread/startup task. Never
    // blocks the caller on device or qualification work.
    GpuPreviewDisplayService(TaskScheduler& scheduler, PreviewCpuStageFunction stageFunction,
                             PreviewCpuDisplayFallback displayFallback,
                             GpuPreviewDisplayServiceOptions options = {});

    // Resident-route overload. The service additionally owns a genuine resident-route qualification
    // (run on the service owner thread at startup) and, for an eligible resolved scene, prepares
    // the GPU scene on a CPU child, then runs it through the existing GpuSceneExecutor ->
    // GpuResidentDisplay -> product factory on the same owner thread and publishes an opaque
    // GpuResidentFrameLease (no full-frame readback). A request whose stage is Unsupported,
    // UnsupportedGpuSubset, non-neutral, over budget, whose presentation generation is unavailable,
    // or whose resident dispatch fails takes the FULL original CPU path through `cpuStageFunction`
    // + `displayFallback`. There is no second service, thread, device, or scheduler lease.
    GpuPreviewDisplayService(TaskScheduler& scheduler,
                             PreviewGpuSceneStageFunction gpuStageFunction,
                             PreviewCpuStageFunction cpuStageFunction,
                             PreviewCpuDisplayFallback displayFallback,
                             GpuPreviewDisplayServiceOptions options = {});

    // Returns the stable final handle for the request: a submitGpu handle on the qualified GPU
    // path, or an ordinary CPU task handle (CPU stage + display fallback on the same evaluated
    // frame) on the reference/unavailable/disabled path. `pixelStorageByteLimit` is the request's
    // display byte allowance and is also the GPU request-owned admission reservation.
    [[nodiscard]] TaskSubmission<PreviewPreparationResultHandle>
    submit(TaskRequest request, const document::Snapshot& snapshot,
           const PreviewRequestIdentity& identity, std::size_t pixelStorageByteLimit,
           const std::vector<SnapshotParameterOverride>& overrides);

    [[nodiscard]] GpuPreviewDisplayServiceStatus status() const;

    // Owner-thread clear of the resident GPU scene cache's retained operation/output images. Must
    // be called from a worker thread, never the UI thread: it posts the clear to the service owner
    // thread and waits there (bounded by `timeout`). Clearing drops only the cache's own
    // references, so a displayed or exported lease that still pins an image stays valid -- no live
    // lease is invalidated and no native object is destroyed. Returns true when the clear ran (or
    // this service has no resident route), false when the service is shutting down or the wait
    // expired.
    [[nodiscard]] bool purgeRetainedCaches(std::chrono::milliseconds timeout) noexcept;

    // Non-blocking. Closes admission for this service's own tasks, requests cancellation on the
    // tasks it submitted (including ordinary CPU fallback roots), and wakes the service thread.
    // It never drains and never touches unrelated scheduler tasks.
    void beginShutdown() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend struct detail::GpuPreviewDisplayServiceTestAccess;
};

} // namespace bloom::runtime
