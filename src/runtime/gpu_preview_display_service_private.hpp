#pragma once

// Private implementation surface shared by the two service translation units and the narrow test
// access hook. Nothing here is part of the public contract.

#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/document/document.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_neutral_display.hpp>
#include <bloom/runtime/gpu_neutral_display_qualification.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bloom::runtime {

class CpuCompositionEvaluator;

namespace detail {

inline constexpr std::size_t kMaxAcceptedServiceRoots = 512;

// One accepted GPU display request. All fields are touched only on the service thread except
// `completion` which the service thread consumes while the scheduler owns the caller's own parent
// handle independently.
struct PreviewDisplayStageRecord final {
    GpuTaskCompletion<PreviewPreparationResultHandle> completion;
    TaskHandle<PreviewCpuStageOutcomeHandle> stageChild;
    TaskHandle<PreviewPreparationResultHandle> fallbackChild;
    std::shared_ptr<const PreviewCpuStage> stage;
    std::shared_ptr<const GpuNeutralDisplayQualificationReport> report;

    TaskPriority priority = TaskPriority::Background;
    TaskOwner owner;
    std::optional<TaskGroupId> groupId;
    TaskSourceVersion sourceVersion;
    std::optional<std::string> childCoalescingKey;
    std::size_t requestOwnedBytes = 0;
    std::size_t pixelStorageByteLimit = 0;

    bool cancellationRequested = false;
    bool nativeDiscard = false;
    bool nativeDispatched = false;
    std::chrono::steady_clock::time_point nativeStartedAt{};

    enum class Phase : std::uint8_t {
        AwaitingStage,
        AwaitingNative,
        AwaitingFallback,
        Done,
    };
    Phase phase = Phase::AwaitingStage;
};

// Shared-owned service core. The loop, the scheduler wake sink, and every child task capture this
// by shared_ptr; no raw `this` of the public service is ever reachable from a scheduler thread.
struct PreviewDisplayServiceCore final {
    PreviewDisplayServiceCore() = default;
    PreviewDisplayServiceCore(const PreviewDisplayServiceCore&) = delete;
    PreviewDisplayServiceCore& operator=(const PreviewDisplayServiceCore&) = delete;

    // Immutable dependencies (must outlive the service).
    TaskScheduler* scheduler = nullptr;
    GpuServiceGeneration generation;
    GpuPreviewDisplayServiceOptions options;
    PreviewCpuStageFunction stageFunction;
    PreviewCpuDisplayFallback fallback;

    std::shared_ptr<GpuExecutorLease> lease;

    // Service-thread-owned native ownership. Reset on the service thread before the loop returns.
    std::unique_ptr<render::GpuDevice> device;
    std::unique_ptr<render::GpuNeutralDisplay> display;
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> processor;

    // State protected by `stateMutex`.
    mutable std::mutex stateMutex;
    GpuPreviewDisplayServiceState state = GpuPreviewDisplayServiceState::Initializing;
    bool gpuAvailable = false;
    std::shared_ptr<const GpuNeutralDisplayQualificationReport> qualification;
    GpuPreviewDisplayServiceDiagnostic diagnostic;

    // Wake generation / condition flag: a notification can never be lost between the loop's check
    // and its wait because the generation is bumped under this lock.
    std::mutex wakeMutex;
    std::condition_variable wakeCondition;
    std::uint64_t wakeGeneration = 0;
    std::atomic_bool stopping{false};
    std::atomic_bool shutdownRequested{false};

    // Service-thread-owned stage bookkeeping.
    std::vector<std::shared_ptr<PreviewDisplayStageRecord>> stages;
    std::deque<std::shared_ptr<PreviewDisplayStageRecord>> nativeReady;
    std::shared_ptr<PreviewDisplayStageRecord> nativeActive;
    bool nativeInFlight = false;

    // Narrow fault seam: injectable clock/poll for the bounded native-dispatch deadline.
    // Empty uses the real steady clock and the pipeline's own poll().
    std::function<std::chrono::steady_clock::time_point()> nativeClockOverride;
    std::function<render::GpuNeutralDisplayPollResult(render::GpuNeutralDisplay&)>
        nativePollOverride;

