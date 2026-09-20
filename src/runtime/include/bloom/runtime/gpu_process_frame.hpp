#pragma once

// Qt-free, Vulkan-free GPU final-render/export bridge.
//
// This is the first bounded vertical slice of GPU final output. It owns one dedicated owner worker
// thread, a native `render::GpuDevice`, a content cache, and the genuine scene executor, and it
// turns a real `CompiledCompositionPlan` + `EvaluationRequest` into
//
//   * the same `PreparedGpuScene` vocabulary the resident preview route already uses
//     (`CpuGpuSceneBuilder` -> `GpuSceneExecutor`), and
//   * ONE final, bounded, owner-thread RGBA32F host readback at the CPU output-adapter boundary.
//
// The resulting immutable `ProcessFrame` carries `EvaluationProvider::GpuResident` and is built
// only through the real private `ProcessFrame` constructor (narrow trusted friend), so a GPU frame
// is indistinguishable from a CPU frame except for the provenance the export pipeline already
// models.
//
// The CPU reference evaluator remains the correctness oracle and the fallback. When the device is
// unavailable, the requested scene is outside the currently qualified prepared-GPU subset, or the
// request is over budget, `evaluate()` returns a typed non-`Evaluated` outcome and the caller takes
// the ordinary CPU path. Unsupported ordinary operations are diagnosed, never silently executed on
// the CPU and reported as GPU.
//
// This slice intentionally uses the existing prepared-GPU operation subset
// (solid/covered-solid/translation/source-over/image-upload). Every additional operation added to
// `CpuGpuSceneBuilder` is accelerated automatically because this bridge drives the shared executor
// vocabulary rather than a parallel node evaluator.

#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bloom::render {
class Rgba32fImage;
} // namespace bloom::render

namespace bloom::runtime {

enum class GpuProcessFrameStatus : std::uint8_t {
    // A genuine GPU process frame was produced and read back exactly once.
    Evaluated,
    // GPU execution was not requested (options.enabled == false).
    Disabled,
    // No usable loader/device; the caller must take the CPU path.
    DeviceUnavailable,
    // The composition compiled but a reachable operation is outside the prepared-GPU subset, or the
    // evaluator refused the scene. The caller takes the full CPU path.
    UnsupportedGpuSubset,
    // Cancellation before the native result was read back; no frame is published.
    Cancelled,
    // A genuine failure (over budget, deadline, readback failure, device loss, invariant).
    Failed,
};

enum class GpuProcessFrameDiagnosticCode : std::uint8_t {
    None,
    Disabled,
    LoaderUnavailable,
    DeviceUnavailable,
    SceneUnsupported,
    InvalidArgument,
    InvalidRequest,
    OverBudget,
    DeadlineExceeded,
    Cancelled,
    DeviceLost,
    ReadbackFailed,
    ReadbackOverBudget,
    CacheUnavailable,
    ExecutorUnavailable,
    BadAllocation,
    InternalInvariant,
};

struct GpuProcessFrameDiagnostic final {
    GpuProcessFrameDiagnosticCode code = GpuProcessFrameDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuProcessFrameDiagnostic&,
                           const GpuProcessFrameDiagnostic&) = default;
};

// The genuine native work this request performed. `readbacks` counts successful final RGBA32F host
// readbacks (0 or 1 per produced frame). `nativeDispatches` is the actual `GpuSceneExecutor`
// begin() count across the scene families; a warm unchanged scene served by the content cache
// reports zero new dispatches.
struct GpuProcessFrameCounters final {
    std::uint64_t sceneBegins = 0;
    std::uint64_t scenesCompleted = 0;
    std::uint64_t outputCacheHits = 0;
    std::uint64_t outputCacheMisses = 0;
    std::uint64_t solidDispatches = 0;
    std::uint64_t coveredSolidDispatches = 0;
    std::uint64_t translationDispatches = 0;
    std::uint64_t sourceOverDispatches = 0;
    std::uint64_t uploads = 0;
    std::uint64_t nativeDispatches = 0;
    std::uint64_t readbacks = 0;

    friend bool operator==(const GpuProcessFrameCounters&,
                           const GpuProcessFrameCounters&) = default;
};

