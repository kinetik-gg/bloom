// Resident-route service jobs, extracted from gpu_preview_display_service_jobs.cpp so the packed
// and resident paths stay cohesive and reviewable. Every function here is called on the service
// owner thread. Normal operation keeps a stage's completion/admission while native work may still
// be unretired: a failure/deadline/cancellation/lost generation moves the stage
// into an explicit Retiring phase that retains the stage, its completion, its
// snapshot/identity/overrides and the native pins until retirement is actually proven, or the owned
// pipelines are destroyed on the owner thread so their destructors perform the bounded
// drain/quarantine. Bounded shutdown of final packed transfers explicitly hands physical ownership
// to an independent, still-charged transfer reservation before releasing unrelated stage context.

#include "gpu_preview_display_service_cpu_phase.hpp"
#include "gpu_preview_display_service_private.hpp"

#include <bloom/runtime/prepared_preview_frame.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "gpu_preview_display_service_resident_job_diagnostics.hpp"

namespace bloom::runtime {
namespace detail {
namespace {

using StagePhase = PreviewDisplayStageRecord::Phase;

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

// The allocator-generation slot protects all native quarantine families, including synchronous
// OCIO LUT upload. Planning alone submits nothing. Once poll/display/transfer can submit, a failure
// without whole-phase proof is sticky even when a wrapper's logical state no longer reports work.
void releaseProcessingPhase(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                            const std::shared_ptr<PreviewDisplayStageRecord>& stage,
                            const bool proven) noexcept {
    if (stage->processingPhaseArmed && core->device && stage->gpuStage &&
        stage->gpuStage->phaseLease()) {
        if (proven && !residentDeviceLost(core) && !residentOwnerDrainRequired(core) &&
            !residentExecutorHasUnretiredSubmission(core) &&
            !residentDisplayHasUnretiredSubmission(core) &&
            !residentDisplayHasUnretiredSubmissionGeneral(core)) {
            // Cancellation may retire a display submission without consuming its output. Its
            // unpublished image/staging tokens still own the phase, so dispose them before the
            // device drops its lease; waiting for the next begin would strand admission forever.
            if (core->residentGeneralDisplay) {
                core->residentGeneralDisplay->executor().discardRetiredFailures();
                static_cast<void>(core->residentGeneralDisplay->takeDisplayImage());
            }
            if (core->residentDisplay && !core->residentDisplay->hasUnretiredSubmission()) {
                static_cast<void>(core->residentDisplay->discardRetiredFailure(*core->device));
                static_cast<void>(core->residentDisplay->takeImage());
            }
        }
        core->device->retireProcessingPhase(stage->gpuStage->phaseLease()->id(), proven);
        stage->processingPhaseArmed = false;
    }
    // Dispose the prepared scene before terminal publication or admitting a fallback. A sticky
    // generation slot independently keeps the conservative lease charged through native teardown.
    stage->gpuStage.reset();
}

// A failure with NO unretired native submission: safe to release admission immediately.
void finishResidentImmediately(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                               const std::shared_ptr<PreviewDisplayStageRecord>& stage,
                               const bool nativeFailure) {
    if (nativeFailure) {
        core->counterResidentFailures.fetch_add(1, std::memory_order_relaxed);
    }
    struct StorageRefusal final {
        render::GpuProcessingPhaseAdmission status;
        std::uint64_t phaseId;
        std::uint64_t revisionAfterOwnRelease;
        bool buffer;
    };
    std::optional<StorageRefusal> storageRefusal;
    if (core->device) {
        if (const auto refusal = core->device->nativeBufferStorageRefusal()) {
            storageRefusal = {refusal->status, refusal->phaseId, refusal->revisionAfterOwnRelease,
                              true};
        } else if (const auto imageRefusal = core->device->nativeImageStorageRefusal()) {
            storageRefusal = {imageRefusal->status, imageRefusal->phaseId,
                              imageRefusal->revisionAfterOwnRelease, false};
        }
    }
    if (storageRefusal && !residentDeviceLost(core) && !residentOwnerDrainRequired(core) &&
        !residentExecutorHasUnretiredSubmission(core) &&
        !residentDisplayHasUnretiredSubmission(core) &&
        !residentDisplayHasUnretiredSubmissionGeneral(core) &&
        (!core->residentDisplay || !core->residentDisplay->hasUnretiredSubmission()) &&
        (!core->packedTransfer || !core->packedTransfer->hasUnretiredSubmission()))
        stage->processingPhaseRetired = true;
    if (stage->processingPhaseRetired && core->device) {
        if (core->residentDisplay)
            static_cast<void>(core->residentDisplay->discardRetiredFailure(*core->device));
        if (core->residentGeneralDisplay)
            core->residentGeneralDisplay->executor().discardRetiredFailures();
    }
    // A proven transfer failure can still own an unpublished native source and provisional host
    // storage. Dispose these before phase release/terminal delivery, never while a queue is live.
    if (core->packedTransfer && !core->packedTransfer->hasUnretiredSubmission())
        static_cast<void>(core->packedTransfer->reset());
    const bool retryableStorage = storageRefusal && stage->gpuStage &&
                                  stage->gpuStage->phaseLease() && stage->processingPhaseRetired &&
                                  storageRefusal->phaseId == stage->gpuStage->phaseLease()->id();
    releaseProcessingPhase(core, stage,
                           !stage->processingPhaseTouched || stage->processingPhaseRetired);
    if (stage->cancellationRequested || stage->nativeDiscard ||
        core->stopping.load(std::memory_order_acquire)) {
        completeCancelled(stage);
        return;
    }
    if (retryableStorage) {
        auto diagnostic = residentJobDiagnostic(
            storageRefusal->status == render::GpuProcessingPhaseAdmission::AwaitingCapacity
                ? "preview.processing-phase.awaiting-capacity"
                : "preview.processing-phase.budget-refused",
            storageRefusal->buffer
                ? "The native staging buffer is awaiting processing capacity."
                : "The native image allocation is awaiting processing capacity.");
        diagnostic.admissionRetry =
            TaskAdmissionRetry{storageRefusal->revisionAfterOwnRelease, true};
        completeFailed(stage, {std::move(diagnostic)});
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
                             const std::shared_ptr<PreviewDisplayStageRecord>& stage,
                             const bool nativeFailure = true) {
    if (stage->residentPhase == ResidentNativePhase::Retiring) {
        return;
    }
    if (residentDeviceLost(core)) {
        publishResidentLossDetail(core, *stage);
        core->residentRouteTerminal = true;
    } else if (nativeFailure) {
        core->counterResidentFailures.fetch_add(1, std::memory_order_relaxed);
    }
    stage->processingPhaseFailed = stage->processingPhaseFailed || nativeFailure;
    residentCancelNative(core);
    stage->nativeDiscard = stage->nativeDiscard || stage->cancellationRequested ||
                           core->stopping.load(std::memory_order_acquire);
    stage->residentRetirePumps = 0;
    stage->residentPhase = ResidentNativePhase::Retiring;
    core->residentNativeActive = stage;
    core->residentNativeInFlight = true;
}

// Normal retirement consumes parent admission only after proof. Bounded shutdown may instead hand
// retained physical allocations to the transfer-specific independent quarantine reservation.
void finishResidentRetirement(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                              const std::shared_ptr<PreviewDisplayStageRecord>& stage) {
    const bool cancelled = stage->cancellationRequested || stage->nativeDiscard ||
                           core->stopping.load(std::memory_order_acquire);
    core->residentNativeActive.reset();
    core->residentNativeInFlight = false;
    stage->residentPhase = ResidentNativePhase::Done;
    releaseProcessingPhase(
        core, stage, !stage->processingPhaseFailed && core->device && !residentDeviceLost(core));
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
                auto planned = planPreviewCpuStorage(
                    core->options.cpuStoragePlanner, *stage->snapshot, stage->identity,
                    stage->pixelStorageByteLimit, stage->overrides, context);
                if (planned.state() == TaskState::Cancelled)
                    return Result::cancelled(planned.diagnostics());
                if (planned.state() == TaskState::Failed)
                    return Result::failed(planned.diagnostics());
                if (!planned.value())
                    return Result::failed(
                        residentJobDiagnostic("bloom.runtime.gpu-preview-missing-storage-plan",
                                              "The CPU storage planner returned no peak."));
                auto phase = reservePreviewCpuPhase(
                    core->options.processingPhasePolicy, *stage->snapshot, stage->identity,
                    stage->pixelStorageByteLimit, {}, *planned.value());
                if (context.isCancellationRequested()) {
                    return Result::cancelled();
                }
                if (phase.status != render::GpuProcessingPhaseAdmission::Accepted || !phase.lease) {
                    return Result::failed(previewCpuPhaseDiagnostic(phase));
                }
                auto stageResult =
                    core->stageFunction(*stage->snapshot, stage->identity,
                                        static_cast<std::size_t>(phase.lease->sourceByteLimit()),
                                        stage->overrides, context);
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
        stage->phase = StagePhase::AwaitingCpuFallbackDispatch;
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
    if (!eligible) {
        core->counterCpuFallbacks.fetch_add(1, std::memory_order_relaxed);
        const std::string detail =
            reason.empty() ? std::string("the resident stage was not eligible") : reason;
        {
            std::lock_guard lock(core->stateMutex);
            core->publishedResidentDetail = "resident stage ineligible: " + detail;
        }
        // The child result still aliases this prepared scene until the caller finishes consuming
        // it. Defer fallback admission until that result and every service-owned scene alias are
        // disposed; otherwise a count-one root would contend with its own abandoned preparation.
        stage->gpuStage.reset();
        stage->residentProcessor.reset();
        stage->phase = StagePhase::AwaitingCpuFallbackDispatch;
        return;
    }
    // Prepared work waiting for the native owner is back-pressure: its phase is already admitted,
    // so it queues here however many frames are in flight and is never re-rendered on the CPU.
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
        const auto& phase = stage->gpuStage->phaseLease();
        // Recover a phase latched by an earlier unproven retirement (device-wide idle proof).
        if (phase && core->device && core->device->processingPhaseOccupied())
            static_cast<void>(core->device->recoverUnprovenProcessingPhase());
        if ((core->options.processingPhasePolicy && !phase) ||
            (phase && (!core->device || !core->device->retainProcessingPhase(phase)))) {
            auto diagnostic = residentJobDiagnostic(
                "preview.processing-phase.budget-refused",
                "The native generation cannot admit another processing phase");
            if (phase)
                diagnostic.admissionRetry =
                    TaskAdmissionRetry{phase->revisionAfterOwnRelease(), true};
            stage->gpuStage.reset();
            completeFailed(stage, {std::move(diagnostic)});
            return;
        }
        stage->processingPhaseArmed = static_cast<bool>(phase);
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
        // begin() only plans/pins. This is the first native-capable step, including OCIO upload.
        stage->processingPhaseTouched = true;
        const auto poll = residentExecutorPoll(core);
        if (poll == GpuSceneExecutorPollResult::Pending) {
            if (cancelled ||
                nativeNow(core) - stage->nativeStartedAt >= core->options.nativeDispatchDeadline) {
                beginResidentRetirement(core, stage, !cancelled);
            }
            return;
        }
        noteResidentExecutorCounters(core);
        if (residentDeviceLost(core)) {
            beginResidentRetirement(core, stage);
            return;
        }
        if (poll != GpuSceneExecutorPollResult::Ready) {
            const bool expectedCancellation = cancelled &&
                                              poll == GpuSceneExecutorPollResult::Failure &&
                                              core->residentExecutor != nullptr &&
                                              core->residentExecutor->diagnostic().code ==
                                                  GpuSceneExecutorDiagnosticCode::Cancelled;
            if (core->residentExecutor != nullptr && !expectedCancellation) {
                publishResidentJobDetail(
                    core, executorFailureDetail(*core->residentExecutor,
                                                residentStageAdmittedBytes(core, *stage),
                                                "resident scene executor failed", poll));
            }
            // A later operation may refuse its image/staging peak after the earlier native
            // prefix has retired. Only this exact positive proof releases a touched failed phase;
            // unknown work and the device's independent unproven latch remain sticky.
            if (core->residentExecutor &&
                gpuSceneExecutorBudgetRefusalRetired(*core->residentExecutor, poll) &&
                !residentDeviceLost(core) && !residentExecutorHasUnretiredSubmission(core) &&
                !residentDisplayHasUnretiredSubmission(core) &&
                !residentDisplayHasUnretiredSubmissionGeneral(core) &&
                (!core->residentDisplay || !core->residentDisplay->hasUnretiredSubmission()) &&
                (!core->packedTransfer || !core->packedTransfer->hasUnretiredSubmission())) {
                stage->processingPhaseRetired = true;
                core->residentNativeInFlight = false;
                core->residentNativeActive.reset();
                stage->residentPhase = ResidentNativePhase::Done;
                finishResidentImmediately(core, stage, true);
                return;
            }
            beginResidentRetirement(core, stage, !expectedCancellation);
            return;
        }
        if (stage->cancellationRequested || stage->nativeDiscard) {
            beginResidentRetirement(core, stage, false);
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
                beginResidentRetirement(core, stage, !cancelled);
            }
            return;
        }
        const bool expectedCancellation = cancelled &&
                                          poll == render::GpuResidentDisplayPollResult::Failure &&
                                          residentDisplayCancelled(*core, *stage);
        const bool nativeFailure =
            poll != render::GpuResidentDisplayPollResult::Ready && !expectedCancellation;
        if (nativeFailure)
            publishResidentJobDetail(core, residentDisplayFailureDetail(*core, *stage, poll));
        if (residentDeviceLost(core) || residentDisplayHasUnretiredSubmission(core)) {
            beginResidentRetirement(core, stage, nativeFailure);
            return;
        }
        if (!cancelled && poll == render::GpuResidentDisplayPollResult::Ready &&
            core->options.product == GpuPreviewDisplayProductMode::GpuPreparedPacked) {
            std::string reason;
            if (residentPackedTransferBegin(core, stage, reason)) {
                stage->residentPhase = ResidentNativePhase::Transfer;
                stage->nativeStartedAt = nativeNow(core);
                return;
            }
            publishResidentJobDetail(core, "GPU packed transfer begin refused: " + reason);
        } else {
            // The display submission is retired (or never submitted); no unretired pin remains.
            stage->processingPhaseRetired = poll == render::GpuResidentDisplayPollResult::Ready;
            core->residentNativeInFlight = false;
            core->residentNativeActive.reset();
            stage->residentPhase = ResidentNativePhase::Done;
            if (cancelled) {
                releaseProcessingPhase(core, stage, !nativeFailure);
                if (nativeFailure) {
                    core->counterResidentFailures.fetch_add(1, std::memory_order_relaxed);
                }
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
                        releaseProcessingPhase(core, stage, true);
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
        }
        core->residentNativeInFlight = false;
        core->residentNativeActive.reset();
        stage->residentPhase = ResidentNativePhase::Done;
        finishResidentImmediately(core, stage, true);
        return;
    }
    case ResidentNativePhase::Transfer: {
        const auto poll = core->packedTransfer
                              ? (core->packedTransferPollOverride
                                     ? core->packedTransferPollOverride(*core->packedTransfer)
                                     : core->packedTransfer->poll())
                              : render::GpuDisplayTransferState::Failure;
        if (poll == render::GpuDisplayTransferState::Pending) {
            if (cancelled ||
                nativeNow(core) - stage->nativeStartedAt >= core->options.nativeDispatchDeadline)
                beginResidentRetirement(core, stage, !cancelled);
            return;
        }
        if (core->packedTransfer && core->packedTransfer->hasUnretiredSubmission()) {
            beginResidentRetirement(core, stage, !cancelled);
            return;
        }
        core->residentNativeInFlight = false;
        core->residentNativeActive.reset();
        stage->residentPhase = ResidentNativePhase::Done;
        if (cancelled) {
            const bool proven =
                poll == render::GpuDisplayTransferState::Ready ||
                (core->packedTransfer && core->packedTransfer->diagnostic().code ==
                                             render::GpuDisplayTransferCode::Cancelled);
            if (core->packedTransfer) {
                core->packedTransfer->cancel();
                if (core->packedQueueTiming)
                    core->packedQueueTiming->cancel();
                static_cast<void>(core->packedTransfer->reset());
            }
            releaseProcessingPhase(core, stage, proven);
            completeCancelled(stage);
            return;
        }
        if (poll == render::GpuDisplayTransferState::Ready) {
            stage->processingPhaseRetired = true;
            try {
                std::string reason;
                auto frame = residentPackedTransferFinish(core, *stage, reason);
                if (frame) {
                    auto prepared = PreviewPreparationResult::prepared(
                        std::make_shared<const PreparedPreviewFrame>(std::move(*frame)));
                    if (prepared && stage->completion.isValid()) {
                        // All pixel/proof/identity/frame/result allocations succeed before
                        // promoting the provisional storage. No capacity-fallback branch follows
                        // this terminal handoff; cancellation or forced terminal delivery merely
                        // discards aliases.
                        if (core->testBeforePackedPayloadAllocation)
                            core->testBeforePackedPayloadAllocation();
                        auto payload =
                            std::make_shared<const PreviewPreparationResult>(std::move(*prepared));
                        if (stage->completion.cancellationRequested() || core->stopping.load()) {
                            payload.reset();
                            prepared.reset();
                            frame.reset();
                            releaseProcessingPhase(core, stage, true);
                            completeCancelled(stage);
                            return;
                        }
                        const auto& display = payload->frame()->displayOnlyFrame();
                        if (display &&
                            display->packedHostOwnership() !=
                                render::PackedRgba8Ownership::LegacyUnowned &&
                            !display->promotePackedHostOwnership()) {
                            publishResidentJobDetail(core,
                                                     "GPU packed host ownership promotion refused");
                        } else {
                            releaseProcessingPhase(core, stage, true);
                            static_cast<void>(
                                std::move(stage->completion).succeed(std::move(payload)));
                            stage->phase = StagePhase::Done;
                            return;
                        }
                    }
                }
                publishResidentJobDetail(core, "GPU packed product refused: " + reason);
            } catch (...) {
                // The copy fence is proved retired. All provisional frame/result/storage aliases
                // above unwind before releasing the prepared phase; nothing remains Running
                // after this stage has left the native active/ready queues. Allocation and other
                // product-construction failures are terminal, never capacity-retry signals.
                releaseProcessingPhase(core, stage, true);
                std::vector<TaskDiagnostic> diagnostics;
                try {
                    diagnostics.push_back(residentJobDiagnostic(
                        "bloom.runtime.gpu-packed-product-construction-failed",
                        "The completed GPU packed product could not be constructed."));
                } catch (...) {
                    // Diagnostics are best effort. Empty-vector failure uses the already owned
                    // completion and performs no diagnostic allocation, unlike completeFailed.
                    diagnostics.clear();
                }
                if (stage->completion.isValid()) {
                    static_cast<void>(
                        std::move(stage->completion)
                            .complete(TaskResult<PreviewPreparationResultHandle>::failed(
                                std::move(diagnostics))));
                }
                stage->phase = StagePhase::Done;
                return;
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
        if (core->packedTransfer && core->packedTransfer->hasUnretiredSubmission())
            static_cast<void>(core->packedTransferPollOverride
                                  ? core->packedTransferPollOverride(*core->packedTransfer)
                                  : core->packedTransfer->poll());
        if (residentDisplayHasUnretiredSubmission(core)) {
            static_cast<void>(residentDisplayPoll(core));
        }
        if (residentDeviceLost(core)) {
            publishResidentLossDetail(core, *stage);
            stage->processingPhaseFailed = true;
            releaseProcessingPhase(core, stage, false);
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
            if (core->packedTransfer)
                static_cast<void>(core->packedTransfer->reset());
            core->packedTransferLate = false;
            finishResidentRetirement(core, stage);
            return;
        }
        ++stage->residentRetirePumps;
        if (stage->residentRetirePumps >= kResidentRetirementPumpBudget) {
            // Bounded owner-thread teardown: the pipeline destructors drain/quarantine. This is the
            // only point at which an unproven fence is abandoned, and it is explicit and counted.
            const bool transferQuarantined =
                core->packedTransfer && core->packedTransfer->hasUnretiredSubmission();
            if (transferQuarantined && !core->stopping.load(std::memory_order_acquire)) {
                core->packedTransferLate = true;
                stage->residentRetirePumps = kResidentRetirementPumpBudget;
                return; // bounded late backoff still offers the exact owner real fence observations
            }
            stage->processingPhaseFailed = true;
            releaseProcessingPhase(core, stage, false);
            if (transferQuarantined && core->testBeforePackedUnprovenTeardown)
                core->testBeforePackedUnprovenTeardown();
            teardownTerminalResidentRoute(core);
            if (transferQuarantined) {
                publishResidentJobDetail(core,
                                         "packed transfer quarantined on shutdown; independent "
                                         "source/staging/host reservation survives task detach");
                // Explicit bounded shutdown handoff: the independent reservation still charges
                // physically quarantined allocations after task admission/context are released.
                core->residentNativeActive.reset();
                core->residentNativeInFlight = false;
                stage->residentPhase = ResidentNativePhase::Done;
                completeCancelled(stage);
            } else {
                finishResidentRetirement(core, stage);
            }
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
            // Test-only staging needs finite headroom for native allocation alignment. The copy
            // still contains exactly the requested sparse pixels, and the renderer checks actual
            // VMA bytes against this allowance before submitting it.
            const auto sampleBudget = std::max<std::uint64_t>(
                64ULL * 1024ULL, sampleCoordinates->size() * sizeof(render::Rgba8));
            const auto readback = render::readbackResidentDisplayImageSparse(
                pinned.pin.image(), *sampleCoordinates, sampleBudget);
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