    // Root admission is synchronized with beginShutdown(): every accepted root (CPU fallback or
    // queued GPU parent) is either tracked and cancelled by shutdown, or -- if shutdown landed
    // between admission and registration -- the accepting thread cancels its own root. Active roots
    // are pruned only when terminal; they are never silently dropped.
    mutable std::mutex rootsMutex;
    bool shuttingDown = false;
    std::size_t pendingAdmissions = 0;
    std::vector<TaskId> acceptedRoots;

    [[nodiscard]] bool beginRootAdmission() noexcept;
    void finishRootAdmission(TaskId id, bool accepted) noexcept;
    void abandonRootAdmission() noexcept;
    void trackRoot(TaskId id) noexcept;
    void beginShutdownRoots(std::vector<TaskId>& out) noexcept;

    void notify() noexcept {
        {
            std::lock_guard lock(wakeMutex);
            ++wakeGeneration;
        }
        wakeCondition.notify_all();
    }

    [[nodiscard]] std::shared_ptr<const GpuNeutralDisplayQualificationReport>
    qualificationReport() const {
        std::lock_guard lock(stateMutex);
        return qualification;
    }

    void publishState(GpuPreviewDisplayServiceState next, bool available,
                      GpuPreviewDisplayServiceDiagnostic nextDiagnostic,
                      std::shared_ptr<const GpuNeutralDisplayQualificationReport> report) {
        {
            std::lock_guard lock(stateMutex);
            state = next;
            gpuAvailable = available;
            diagnostic = std::move(nextDiagnostic);
            if (report != nullptr) {
                qualification = std::move(report);
            }
        }
        notify();
    }
};

// The scheduler GPU startup task body: device bootstrap, exact embedded neutral CPU processor
// build, and cancellation-aware qualification, all on the service thread.
void runGpuStartup(const std::shared_ptr<PreviewDisplayServiceCore>& core, TaskContext& context,
                   GpuTaskCompletion<int> completion);

// GPU parent starter payload (runs on the service thread): submit the CPU stage child, retain the
// final completion, and return immediately.
struct PreviewStageSubmission final {
    PreviewStageSubmission(document::Snapshot snapshotValue, PreviewRequestIdentity identityValue,
                           std::size_t pixelStorageByteLimitValue,
                           std::vector<SnapshotParameterOverride> overridesValue,
                           TaskPriority priorityValue, TaskOwner ownerValue,
                           std::optional<TaskGroupId> groupIdValue,
                           TaskSourceVersion sourceVersionValue,
                           std::optional<std::string> coalescingKeyValue)
        : snapshot(std::move(snapshotValue)), identity(std::move(identityValue)),
          pixelStorageByteLimit(pixelStorageByteLimitValue), overrides(std::move(overridesValue)),
          priority(priorityValue), owner(ownerValue), groupId(groupIdValue),
          sourceVersion(sourceVersionValue), coalescingKey(std::move(coalescingKeyValue)) {}

