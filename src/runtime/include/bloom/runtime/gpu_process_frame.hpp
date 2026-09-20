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
#include <bloom/runtime/gpu_ocio_command.hpp>
#include <bloom/runtime/gpu_output_color.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::render {
class Rgba32fImage;
} // namespace bloom::render

namespace bloom::runtime {

class GpuOcioContextResolver;

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
    // The native device ownership epoch that produced this outcome, read on the owner thread from
    // GpuDevice::ownershipEpoch(). It is a genuine value from the actual device, never fabricated,
    // and is zero when no device evaluated the request (disabled, unavailable, unsupported,
    // cancelled, or failed before a device result). Diagnostics only; it never enters a digest.
    std::uint64_t deviceOwnershipEpoch = 0;

    // Encoded output-colour product, produced by the same single combined final readback that
    // produced the process payload above. `encodedArm` is None for the identity arm (no output
    // command was supplied): only the exact process payload is transferred and
    // `outputColorCounters.transferredPayloads == 1`. A ProcessEffect command fills
    // `encodedEffectRgba32f`; a DisplayRgba8 command fills `encodedDisplayRgba8`. The process
    // payload is always the unchanged process bits used for semantic identity.
    GpuOutputColorArm encodedArm = GpuOutputColorArm::None;
    std::vector<render::Rgba32f> encodedEffectRgba32f;
    std::vector<render::Rgba8> encodedDisplayRgba8;
    core::Sha256Digest outputCommandIdentity{};
    GpuOutputColorCounters outputColorCounters;

    [[nodiscard]] bool hasValue() const noexcept { return frame != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

// A bounded default working byte budget for ONE GPU final-render/export frame, derived from the
// host's currently-available RAM. Pure policy: a conservative share (1/4) of what is actually
// available, with NO fixed floor and NO fixed ceiling. A low-memory host therefore gets a small
// budget rather than an over-committing floor, and a large host gets a proportionally large budget
// rather than an artificial ceiling, so the gate opens for any composition the host can genuinely
// run. Unknown availability uses a small typed conservative fallback. A caller that needs an exact
// limit still injects one explicitly.
[[nodiscard]] std::size_t
gpuProcessFrameByteBudgetForAvailable(std::optional<std::size_t> availableBytes) noexcept;
[[nodiscard]] std::size_t defaultGpuProcessFrameByteBudget() noexcept;

struct GpuProcessFrameEvaluatorOptions final {
    // Explicit activation: false never touches a device and every request is Disabled.
    bool enabled = true;
    // Explicit native loader override (empty means the platform loader). No workspace/build path is
    // ever hardcoded. This is the same field `GpuDeviceCreationOptions::loader_path` uses.
    std::filesystem::path loaderPath;
    // Per-request LIVE unique pinned-byte budget handed to GpuSceneExecutor::begin(). The default
    // is the host-availability-derived budget above, never a fixed 1 GiB that would refuse a large
    // composition or over-commit a small host.
    std::uint64_t requestByteBudget = defaultGpuProcessFrameByteBudget();
    // Hard ceiling for the one final readback allocation (host staging + returned pixels).
    std::uint64_t readbackByteBudget = defaultGpuProcessFrameByteBudget();
    // Bounded per-native-job deadline while polling the executor.
    std::chrono::milliseconds nativeDeadline{30000};
    // Bounded admission: the maximum number of admitted-but-not-yet-running requests. A request
    // beyond this is refused with a typed OverBudget and no frame.
    std::size_t maxQueuedRequests = 16;
    GpuSceneCacheBudgets cacheBudgets{};
    // Per-operation native ceilings stay at the render layer's own bounded defaults (its per-image
    // hard cap is fixed there); only the request/readback budgets above scale with the host. A
    // composition larger than a single render operation is handled by the CPU reference path.
    GpuSceneExecutorBudgets executorBudgets{};
    // Off-UI resolved, immutable OCIO context (one shared preparer + qualified compile options) for
    // media-source colour conversion and image-effect transforms. Null fails every non-identity
    // transform closed, so the caller keeps the CPU reference path rather than mis-rendering.
    std::shared_ptr<const GpuSceneOcioContext> ocioContext;
    // Per-request media context. Preferred form is `mediaContextProvider`, which the evaluator
    // calls on the CALLING CPU worker during scene preparation (never on the GPU owner thread) so
    // the asset base directory follows a session Open/SaveAs exactly like the preview path;
    // `mediaContext` is the static fallback when no provider is set.
    GpuSceneMediaContext mediaContext;
    std::function<GpuSceneMediaContext()> mediaContextProvider;
    // Inert shared resolver the owning GpuExportProvider runs once on its CPU-worker bootstrap to
    // qualify the packaged tools and publish the context above. Never resolved by the evaluator,
    // and never consulted from the UI thread or this evaluator's owner thread.
    std::shared_ptr<GpuOcioContextResolver> ocioResolver;
    // Test-only deterministic fault seam: when non-zero, the Nth scene-preparation slot allocation
    // fails with a typed BadAllocation. Zero (the default) disables it and production never sets
    // it.
    std::uint32_t failPreparationAllocationAt = 0;
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

    // Prepares the immutable scene with the real CpuGpuSceneBuilder ON THE CALLING CPU WORKER
    // (media decode, OCIO configuration, and shader compilation never run on the GPU owner thread),
    // enqueues it under bounded admission, executes it on the owner worker, and performs exactly
    // one final combined readback. Never blocks on a Vulkan call from the caller thread. A scene
    // outside the prepared-GPU subset is `UnsupportedGpuSubset`; the caller falls back to the CPU
    // reference evaluator.
    //
    // `outputCommand` is an already-compiled, immutable OCIO output command prepared on a CPU task
    // before dispatch. It may be null (identity arm): only the exact process payload is read back.
    // When non-null the one combined submission also transfers the encoded output (process-effect
    // RGBA32F or straight display RGBA8) alongside the unchanged process payload.
    [[nodiscard]] GpuProcessFrameOutcome
    evaluate(std::shared_ptr<const CompiledCompositionPlan> plan, const EvaluationRequest& request,
             const CancellationToken& cancellation = {}, EvaluationProgressCallback progress = {},
             std::shared_ptr<const PreparedGpuOcioCommand> outputCommand = nullptr);

    // Non-blocking. The destructor joins the owner worker.
    void beginShutdown() noexcept;

    // Non-blocking. True once the owner worker has fully retired: it has left its request loop,
    // destroyed the device/cache/executor generation on the owner thread, and the thread function
    // has returned. A caller may release the evaluator once this is true without joining a live
    // owner thread. False while bootstrap or request work is still in progress, and false before
    // beginShutdown() has been observed by the owner loop.
    [[nodiscard]] bool retirementComplete() const noexcept;

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
