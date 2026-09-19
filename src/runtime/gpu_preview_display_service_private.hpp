#pragma once

// Private implementation surface shared by the two service translation units and the narrow test
// access hook. Nothing here is part of the public contract.

#include "gpu_preview_display_service_presentation_private.hpp"
#include "gpu_preview_display_service_resident_private.hpp"

#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/document/document.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_neutral_display.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/runtime/gpu_neutral_display_qualification.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>

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
    TaskHandle<PreviewGpuSceneStageOutcomeHandle> gpuStageChild;
    std::shared_ptr<const PreviewCpuStage> stage;
    std::shared_ptr<const GpuNeutralDisplayQualificationReport> report;
    // Resident route only: the prepared immutable GPU scene and its selected processor, plus the
    // resident native sub-state. `resident` is set when this record was started through the GPU
    // scene stage function.
    std::shared_ptr<const PreviewGpuSceneStage> gpuStage;
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> residentProcessor;
    bool resident = false;
    ResidentNativePhase residentPhase = ResidentNativePhase::None;
    // Bounded owner-thread retirement pumps spent while proving an unretired resident submission.
    std::size_t residentRetirePumps = 0;

    TaskPriority priority = TaskPriority::Background;
    TaskOwner owner;
    std::optional<TaskGroupId> groupId;
    TaskSourceVersion sourceVersion;
    std::optional<std::string> childCoalescingKey;
    std::size_t requestOwnedBytes = 0;
    std::size_t pixelStorageByteLimit = 0;
    // Retained only for the resident route so a GPU-subset refusal or a resident native failure can
    // take the SAME full original CPU path (compile + evaluate + display) without re-asking the
    // caller. The packed path keeps its own evaluated stage instead.
    std::optional<document::Snapshot> snapshot;
    PreviewRequestIdentity identity;
    std::vector<SnapshotParameterOverride> overrides;

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
    // Resident route: the GPU-scene CPU preparation seam. Empty selects the existing
    // packed/CPU-only service; non-empty selects the resident-route overload.
    PreviewGpuSceneStageFunction gpuStageFunction;

    std::shared_ptr<GpuExecutorLease> lease;

    // Service-thread-owned native ownership. Reset on the service thread before the loop returns.
    std::unique_ptr<render::GpuDevice> device;
    std::unique_ptr<render::GpuNeutralDisplay> display;
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> processor;

    // Resident route, service-thread-owned. Created only on the device owner thread when
    // `gpuStageFunction` is set; destroyed before `device`. `residentDisplay` is the single
    // GpuResidentDisplay used for both the startup qualification and every resident frame.
    std::unique_ptr<GpuSceneCache> residentSceneCache;
    std::unique_ptr<GpuSceneExecutor> residentExecutor;
    std::unique_ptr<render::GpuResidentDisplay> residentDisplay;
    // Genuine immutable resident qualification report, produced on the owner thread at startup
    // independently of the packed readback qualification. Never fabricated.
    std::shared_ptr<const GpuResidentPreviewQualificationReport> residentQualification;
    // Set on the owner thread when the resident route is terminal (device lost, or a native fence
    // could not be proven retired and the pipelines were destroyed). Future submits take the CPU
    // path; the lost executor is never retried.
    bool residentRouteTerminal = false;

    // Service-thread-owned presentation generation (registry + coordinator on the same device).
    // Null when presentation mode is Disabled or the capability is Unavailable. Destroyed on the
    // service thread before `device`; destruction order is coordinator then registry.
    std::unique_ptr<PreviewDisplayPresentation> presentation;
    // Test-only: a second owner-thread registry on the same device, used to prove that a valid
    // lease bound to a foreign registry is rejected by this service's coordinator. Never used in
    // production paths.
    std::unique_ptr<GpuResidentFrameLeaseRegistry> testForeignRegistry;
    // Set true by any thread in beginShutdown(); observed by the owner drain before it begins the
    // coordinator's bounded retirement. Kept separate so no non-atomic owner state is touched off
    // the owner thread.
    std::atomic_bool presentationShutdownRequested{false};
    // Owner-thread only: whether the coordinator's beginShutdown() has been issued, how many owner
    // pumps have been spent on retirement (bounded exactly like the coordinator's own drain
    // budget), and whether the generation is fully retired.
    bool presentationShutdownBegun = false;
    std::size_t presentationPumps = 0;
    bool presentationRetired = false;
    // True when a live presentation generation could not be proven retired within the bounded
    // drain. In that case the native device is deliberately retained (the process quarantine holds
    // native targets that reference it) and the service reports the retention so the host refuses
    // Qt teardown rather than pretending a safe acknowledgement.
    bool presentationRetirementUnproven = false;

    // State protected by `stateMutex`. The presentation fields are immutable copies published by
    // the owner thread for safe UI-thread reads.
    mutable std::mutex stateMutex;
    GpuPreviewDisplayServiceState state = GpuPreviewDisplayServiceState::Initializing;
    bool gpuAvailable = false;
    std::shared_ptr<const GpuNeutralDisplayQualificationReport> qualification;
    GpuPreviewDisplayServiceDiagnostic diagnostic;
    render::GpuPresentationAvailability publishedPresentationAvailability =
        render::GpuPresentationAvailability::NotRequested;
    std::string publishedPresentationDetail;
    std::shared_ptr<GpuPresentationClient> publishedPresentationClient;
    GpuPresentationShutdownStatus publishedPresentationShutdown;
    // Resident route published snapshot (owner-written, UI-readable).
    std::shared_ptr<const GpuResidentPreviewQualificationReport> publishedResidentQualification;
    std::string publishedResidentDetail;
    // Bounded cached counters, owner-written atomics and read locklessly/under lock by status().
    std::atomic<std::uint64_t> counterResidentGraphJobs{0};
    std::atomic<std::uint64_t> counterNativeDispatches{0};
    std::atomic<std::uint64_t> counterResidentCompletions{0};
    std::atomic<std::uint64_t> counterResidentFailures{0};
    std::atomic<std::uint64_t> counterGpuCacheHits{0};
    std::atomic<std::uint64_t> counterGpuCacheMisses{0};
    std::atomic<std::uint64_t> counterCpuFallbacks{0};
    std::atomic<std::uint64_t> counterFullFrameReadbacks{0};
    std::atomic<std::uint64_t> counterDisplayStatusReads{0};
    std::atomic<std::uint64_t> counterResidentLeaseRefusals{0};
    std::atomic<std::uint64_t> counterRetirementUnprovenTeardowns{0};
    std::atomic<std::uint64_t> counterResidentQualificationMicros{0};

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

    // Resident native job slot (independent of the packed slot; only one is ever used per service
    // generation because the resident overload does not build the packed display).
    std::deque<std::shared_ptr<PreviewDisplayStageRecord>> residentNativeReady;
    std::shared_ptr<PreviewDisplayStageRecord> residentNativeActive;
    bool residentNativeInFlight = false;

    // Narrow fault seam: injectable clock/poll for the bounded native-dispatch deadline.
    // Empty uses the real steady clock and the pipeline's own poll().
    std::function<std::chrono::steady_clock::time_point()> nativeClockOverride;
    std::function<render::GpuNeutralDisplayPollResult(render::GpuNeutralDisplay&)>
        nativePollOverride;
    // Resident-route fault seam (test-only; never set in production). The overrides let a test
    // force a deterministic stalled/unknown-fence retirement and observe that the production
    // Retiring code retains the stage, completion and request-owned admission until the override
    // reports proven retirement. Empty uses the real native accessors/poll.
    std::function<GpuSceneExecutorPollResult(GpuSceneExecutor&)> residentExecutorPollOverride;
    std::function<render::GpuResidentDisplayPollResult(render::GpuResidentDisplay&)>
        residentDisplayPollOverride;
    std::function<bool(const GpuSceneExecutor&)> residentExecutorUnretiredOverride;
    std::function<bool(const render::GpuResidentDisplay&)> residentDisplayUnretiredOverride;

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