    document::Snapshot snapshot;
    PreviewRequestIdentity identity;
    std::size_t pixelStorageByteLimit = 0;
    std::vector<SnapshotParameterOverride> overrides;
    TaskPriority priority = TaskPriority::Background;
    TaskOwner owner;
    std::optional<TaskGroupId> groupId;
    TaskSourceVersion sourceVersion;
    std::optional<std::string> coalescingKey;
};

void startGpuPreviewStage(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                          bool cancellationRequested,
                          GpuTaskCompletion<PreviewPreparationResultHandle> completion,
                          PreviewStageSubmission submission);

// Service-loop jobs (service thread only).
void processPreviewStages(const std::shared_ptr<PreviewDisplayServiceCore>& core);
void processNativeDisplay(const std::shared_ptr<PreviewDisplayServiceCore>& core);
void dispatchDisplayFallbackChild(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                  const std::shared_ptr<PreviewDisplayStageRecord>& stage);
void disableGpuAfterNativeFailure(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                  const std::string& detail);

// Conservative native selection. The service's active poll adds two handoff intervals the
// qualification timings do not include (stage child completion -> service, native fence ->
// service), so a raw "native faster" reading can still be a loss for sub-millisecond wins.
inline constexpr std::uint64_t kGpuPreviewActivePollMicros = 2000;

[[nodiscard]] bool
gpuPreviewDisplayRequestIsNeutral(const PreviewRequestIdentity& identity) noexcept;
[[nodiscard]] std::optional<std::uint64_t>
gpuPreviewDisplayEstimatePixels(const PreviewRequestIdentity& identity) noexcept;
[[nodiscard]] std::uint64_t gpuPreviewDisplayHandoffOverheadMicros() noexcept;
[[nodiscard]] bool
gpuPreviewDisplaySelectsNative(const GpuNeutralDisplayQualificationReport& report,
                               std::uint64_t pixels, std::uint64_t overheadMicros) noexcept;

// Shared test fixture moved out of the test TU to keep each file readable: real compiled solid
// plan, real evaluator frame, real qualified CPU processor, with evaluation/fallback counters.
[[nodiscard]] std::shared_ptr<const ProcessFrame>
testEvaluateFrame(const std::shared_ptr<const CompiledCompositionPlan>& plan, std::size_t budget);
[[nodiscard]] PreviewRequestIdentity testIdentity(const CompiledCompositionPlan& plan);
class PreviewDisplayServiceTestFixture final {
  public:
    PreviewDisplayServiceTestFixture(std::uint32_t width, std::uint32_t height);
    std::shared_ptr<const CompiledCompositionPlan> plan;
    std::shared_ptr<CpuCompositionEvaluator> evaluator;
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> processor;
    std::shared_ptr<std::atomic<int>> evaluations = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> fallbacks = std::make_shared<std::atomic<int>>(0);
    [[nodiscard]] PreviewCpuStageFunction stageFn() const;
    [[nodiscard]] PreviewCpuDisplayFallback fallback() const;
};

// Bounded native-dispatch-deadline scenario: builds a real device/pipeline/qualification on the
// calling thread and drives the private native pump with an injected clock/poll so the deadline
// retirement path is verified without a fabricated eligibility report. `ran == false` means no
// compatible device was available.
struct NativeDeadlineScenarioResult final {
    bool ran = false;
    bool dispatched = false;
    bool retired = false;
    bool timeoutDiagnostic = false;
    bool fallbackDispatched = false;
    std::string message;
};
[[nodiscard]] NativeDeadlineScenarioResult
runNativeDeadlineScenario(const std::filesystem::path& loader);

// Paired service-submit vs ordinary-CPU benchmark preparation over one warm graph. Medians include
// task scheduling and the service handoff. `usedGpu` records actual GPU provenance.
struct PreviewDisplayBenchmarkSample final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double serviceMedianMs = 0.0;
    double cpuMedianMs = 0.0;
    bool usedGpu = false;
    bool ran = false;
    bool succeeded = true;
    std::size_t cacheHits = 0;
};
[[nodiscard]] std::vector<PreviewDisplayBenchmarkSample>
runGpuPreviewDisplayBenchmark(const std::filesystem::path& loader);

// Narrow test access hook. Tests may hold a completion/child retirement open to prove accounting is
// not released early. It fabricates nothing about qualification.
struct GpuPreviewDisplayServiceTestAccess final {
    static void holdChildRetirement(const std::shared_ptr<PreviewDisplayStageRecord>& stage,
                                    const bool hold) {
        stage->phase = hold ? PreviewDisplayStageRecord::Phase::AwaitingStage
                            : PreviewDisplayStageRecord::Phase::Done;
    }
    static bool isNativeInFlight(const PreviewDisplayServiceCore& core) {
        return core.nativeInFlight;
    }
    static std::size_t stageCount(const PreviewDisplayServiceCore& core) {
        return core.stages.size();
    }
    static std::shared_ptr<PreviewDisplayServiceCore>
    coreOf(const GpuPreviewDisplayService& service);
};

} // namespace detail
} // namespace bloom::runtime
