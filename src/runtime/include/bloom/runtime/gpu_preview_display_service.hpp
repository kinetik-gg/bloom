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

#include <bloom/render/gpu_neutral_display.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/runtime/gpu_memory_budget.hpp>
#include <bloom/runtime/gpu_presentation_coordinator.hpp>
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
// compute-only service exactly: the device is created without a presentation bootstrap and every
// request takes the ordinary CPU/packed path. Wayland asks the same service device for the Wayland
// presentation bootstrap; the app selects it explicitly at construction (no hidden activation).
enum class GpuPreviewDisplayServicePresentationMode : std::uint8_t {
    Disabled,
    Wayland,
};

// Cached, end-to-end counters for the resident display route. This is a bounded counter snapshot,
// not a profiling framework: it exists so a concrete integration test can assert the resident arm
// actually dispatched native work, hit the content cache, read nothing back, and fell back to the
// CPU for the unsupported subset. `readbacks` is always zero on the resident route by construction
// and is exposed precisely so a test can prove no full-frame readback happened.
struct GpuPreviewDisplayServiceCounters final {
    // Resident scene graphs admitted to the GpuSceneExecutor. One per prepared GPU scene that
    // entered the resident native route, INCLUDING a warm-cache graph that performs zero native
    // operations. This is deliberately distinct from `nativeDispatches` so "graph jobs" are never
    // confused with "actual native operations".
    std::uint64_t residentGraphJobs = 0;
    // ACTUAL native operation begin() count executed by the GpuSceneExecutor
    // (solid + covered-solid + translation + source-over + uploads). A warm content-cache hit that
    // cuts the whole subtree leaves this unchanged (zero for an identical warm request).
    std::uint64_t nativeDispatches = 0;
    // Resident graph jobs that produced a published resident frame.
    std::uint64_t residentCompletions = 0;
    // Resident graph jobs that failed, timed out, or were cancelled after admission.
    std::uint64_t residentFailures = 0;
    // GpuSceneExecutor content-cache observations for the resident route.
    std::uint64_t gpuCacheHits = 0;
    std::uint64_t gpuCacheMisses = 0;
    // Requests that took the full original CPU path (unsupported subset, non-neutral, over budget,
    // unavailable presentation, lease pressure, or a resident-native failure).
    std::uint64_t cpuFallbacks = 0;
    // FULL-FRAME host readbacks performed by the resident route. Always zero: the only host read on
    // the normal resident path is the resident display's 4-byte status word, counted separately
    // below. This field is an explicit negative assertion hook for "no CPU buffer / no packed
    // readback".
    std::uint64_t fullFrameReadbacks = 0;
    // 4-byte resident-display status-word invalidations. One per completed resident display job;
    // this is NOT a full-frame readback.
    std::uint64_t displayStatusReads = 0;
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
    GpuPreviewDisplayServiceCounters counters;

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
// pipeline's persistent buffers separately (160 MiB default). `readyStageQueueCapacity` bounds
// stages waiting for the one native job.
struct GpuPreviewDisplayServiceOptions final {
    bool enabled = false;
    std::filesystem::path loaderPath;
    // Presentation ownership. Default Disabled keeps the compute-only device exactly as before.
    GpuPreviewDisplayServicePresentationMode presentation =
        GpuPreviewDisplayServicePresentationMode::Disabled;
    // Bounded resident-frame lease registry budget (charged in actual native allocation bytes).
    GpuResidentFrameLeaseBudgets residentLeaseBudgets{};
    // Bounded resident content-cache budget (retained intermediate GpuImages, actual VMA bytes).
    // This is a separate ledger from the lease registry and the executor's per-request budget.
    GpuSceneCacheBudgets residentSceneCacheBudgets{};
    // Bounded resident executor per-request budget (LIVE unique pinned bytes) and structural caps.
    GpuSceneExecutorBudgets residentExecutorBudgets{};
    // Bounded resident display pipeline owned-bytes budget per job.
    render::GpuResidentDisplayBudgets residentDisplayBudgets{};
    // Bounded resident qualification native budgets (the qualification reads back only internally).
    GpuResidentPreviewBudgets residentQualificationBudgets{};
    // Bounded presentation coordinator admission/overlay/drain options.
    GpuPresentationCoordinatorOptions presentationCoordinator{};
    std::size_t previewByteAllowance = gpuPreviewRequestByteAllowance();
    render::GpuNeutralDisplayBudgets nativeBudgets{};
    // Bounded per-native-dispatch deadline. Expiry drains/quarantines the native pipeline on
    // the owner thread, disables GPU, and takes the same-frame CPU fallback (or cancels).
    std::chrono::milliseconds nativeDispatchDeadline{10000};
    std::size_t readyStageQueueCapacity = 8;
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
