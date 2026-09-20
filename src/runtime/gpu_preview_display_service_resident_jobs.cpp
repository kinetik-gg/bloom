// Resident-route service jobs, extracted from gpu_preview_display_service_jobs.cpp so the packed
// and resident paths stay cohesive and reviewable. Every function here is called on the service
// owner thread. The central safety invariant is that a stage's GpuTaskCompletion is NEVER consumed
// (and therefore GPU request-owned admission is never released) while a native submission it
// started may still be unretired: a failure/deadline/cancellation/lost generation moves the stage
// into an explicit Retiring phase that retains the stage, its completion, its
// snapshot/identity/overrides and the native pins until retirement is actually proven, or the owned
// pipelines are destroyed on the owner thread so their destructors perform the bounded
// drain/quarantine.

#include "gpu_preview_display_service_private.hpp"

#include <bloom/runtime/prepared_preview_frame.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::runtime {
namespace detail {
namespace {

using StagePhase = PreviewDisplayStageRecord::Phase;

[[nodiscard]] TaskDiagnostic residentJobDiagnostic(std::string code, std::string summary,
                                                   std::string detail = {}) {
    return {.code = std::move(code),
            .severity = DiagnosticSeverity::Error,
            .summary = std::move(summary),
            .detail = std::move(detail),
            .suggestedAction = "Review the GPU preview display service resident diagnostics."};
}

// Publish the concrete refusal reason for a resident-stage failure. Without this the service
// reports an empty residentDetail and the actual GpuSceneExecutor/native diagnostic is lost.
void publishResidentJobDetail(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                              std::string detail) {
    std::lock_guard lock(core->stateMutex);
    core->publishedResidentDetail = std::move(detail);
}

// The executor's own diagnostic (code + message) plus the actual peak live bytes it charged
// against the per-request allowance: the required-vs-admitted evidence a refusal lacks otherwise.
[[nodiscard]] std::string executorFailureDetail(const GpuSceneExecutor& executor,
                                                const std::uint64_t admittedBytes,
                                                std::string_view prefix) {
    const auto diagnostic = executor.diagnostic();
    const auto counters = executor.counters();
    std::string detail(prefix);
    if (!diagnostic.message.empty()) {
        detail += ": ";
        detail += diagnostic.message;
    }
    detail += " [code=";
    detail += std::to_string(static_cast<unsigned>(diagnostic.code));
    detail += ", admittedBytes=";
    detail += std::to_string(admittedBytes);
    detail += ", peakLiveBytes=";
    detail += std::to_string(counters.peakLiveImageBytes);
    detail += ", liveBytes=";
    detail += std::to_string(counters.currentLiveImageBytes);
    detail += ']';
    return detail;
}

[[nodiscard]] std::uint64_t
residentStageAdmittedBytes(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                           const PreviewDisplayStageRecord& stage) noexcept {
    return stage.gpuByteAllowance != 0 ? stage.gpuByteAllowance : core->previewByteAllowance();
}

void completeCancelled(const std::shared_ptr<PreviewDisplayStageRecord>& stage,
                       std::vector<TaskDiagnostic> diagnostics = {}) {
    if (stage->completion.isValid()) {
        static_cast<void>(std::move(stage->completion).cancel(std::move(diagnostics)));
    }
    stage->phase = StagePhase::Done;
}

void completeFailed(const std::shared_ptr<PreviewDisplayStageRecord>& stage,
                    std::vector<TaskDiagnostic> diagnostics) {
    if (diagnostics.empty()) {
        diagnostics.push_back(residentJobDiagnostic("bloom.runtime.gpu-preview-display-failed",
                                                    "The GPU preview resident request failed."));
    }
    if (stage->completion.isValid()) {
        static_cast<void>(std::move(stage->completion)
                              .complete(TaskResult<PreviewPreparationResultHandle>::failed(
                                  std::move(diagnostics))));
    }
    stage->phase = StagePhase::Done;
}

[[nodiscard]] std::chrono::steady_clock::time_point
nativeNow(const std::shared_ptr<PreviewDisplayServiceCore>& core) {
    return core->nativeClockOverride ? core->nativeClockOverride()
                                     : std::chrono::steady_clock::now();
}

[[nodiscard]] std::shared_ptr<PreviewDisplayStageRecord>
pickResidentReadyStage(const std::shared_ptr<PreviewDisplayServiceCore>& core) {
    auto best = core->residentNativeReady.end();
    for (auto it = core->residentNativeReady.begin(); it != core->residentNativeReady.end(); ++it) {
        if ((*it)->cancellationRequested) {
            best = it;
            break;
        }
        if (best == core->residentNativeReady.end() ||
            static_cast<int>((*it)->priority) < static_cast<int>((*best)->priority)) {
            best = it;
        }
    }
    if (best == core->residentNativeReady.end()) {
        return nullptr;
    }
    auto stage = *best;
    core->residentNativeReady.erase(best);
    return stage;
}

// A failure with NO unretired native submission: safe to release admission immediately.
void finishResidentImmediately(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                               const std::shared_ptr<PreviewDisplayStageRecord>& stage,
                               const bool nativeFailure) {
    if (nativeFailure) {
        core->counterResidentFailures.fetch_add(1, std::memory_order_relaxed);
    }
    if (stage->cancellationRequested || stage->nativeDiscard ||
        core->stopping.load(std::memory_order_acquire)) {
        completeCancelled(stage);
        return;
    }
    core->counterCpuFallbacks.fetch_add(1, std::memory_order_relaxed);
    dispatchResidentCpuFallbackChild(core, stage);
}

// A failure/deadline/lost generation while a native submission may still be unretired. Retain the
// stage (and its completion, snapshot, identity, overrides and pins) in Retiring and cancel the
// native pipelines; the completion is released only once retirement is proven or the pipelines are
// destroyed.
void beginResidentRetirement(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                             const std::shared_ptr<PreviewDisplayStageRecord>& stage) {
    if (stage->residentPhase == ResidentNativePhase::Retiring) {
        return;
    }
    if (residentDeviceLost(core)) {
        core->residentRouteTerminal = true;
    } else {
        core->counterResidentFailures.fetch_add(1, std::memory_order_relaxed);
    }
    residentCancelNative(core);
    stage->nativeDiscard = stage->nativeDiscard || stage->cancellationRequested ||
                           core->stopping.load(std::memory_order_acquire);
    stage->residentRetirePumps = 0;
    stage->residentPhase = ResidentNativePhase::Retiring;
    core->residentNativeActive = stage;
    core->residentNativeInFlight = true;
}

// Retiring is done: either every native submission is proven retired, or the owned pipelines were
// destroyed on this owner thread (their destructors performed the bounded drain/quarantine). Only
// now may the parent completion be consumed and the request-owned admission released.
void finishResidentRetirement(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                              const std::shared_ptr<PreviewDisplayStageRecord>& stage) {
    const bool cancelled = stage->cancellationRequested || stage->nativeDiscard ||
                           core->stopping.load(std::memory_order_acquire);
    core->residentNativeActive.reset();
    core->residentNativeInFlight = false;
    stage->residentPhase = ResidentNativePhase::Done;
    if (cancelled) {
        completeCancelled(stage);
        return;
    }
    core->counterCpuFallbacks.fetch_add(1, std::memory_order_relaxed);
    dispatchResidentCpuFallbackChild(core, stage);
}

// Final bounded teardown when a fence could not be proven retired: the owned display/executor are
// destroyed on the owner thread and perform their own bounded drain/quarantine. The route becomes
// terminal so no future submit retries the lost pipeline.
void teardownTerminalResidentRoute(const std::shared_ptr<PreviewDisplayServiceCore>& core) {
    core->residentRouteTerminal = true;
    core->counterRetirementUnprovenTeardowns.fetch_add(1, std::memory_order_relaxed);
    retireResidentRoute(core);
}

} // namespace

// -----------------------------------------------------------------------------------------------
// GPU-scene CPU preparation child (service-thread submission, CPU worker execution)
// -----------------------------------------------------------------------------------------------

void startResidentPreviewStage(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                               const bool cancellationRequested,
                               GpuTaskCompletion<PreviewPreparationResultHandle> completion,
                               PreviewStageSubmission submission) {
    auto stage = std::make_shared<PreviewDisplayStageRecord>();
    stage->completion = std::move(completion);
    stage->priority = submission.priority;
    stage->owner = submission.owner;
    stage->groupId = submission.groupId;
    stage->sourceVersion = submission.sourceVersion;
    stage->requestOwnedBytes = submission.gpuByteAllowance;
    stage->pixelStorageByteLimit = submission.pixelStorageByteLimit;
    stage->gpuByteAllowance = submission.gpuByteAllowance;
    stage->resident = true;
    // Retained so a GPU-subset refusal or a resident native failure can take the full original CPU
    // path without re-asking the caller, using the same snapshot/identity/overrides.
    stage->snapshot = submission.snapshot;
    stage->identity = submission.identity;
    stage->overrides = submission.overrides;
    if (submission.coalescingKey.has_value() && !submission.coalescingKey->empty()) {
        stage->childCoalescingKey = *submission.coalescingKey + ".gpuscene";
    }

    if (cancellationRequested) {
        completeCancelled(stage);
        return;
    }

    TaskRequest childRequest("GPU preview GPU-scene stage", submission.owner, submission.priority,
                             TaskExecutor::Cpu);
    childRequest.groupId = submission.groupId;
    childRequest.sourceVersion = submission.sourceVersion;
    childRequest.coalescingKey = stage->childCoalescingKey;
    try {
        auto child = core->scheduler->submit<PreviewGpuSceneStageOutcomeHandle>(
            std::move(childRequest),
            [core, snapshot = std::move(submission.snapshot),
             identity = std::move(submission.identity), limit = submission.pixelStorageByteLimit,
             overrides = std::move(submission.overrides)](
                TaskContext& context) -> TaskResult<PreviewGpuSceneStageOutcomeHandle> {
                using Result = TaskResult<PreviewGpuSceneStageOutcomeHandle>;
                if (context.isCancellationRequested()) {
                    return Result::cancelled();
                }
                auto result = core->gpuStageFunction(snapshot, identity, limit, overrides, context);
                core->notify();
                return result;
            });
        if (!child.accepted()) {
            completeFailed(stage, child.diagnostic.has_value()
                                      ? std::vector<TaskDiagnostic>{*child.diagnostic}
                                      : std::vector<TaskDiagnostic>{residentJobDiagnostic(
                                            "bloom.runtime.gpu-preview-child-rejected",
                                            "The GPU-scene stage child was not admitted.")});
            return;
        }
        stage->gpuStageChild = child.handle;
        core->stages.push_back(std::move(stage));
    } catch (...) {
        completeFailed(stage,
                       {residentJobDiagnostic("bloom.runtime.gpu-preview-child-threw",
                                              "The GPU-scene stage child submission raised.")});
    }
}

// -----------------------------------------------------------------------------------------------
// Full CPU fallback child for the resident route
// -----------------------------------------------------------------------------------------------

void dispatchResidentCpuFallbackChild(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                      const std::shared_ptr<PreviewDisplayStageRecord>& stage) {
    if (core->stopping.load(std::memory_order_acquire) || stage->cancellationRequested) {
        completeCancelled(stage);
        return;
    }
    TaskRequest request("GPU preview resident CPU fallback", stage->owner, stage->priority,
                        TaskExecutor::Cpu);
    request.groupId = stage->groupId;
    request.sourceVersion = stage->sourceVersion;
    request.coalescingKey = stage->childCoalescingKey;
    try {
        auto child = core->scheduler->submit<PreviewPreparationResultHandle>(
            std::move(request), [core, stage](TaskContext& context) {
                using Result = TaskResult<PreviewPreparationResultHandle>;
                if (context.isCancellationRequested()) {
                    return Result::cancelled();
                }
                if (!stage->snapshot.has_value()) {
                    return Result::failed(
                        residentJobDiagnostic("bloom.runtime.gpu-preview-missing-snapshot",
                                              "The resident fallback has no snapshot."));
                }
                auto stageResult =
                    core->stageFunction(*stage->snapshot, stage->identity,
                                        stage->pixelStorageByteLimit, stage->overrides, context);
                if (stageResult.state() == TaskState::Cancelled) {
                    return Result::cancelled(stageResult.diagnostics());
                }
                if (stageResult.state() == TaskState::Failed) {
                    return Result::failed(stageResult.diagnostics());
                }
                const auto& outcome = stageResult.value();
                if (!outcome.has_value() || *outcome == nullptr) {
                    core->notify();
                    return Result::failed(
                        residentJobDiagnostic("bloom.runtime.gpu-preview-missing-stage",
                                              "The CPU stage returned no outcome."));
                }
                if ((*outcome)->status == PreviewCpuStageStatus::Unsupported) {
                    auto unsupported = std::make_shared<const PreviewPreparationResult>(
                        PreviewPreparationResult::unsupported());
                    auto result =
                        Result::succeeded(std::move(unsupported), (*outcome)->diagnostics);
                    core->notify();
                    return result;
                }
                if ((*outcome)->stage == nullptr) {
                    core->notify();
                    return Result::failed(
                        residentJobDiagnostic("bloom.runtime.gpu-preview-missing-frame",
                                              "The CPU stage produced no evaluated frame."));
                }
                auto result = core->fallback(*(*outcome)->stage, context);
                core->notify();
                return result;
            });
        if (!child.accepted()) {
            completeFailed(stage, child.diagnostic.has_value()
                                      ? std::vector<TaskDiagnostic>{*child.diagnostic}
                                      : std::vector<TaskDiagnostic>{});
            return;
        }
        stage->fallbackChild = child.handle;
        stage->phase = StagePhase::AwaitingFallback;
    } catch (...) {
        completeFailed(stage,
                       {residentJobDiagnostic("bloom.runtime.gpu-preview-fallback-threw",
                                              "The resident CPU fallback submission raised.")});
    }
}

// -----------------------------------------------------------------------------------------------
// GPU-scene stage result handling
// -----------------------------------------------------------------------------------------------

void handleGpuStageChildResult(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                               const std::shared_ptr<PreviewDisplayStageRecord>& stage,
                               const TaskResult<PreviewGpuSceneStageOutcomeHandle>& result) {
    using Result = TaskResult<PreviewPreparationResultHandle>;
    switch (result.state()) {
    case TaskState::Cancelled:
        completeCancelled(stage, result.diagnostics());
        return;
    case TaskState::Failed:
        completeFailed(stage, result.diagnostics());
        return;
    case TaskState::Succeeded:
        break;
    default:
        completeFailed(stage, {});
        return;
    }

    const auto& value = result.value();
    if (!value.has_value() || *value == nullptr) {
        completeFailed(stage, {residentJobDiagnostic("bloom.runtime.gpu-preview-missing-stage",
                                                     "The GPU-scene stage returned no outcome.")});
        return;
    }
    const auto& outcome = **value;
    if (outcome.status == PreviewGpuSceneStageStatus::Unsupported) {
        auto unsupported = std::make_shared<const PreviewPreparationResult>(
            PreviewPreparationResult::unsupported());
        if (stage->completion.isValid()) {
            static_cast<void>(
                std::move(stage->completion)
                    .complete(Result::succeeded(std::move(unsupported), outcome.diagnostics)));
        }
        stage->phase = StagePhase::Done;
        return;
    }
    if (outcome.status == PreviewGpuSceneStageStatus::UnsupportedGpuSubset ||
        outcome.stage == nullptr) {
        // The composition compiled but is outside the prepared GPU subset: take the FULL original
        // CPU path. No fake empty frame and no silently different subset.
        core->counterCpuFallbacks.fetch_add(1, std::memory_order_relaxed);
        dispatchResidentCpuFallbackChild(core, stage);
        return;
    }
    stage->gpuStage = outcome.stage;
    stage->residentProcessor = outcome.stage->displayProcessor();

    if (stage->cancellationRequested) {
        completeCancelled(stage);
        return;
    }

    std::string reason;
    const bool eligible = residentStageIsEligible(core, *stage, reason);
    if (!eligible || core->residentNativeReady.size() >= core->options.readyStageQueueCapacity) {
        core->counterCpuFallbacks.fetch_add(1, std::memory_order_relaxed);
        const std::string detail =
            eligible
                ? std::string("the resident ready-stage queue is full")
                : (reason.empty() ? std::string("the resident stage was not eligible") : reason);
        {
            std::lock_guard lock(core->stateMutex);
            core->publishedResidentDetail = "resident stage ineligible: " + detail;
        }
        dispatchResidentCpuFallbackChild(core, stage);
        return;
    }
    stage->phase = StagePhase::AwaitingNative;
    core->residentNativeReady.push_back(stage);
}

// -----------------------------------------------------------------------------------------------
// Resident native pump (one resident job in flight; safe retirement)
// -----------------------------------------------------------------------------------------------

void processResidentNativeDisplay(const std::shared_ptr<PreviewDisplayServiceCore>& core) {
    // The general display route needs only the scene executor; the Neutral fast path additionally
    // requires the resident display (checked per-stage in
    // residentStageIsEligible/residentDisplayBegin).
    if (!core->residentNativeReady.empty() &&
        (core->residentExecutor == nullptr || core->stopping.load(std::memory_order_acquire))) {
        while (!core->residentNativeReady.empty()) {
            auto stage = core->residentNativeReady.front();
            core->residentNativeReady.pop_front();
            finishResidentImmediately(core, stage, false);
        }
    }
    if (core->residentExecutor == nullptr) {
        return;
    }

    if (!core->residentNativeInFlight) {
        auto stage = pickResidentReadyStage(core);
        if (stage == nullptr) {
            return;
        }
        if (stage->cancellationRequested || core->stopping.load(std::memory_order_acquire)) {
            completeCancelled(stage);
            return;
        }
        std::string reason;
        if (!residentExecutorBegin(core, *stage, reason)) {
            const std::uint64_t admitted = residentStageAdmittedBytes(core, *stage);
            const std::uint64_t live = core->residentExecutor != nullptr
                                           ? core->residentExecutor->counters().peakLiveImageBytes
                                           : 0U;
            publishResidentJobDetail(core, "resident scene executor begin refused: " + reason +
                                               " [admittedBytes=" + std::to_string(admitted) +
                                               ", peakLiveBytes=" + std::to_string(live) + ']');
            if (residentDeviceLost(core)) {
                teardownTerminalResidentRoute(core);
                finishResidentImmediately(core, stage, true);
            } else if (residentExecutorHasUnretiredSubmission(core) ||
                       residentOwnerDrainRequired(core)) {
                // A previous unretired submission blocks reuse: retire before releasing this one.
                beginResidentRetirement(core, stage);
            } else {
                finishResidentImmediately(core, stage, true);
            }
            return;
        }
        stage->nativeDispatched = true;
        stage->nativeStartedAt = nativeNow(core);
        stage->residentPhase = ResidentNativePhase::Executor;
        core->residentNativeActive = stage;
        core->residentNativeInFlight = true;
        return;
    }

    auto stage = core->residentNativeActive;
    if (stage == nullptr) {
        core->residentNativeInFlight = false;
        return;
    }

    const bool cancelled = stage->cancellationRequested || stage->nativeDiscard ||
                           core->stopping.load(std::memory_order_acquire);
    switch (stage->residentPhase) {
    case ResidentNativePhase::Executor: {
        const auto poll = residentExecutorPoll(core);
        if (poll == GpuSceneExecutorPollResult::Pending) {
            if (cancelled ||
                nativeNow(core) - stage->nativeStartedAt >= core->options.nativeDispatchDeadline) {
                beginResidentRetirement(core, stage);
            }
            return;
        }
        noteResidentExecutorCounters(core);
        if (residentDeviceLost(core)) {
            beginResidentRetirement(core, stage);
            return;
        }
        if (poll != GpuSceneExecutorPollResult::Ready) {
            if (core->residentExecutor != nullptr) {
                publishResidentJobDetail(
                    core, executorFailureDetail(*core->residentExecutor,
                                                residentStageAdmittedBytes(core, *stage),
                                                "resident scene executor failed"));
            }
            beginResidentRetirement(core, stage);
            return;
        }
        if (stage->cancellationRequested || stage->nativeDiscard) {
            beginResidentRetirement(core, stage);
            return;
        }
        std::string reason;
        if (!residentDisplayBegin(core, *stage, reason)) {
            publishResidentJobDetail(
                core, "resident display begin refused: " + reason + " [admittedBytes=" +
                          std::to_string(residentStageAdmittedBytes(core, *stage)) + ']');
            if (residentDisplayHasUnretiredSubmission(core) ||
                residentExecutorHasUnretiredSubmission(core) || residentOwnerDrainRequired(core)) {
                beginResidentRetirement(core, stage);
            } else {
                core->residentNativeInFlight = false;
                core->residentNativeActive.reset();
                stage->residentPhase = ResidentNativePhase::Done;
                finishResidentImmediately(core, stage, true);
            }
            return;
        }
        stage->residentPhase = ResidentNativePhase::Display;
        stage->nativeStartedAt = nativeNow(core);
        return;
    }
    case ResidentNativePhase::Display: {
        const auto poll = residentDisplayPoll(core);
        if (poll == render::GpuResidentDisplayPollResult::Pending) {
            if (cancelled ||
                nativeNow(core) - stage->nativeStartedAt >= core->options.nativeDispatchDeadline) {
                beginResidentRetirement(core, stage);
            }
            return;
        }
        // The display submission is retired (or never submitted); no unretired pin remains.
        core->residentNativeInFlight = false;
        core->residentNativeActive.reset();
        stage->residentPhase = ResidentNativePhase::Done;
        if (cancelled) {
            core->counterResidentFailures.fetch_add(1, std::memory_order_relaxed);
            completeCancelled(stage);
            return;
        }
        if (poll == render::GpuResidentDisplayPollResult::Ready) {
            std::string reason;
            auto frame = residentFinishFrame(core, *stage, reason);
            if (frame.has_value()) {
                auto prepared = PreviewPreparationResult::prepared(
                    std::make_shared<const PreparedPreviewFrame>(std::move(*frame)));
                if (prepared.has_value()) {
                    if (stage->completion.isValid()) {
                        static_cast<void>(
                            std::move(stage->completion)
                                .succeed(std::make_shared<const PreviewPreparationResult>(
                                    std::move(*prepared))));
                    }
                    stage->phase = StagePhase::Done;
                    return;
                }
            }
            if (!reason.empty()) {
                publishResidentJobDetail(core, "resident display frame refused: " + reason);
            }
        }
        finishResidentImmediately(core, stage, true);
        return;
    }
    case ResidentNativePhase::Retiring: {
        // Advance retirement on the owner thread without blocking. A Ready executor output whose
        // stage was cancelled is taken and discarded so the executor returns to Idle. The
        // authoritative test is hasUnretiredSubmission(), NOT state(): a logical Failure can leave
        // an unretired submission behind and ownerDrainRequired() does not latch an ordinary
        // cancel.
        if (core->residentExecutor != nullptr &&
            core->residentExecutor->state() == GpuSceneExecutorJobState::Ready) {
            static_cast<void>(core->residentExecutor->takeImage());
        }
        if (core->residentExecutor != nullptr &&
            (core->residentExecutor->state() == GpuSceneExecutorJobState::Pending ||
             residentExecutorHasUnretiredSubmission(core) || residentOwnerDrainRequired(core))) {
            static_cast<void>(residentExecutorPoll(core));
        }
        if (residentDisplayHasUnretiredSubmission(core)) {
            static_cast<void>(residentDisplayPoll(core));
        }
        if (residentDeviceLost(core)) {
            teardownTerminalResidentRoute(core);
            finishResidentRetirement(core, stage);
            return;
        }
        const bool executorRetired =
            core->residentExecutor == nullptr ||
            (core->residentExecutor->state() != GpuSceneExecutorJobState::Pending &&
             !residentExecutorHasUnretiredSubmission(core) && !residentOwnerDrainRequired(core));
        const bool displayRetired = !residentDisplayHasUnretiredSubmission(core);
        if (executorRetired && displayRetired) {
            finishResidentRetirement(core, stage);
            return;
        }
        ++stage->residentRetirePumps;
        if (stage->residentRetirePumps >= kResidentRetirementPumpBudget) {
            // Bounded owner-thread teardown: the pipeline destructors drain/quarantine. This is the
            // only point at which an unproven fence is abandoned, and it is explicit and counted.
            teardownTerminalResidentRoute(core);
            finishResidentRetirement(core, stage);
            return;
        }
        return;
    }
    case ResidentNativePhase::None:
    case ResidentNativePhase::Done:
    default:
        return;
    }
}

bool GpuPreviewDisplayServiceTestAccess::sampleResidentFrameSparse(
    GpuPreviewDisplayService& service, const GpuResidentFrameLease& lease,
    const std::span<const render::ImagePixelCoordinate> coordinates,
    ResidentSparseSampleResult& out, const std::chrono::milliseconds timeout) {
    out = ResidentSparseSampleResult{};
    auto core = coreOf(service);
    if (core == nullptr) {
        out.diagnostic = "the service has no core";
        return false;
    }
    if (core->presentation == nullptr || !core->presentation->available ||
        core->presentation->registry == nullptr) {
        out.diagnostic = "the service owns no live presentation generation";
        return false;
    }
    if (core->scheduler == nullptr || !core->generation.isValid()) {
        out.diagnostic = "the service has no attached GPU executor";
        return false;
    }
    if (coordinates.empty()) {
        out.diagnostic = "the sparse sample requested no coordinates";
        return false;
    }
    auto shared = std::make_shared<ResidentSparseSampleResult>();
    auto done = std::make_shared<std::atomic_bool>(false);
    auto sampleCoordinates = std::make_shared<std::vector<render::ImagePixelCoordinate>>(
        coordinates.begin(), coordinates.end());
    static std::atomic<std::uint64_t> nextSampleRequest{1};
    const std::uint64_t requestId = nextSampleRequest.fetch_add(1, std::memory_order_relaxed);
    TaskRequest request("GPU preview resident sparse sample",
                        TaskOwner{.kind = TaskOwnerKind::Application,
                                  .id = TaskOwnerId::fromRaw(core->generation.value())},
                        TaskPriority::Interactive, TaskExecutor::Gpu);
    request.coalescingKey = "bloom.preview.gpu.resident.sparse." + std::to_string(requestId);
    auto submission = core->scheduler->submitGpu<int>(
        std::move(request), core->generation, GpuTaskAdmission{0, 0},
        [core, shared, done, lease, sampleCoordinates](TaskContext&,
                                                       GpuTaskCompletion<int> completion) {
            // Pin the lease on the owner thread so the native image cannot be reclaimed while the
            // sparse copy runs; the pin is released at the end of this owner-thread scope.
            const auto pinned = core->presentation->registry->pin(lease);
            if (!pinned.hasValue()) {
                shared->diagnostic = pinned.diagnostic.message;
                static_cast<void>(
                    std::move(completion)
                        .fail(residentJobDiagnostic(
                            "bloom.runtime.gpu-preview-resident-sparse-sample",
                            "The resident frame could not be pinned for a sparse sample.",
                            shared->diagnostic)));
                done->store(true, std::memory_order_release);
                return;
            }
            const auto readback = render::readbackResidentDisplayImageSparse(
                pinned.pin.image(), *sampleCoordinates,
                sampleCoordinates->size() * sizeof(render::Rgba8));
            if (!readback.hasValue()) {
                shared->diagnostic = readback.message;
                static_cast<void>(
                    std::move(completion)
                        .fail(residentJobDiagnostic(
                            "bloom.runtime.gpu-preview-resident-sparse-sample",
                            "The resident frame sparse sample failed.", shared->diagnostic)));
                done->store(true, std::memory_order_release);
                return;
            }
            shared->pixels = readback.pixels;
            shared->width = pinned.pin.image().width();
            shared->height = pinned.pin.image().height();
            shared->ran = true;
            static_cast<void>(std::move(completion).succeed(0));
            done->store(true, std::memory_order_release);
        });
    if (!submission.accepted()) {
        out.diagnostic = "the resident sparse sample task was not admitted";
        return false;
    }
    core->notify();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done->load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        core->notify();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!done->load(std::memory_order_acquire)) {
        out.diagnostic = "timed out waiting for the owner-thread resident sparse sample";
        return false;
    }
    out = std::move(*shared);
    return out.ran;
}

} // namespace detail
} // namespace bloom::runtime
