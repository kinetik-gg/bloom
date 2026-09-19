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
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/preview_cpu_stage.hpp>
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
};

struct GpuPreviewDisplayServiceDiagnostic final {
    GpuPreviewDisplayServiceDiagnosticCode code = GpuPreviewDisplayServiceDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuPreviewDisplayServiceDiagnostic&,
                           const GpuPreviewDisplayServiceDiagnostic&) = default;
};

// One consistent locked read of the service state: status, eligibility, the immutable
// qualification report, and the structured diagnostic that explains a non-Ready state.
struct GpuPreviewDisplayServiceStatus final {
    GpuPreviewDisplayServiceState state = GpuPreviewDisplayServiceState::Initializing;
    bool gpuAvailable = false;
    std::shared_ptr<const GpuNeutralDisplayQualificationReport> qualification;
    GpuPreviewDisplayServiceDiagnostic diagnostic;
};

// `enabled` is the explicit activation flag; when false the service never touches a device or the
// scheduler GPU executor and every request takes the ordinary CPU path. `loaderPath` is the
// explicit native loader override (empty means the platform loader); the service never hardcodes a
// workspace or build path. `previewByteAllowance` is the full per-request byte allowance reserved
// as GPU request-owned admission (512 MiB default; a 1 GiB scheduler request-owned capacity admits
// two). `nativeBudgets` bounds the native pipeline's persistent buffers separately (160 MiB
// default). `readyStageQueueCapacity` bounds stages waiting for the one native job.
struct GpuPreviewDisplayServiceOptions final {
    bool enabled = false;
    std::filesystem::path loaderPath;
    std::size_t previewByteAllowance = std::size_t{512} * 1024U * 1024U;
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