// Resident-route parent starter (service thread). Submits the GPU-scene CPU preparation child and
// retains the final completion. The prepared scene is dispatched later on this same owner thread.
void startResidentPreviewStage(const std::shared_ptr<PreviewDisplayServiceCore>& core,
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

// Resident-route service-thread jobs.
void handleGpuStageChildResult(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                               const std::shared_ptr<PreviewDisplayStageRecord>& stage,
                               TaskResult<PreviewGpuSceneStageOutcomeHandle> result);
// Dispatches the FULL original CPU path for a resident stage whose GPU subset is unavailable: the
// CPU stage function is run again and then the display fallback, exactly the existing CPU pipeline.
void dispatchResidentCpuFallbackChild(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                      const std::shared_ptr<PreviewDisplayStageRecord>& stage);
// Owner-thread resident native pump: advances the GpuSceneExecutor -> GpuResidentDisplay -> product
// factory sequence for the active resident stage by at most one bounded step per call.
void processResidentNativeDisplay(const std::shared_ptr<PreviewDisplayServiceCore>& core);

// Copies the core's cached end-to-end counter snapshot into a status result. The caller must
// already hold stateMutex. Kept beside the core so status() stays a thin lifecycle projection.
void copyLifecycleCounters(const PreviewDisplayServiceCore& core,
                           GpuPreviewDisplayServiceCounters& out) noexcept;

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
    // Resident-route probes. Read-only owner-created state; no native object is exposed.
    static bool residentInFlight(const PreviewDisplayServiceCore& core) {
        return core.residentNativeInFlight;
    }
    static bool residentRetiring(const PreviewDisplayServiceCore& core) {
        return core.residentNativeActive != nullptr &&
               core.residentNativeActive->residentPhase == ResidentNativePhase::Retiring;
    }
    static bool residentRouteTerminal(const PreviewDisplayServiceCore& core) {
        return core.residentRouteTerminal;
    }
    static std::size_t residentReadyCount(const PreviewDisplayServiceCore& core) {
        return core.residentNativeReady.size();
    }
    static bool residentRouteAvailable(const PreviewDisplayServiceCore& core) {
        return core.residentExecutor != nullptr && core.residentDisplay != nullptr;
    }
    static std::shared_ptr<PreviewDisplayServiceCore>
    coreOf(const GpuPreviewDisplayService& service);

    // Presentation ownership probes. All read-only and owner-thread-created state; no native object
    // is exposed. `ownershipEpoch` is this exact device generation, `presentationReady` is whether
    // the owner actually created the registry + coordinator.
    static bool presentationReady(const PreviewDisplayServiceCore& core) {
        return core.presentation != nullptr && core.presentation->available;
    }
    static std::uint64_t ownershipEpoch(const PreviewDisplayServiceCore& core) {
        return core.device == nullptr ? 0U : core.device->ownershipEpoch();
    }
    static bool sameOwnerThread(const PreviewDisplayServiceCore& core) {
        return core.device != nullptr && core.device->isOwnerThread();
    }
    static std::shared_ptr<GpuPresentationClient>
    presentationClient(const PreviewDisplayServiceCore& core) {
        return core.presentation == nullptr ? nullptr : core.presentation->client;
    }
    static std::uint64_t registryEpoch(const PreviewDisplayServiceCore& core) {
        return core.presentation == nullptr || core.presentation->registry == nullptr
                   ? 0U
                   : core.presentation->registry->epoch();
    }

    // Test-only: run one actual owner-thread scheduler GPU task that produces a real resident image
    // and publishes an opaque lease into the service registry (`foreign == false`) or into a second
    // owner-thread registry on the same device (`foreign == true`, for foreign-lease rejection).
    [[nodiscard]] static bool requestPresentationTestLease(GpuPreviewDisplayService& service,
                                                           PresentationTestLeaseResult& out,
                                                           std::chrono::milliseconds timeout,
                                                           bool foreign = false);
};

} // namespace detail
} // namespace bloom::runtime