struct GpuProcessFrameOutcome final {
    GpuProcessFrameStatus status = GpuProcessFrameStatus::Failed;
    std::shared_ptr<const ProcessFrame> frame;
    GpuProcessFrameDiagnostic diagnostic;
    GpuProcessFrameCounters counters;

    [[nodiscard]] bool hasValue() const noexcept { return frame != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

struct GpuProcessFrameEvaluatorOptions final {
    // Explicit activation: false never touches a device and every request is Disabled.
    bool enabled = true;
    // Explicit native loader override (empty means the platform loader). No workspace/build path is
    // ever hardcoded. This is the same field `GpuDeviceCreationOptions::loader_path` uses.
    std::filesystem::path loaderPath;
    // Per-request LIVE unique pinned-byte budget handed to GpuSceneExecutor::begin().
    std::uint64_t requestByteBudget = 1ULL << 30U;
    // Hard ceiling for the one final readback allocation (host staging + returned pixels).
    std::uint64_t readbackByteBudget = 1ULL << 30U;
    // Bounded per-native-job deadline while polling the executor.
    std::chrono::milliseconds nativeDeadline{30000};
    // Bounded admission: the maximum number of admitted-but-not-yet-running requests. A request
    // beyond this is refused with a typed OverBudget and no frame.
    std::size_t maxQueuedRequests = 16;
    GpuSceneCacheBudgets cacheBudgets{};
    GpuSceneExecutorBudgets executorBudgets{};
};

// Dedicated owner worker. Construction starts the worker and initializes the device on it; a
// request never touches Vulkan from the caller's thread, and the caller is never the UI thread
// requirement on this class (it may be called from a Cpu worker). `evaluate()` is bounded by the
// per-job deadline and cancellation, and performs at most one final RGBA32F readback.
class GpuProcessFrameEvaluator final {
  public:
    GpuProcessFrameEvaluator(const GpuProcessFrameEvaluator&) = delete;
    GpuProcessFrameEvaluator& operator=(const GpuProcessFrameEvaluator&) = delete;
    GpuProcessFrameEvaluator(GpuProcessFrameEvaluator&&) = delete;
    GpuProcessFrameEvaluator& operator=(GpuProcessFrameEvaluator&&) = delete;
    ~GpuProcessFrameEvaluator();

    // Starts the owner worker; never submits arbitrary work on the caller's thread.
    [[nodiscard]] static std::unique_ptr<GpuProcessFrameEvaluator>
    create(const GpuProcessFrameEvaluatorOptions& options = {});

    // True once the device, cache, and executor are live on the owner thread.
    [[nodiscard]] bool gpuAvailable() const noexcept;
    // Explains a non-ready state; None when gpuAvailable().
    [[nodiscard]] GpuProcessFrameDiagnostic availabilityDiagnostic() const;

    // Builds the prepared scene with the real CpuGpuSceneBuilder, executes it on the owner worker,
    // and performs exactly one final readback. Never blocks on a Vulkan call from the caller
    // thread. A scene outside the prepared-GPU subset is `UnsupportedGpuSubset`; the caller falls
    // back to the CPU reference evaluator.
    [[nodiscard]] GpuProcessFrameOutcome
    evaluate(std::shared_ptr<const CompiledCompositionPlan> plan, const EvaluationRequest& request,
             const CancellationToken& cancellation = {}, EvaluationProgressCallback progress = {});

    // Non-blocking. The destructor joins the owner worker.
    void beginShutdown() noexcept;

  private:
    struct Impl;
    explicit GpuProcessFrameEvaluator(std::unique_ptr<Impl> impl) noexcept;

    // The only place a GPU-provenance frame is constructed. It is a member of the friend class, so
    // the real private ProcessFrame constructor is reached without any public or forged path.
    [[nodiscard]] static std::shared_ptr<const ProcessFrame>
    publishGpuProcessFrame(ProcessFrameIdentity identity,
                           std::shared_ptr<const render::Rgba32fImage> processImage,
                           std::vector<EvaluatedOperationBounds> bounds, std::string contentHash);

    std::unique_ptr<Impl> impl_;
};

} // namespace bloom::runtime
