#include "gpu_preview_display_service_private.hpp"

#include <bloom/runtime/gpu_preview_display_product.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace bloom::runtime {
namespace detail {
namespace {

using StagePhase = PreviewDisplayStageRecord::Phase;

[[nodiscard]] TaskDiagnostic previewDiagnostic(std::string code, std::string summary,
                                               std::string detail = {}) {
    return {.code = std::move(code),
            .severity = DiagnosticSeverity::Error,
            .summary = std::move(summary),
            .detail = std::move(detail),
            .suggestedAction = "Review the GPU preview display service diagnostics."};
}

[[nodiscard]] TaskDiagnostic startupDiagnostic() {
    return previewDiagnostic("bloom.runtime.gpu-preview-startup-failed",
                             "The GPU preview display service could not initialize.");
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
        diagnostics.push_back(previewDiagnostic("bloom.runtime.gpu-preview-display-failed",
                                                "The GPU preview display request failed."));
    }
    if (stage->completion.isValid()) {
        static_cast<void>(std::move(stage->completion)
                              .complete(TaskResult<PreviewPreparationResultHandle>::failed(
                                  std::move(diagnostics))));
    }
    stage->phase = StagePhase::Done;
}

// The retained frame plus native input/readback must fit the native pipeline ceilings and the
// request's own preview byte allowance before any dispatch happens.
[[nodiscard]] bool nativeGeometryFits(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                      const std::shared_ptr<PreviewDisplayStageRecord>& stage) {
    if (stage->stage == nullptr || stage->stage->processFrame() == nullptr) {
        return false;
    }
    const std::uint64_t pixels = stage->stage->processFrame()->processImage().pixels().size();
    if (pixels == 0) {
        return false;
    }
    const std::uint64_t inputBytes = pixels * sizeof(render::Rgba32f);
    const std::uint64_t readbackBytes = pixels * sizeof(render::Rgba8);
    const std::uint64_t allowance = stage->pixelStorageByteLimit != 0
                                        ? stage->pixelStorageByteLimit
                                        : core->options.previewByteAllowance;
    return inputBytes <= core->options.nativeBudgets.maxInputBytes &&
           inputBytes + readbackBytes <= core->options.nativeBudgets.maxOwnedBytes &&
           (allowance == 0 || inputBytes + readbackBytes <= allowance);
}

[[nodiscard]] std::shared_ptr<PreviewDisplayStageRecord>
pickReadyStage(const std::shared_ptr<PreviewDisplayServiceCore>& core) {
    auto best = core->nativeReady.end();
    for (auto it = core->nativeReady.begin(); it != core->nativeReady.end(); ++it) {
        if ((*it)->cancellationRequested) {
            best = it;
            break;
        }
        if (best == core->nativeReady.end() ||
            static_cast<int>((*it)->priority) < static_cast<int>((*best)->priority)) {
            best = it;
        }
    }
    if (best == core->nativeReady.end()) {
        return nullptr;
    }
    auto stage = *best;
    core->nativeReady.erase(best);
    return stage;
}

void handleStageChildResult(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                            const std::shared_ptr<PreviewDisplayStageRecord>& stage,
                            const TaskResult<PreviewCpuStageOutcomeHandle>& result) {
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
        completeFailed(stage, {previewDiagnostic("bloom.runtime.gpu-preview-missing-stage",
                                                 "The CPU stage returned no outcome.")});
        return;
    }
    const auto& outcome = **value;
    if (outcome.status == PreviewCpuStageStatus::Unsupported) {
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
    if (outcome.stage == nullptr) {
        completeFailed(stage, {previewDiagnostic("bloom.runtime.gpu-preview-missing-frame",
                                                 "The CPU stage produced no evaluated frame.")});
        return;
    }
    stage->stage = outcome.stage;

    if (stage->cancellationRequested) {
        completeCancelled(stage);
        return;
    }

    std::string reason;
    const bool eligible =
        core->gpuAvailable && core->qualification != nullptr &&
        gpuNeutralDisplayStageIsEligible(*stage->stage, *core->qualification, reason) &&
        nativeGeometryFits(core, stage);
    const std::uint64_t pixelCount =
        eligible ? stage->stage->processFrame()->processImage().pixels().size() : 0;
    const bool worthwhile =
        eligible && core->qualification != nullptr &&
        gpuPreviewDisplaySelectsNative(*core->qualification, pixelCount,
                                       gpuPreviewDisplayHandoffOverheadMicros());
    if (!eligible || !worthwhile ||
        core->nativeReady.size() >= core->options.readyStageQueueCapacity) {
        dispatchDisplayFallbackChild(core, stage);
        return;
    }
    stage->phase = StagePhase::AwaitingNative;
    core->nativeReady.push_back(stage);
}

void retirePipeline(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                    GpuPreviewDisplayServiceDiagnosticCode code, std::string message,
                    std::shared_ptr<const GpuNeutralDisplayQualificationReport> report) {
    core->display.reset();
    if (render::GpuNeutralDisplay::teardownDrainIncomplete()) {
        message += " Teardown drain incomplete; the native pipeline was quarantined.";
    }
    core->publishState(GpuPreviewDisplayServiceState::Unavailable, false,
                       {code, std::move(message)}, std::move(report));
}

[[nodiscard]] std::chrono::steady_clock::time_point
nativeNow(const std::shared_ptr<PreviewDisplayServiceCore>& core) {
    return core->nativeClockOverride ? core->nativeClockOverride()
                                     : std::chrono::steady_clock::now();
}

[[nodiscard]] render::GpuNeutralDisplayPollResult
pollNative(const std::shared_ptr<PreviewDisplayServiceCore>& core) {
    return core->nativePollOverride ? core->nativePollOverride(*core->display)
                                    : core->display->poll();
}

// Bounded native-dispatch deadline expiry: retire/quarantine the native pipeline on the owner
// thread first, disable the GPU, then take the same-stage CPU fallback (or cancel if requested).
// Never completes early while native ownership is unretired.
void timeoutNativeDisplay(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                          const std::shared_ptr<PreviewDisplayStageRecord>& stage) {
    core->nativeInFlight = false;
    core->nativeActive.reset();
    retirePipeline(core, GpuPreviewDisplayServiceDiagnosticCode::NativeTimeout,
                   "The native GPU display dispatch exceeded its bounded deadline.",
                   core->qualification);
    if (stage->cancellationRequested || core->stopping.load(std::memory_order_acquire)) {
        completeCancelled(stage);
    } else {
        dispatchDisplayFallbackChild(core, stage);
    }
    while (!core->nativeReady.empty()) {
        auto queued = core->nativeReady.front();
        core->nativeReady.pop_front();
        dispatchDisplayFallbackChild(core, queued);
    }
}

} // namespace

// -----------------------------------------------------------------------------------------------
// GPU startup task (service thread)
// -----------------------------------------------------------------------------------------------

namespace {
void runGpuStartupImpl(const std::shared_ptr<PreviewDisplayServiceCore>& core, TaskContext& context,
                       GpuTaskCompletion<int>& completion) {
    if (!core->options.enabled) {
        core->publishState(GpuPreviewDisplayServiceState::Unavailable, false,
                           {GpuPreviewDisplayServiceDiagnosticCode::Disabled,
                            "The GPU preview display service is disabled."},
                           nullptr);
        static_cast<void>(std::move(completion).fail(startupDiagnostic()));
        return;
    }
    if (context.isCancellationRequested() ||
        core->shutdownRequested.load(std::memory_order_acquire)) {
        static_cast<void>(std::move(completion).cancel());
        return;
    }

    render::GpuDeviceCreationOptions deviceOptions;
    deviceOptions.loader_path = core->options.loaderPath;
    if (core->options.presentation == GpuPreviewDisplayServicePresentationMode::Wayland) {
        deviceOptions.request_presentation = true;
        deviceOptions.presentation_platform = render::GpuPresentationPlatform::Wayland;
    }
    auto created = render::GpuDevice::create(deviceOptions);
    if (!created) {
        // Report the requested presentation mode as Unavailable (never NotRequested) so the UI can
        // see there is no blank activation, then fail the compute path as before.
        publishServicePresentation(core);
        core->publishState(GpuPreviewDisplayServiceState::Unavailable, false,
                           {GpuPreviewDisplayServiceDiagnosticCode::LoaderUnavailable,
                            created.diagnostic.message.empty()
                                ? std::string("The native GPU loader/device is unavailable.")
                                : created.diagnostic.message},
                           nullptr);
        static_cast<void>(std::move(completion).fail(startupDiagnostic()));
        return;
    }
    core->device = std::move(created.device);

    // Own the presentation generation on this same device and owner thread. When presentation mode
    // is Disabled or the capability is not Ready this returns a non-available record and the
    // compute/CPU path is completely unchanged. The immutable UI client and the actual shutdown
    // snapshot are published immediately; they are refreshed on every owner pump.
    core->presentation = createServicePresentation(core);
    publishServicePresentation(core);

    auto built = buildBloomNeutralQualifiedDisplayProcessor();
    if (!built.succeeded()) {
        core->publishState(GpuPreviewDisplayServiceState::Unavailable, false,
                           {GpuPreviewDisplayServiceDiagnosticCode::ProcessorUnavailable,
                            built.diagnostic().summary},
                           nullptr);
        static_cast<void>(std::move(completion).fail(startupDiagnostic()));
        return;
    }
    core->processor = built.handle();

    // Resident route: qualify the resident display independently of the packed readback
    // qualification. The resident report is the only authority for resident selection.
    if (core->gpuStageFunction != nullptr) {
        const bool neutralQualified = createAndQualifyResidentRoute(core);
        // A failed Neutral qualification is not fatal for the general display route: as long as the
        // owner created the scene executor (and retained a live resident display), the service is
        // Ready and each request decides per-stage. The service is only Unavailable when neither
        // route exists.
        const bool generalPrerequisites = core->residentExecutor != nullptr &&
                                          core->residentDisplay != nullptr &&
                                          !core->residentRouteTerminal;
        if (!neutralQualified && !generalPrerequisites) {
            std::string detail;
            {
                std::lock_guard lock(core->stateMutex);
                detail = core->publishedResidentDetail;
            }
            core->publishState(
                GpuPreviewDisplayServiceState::Unavailable, false,
                {GpuPreviewDisplayServiceDiagnosticCode::ResidentQualificationUnavailable,
                 detail.empty() ? std::string("The resident preview route did not qualify.")
                                : std::move(detail)},
                nullptr);
            static_cast<void>(std::move(completion).fail(startupDiagnostic()));
            return;
        }
        core->publishState(GpuPreviewDisplayServiceState::Ready, true,
                           {GpuPreviewDisplayServiceDiagnosticCode::None, {}}, nullptr);
        if (context.isCancellationRequested() ||
            core->shutdownRequested.load(std::memory_order_acquire)) {
            static_cast<void>(std::move(completion).cancel());
        } else {
            static_cast<void>(std::move(completion).succeed(0));
        }
        return;
    }

    auto native = render::GpuNeutralDisplay::create(*core->device, core->options.nativeBudgets);
    if (!native) {
        core->publishState(
            GpuPreviewDisplayServiceState::Unavailable, false,
            {GpuPreviewDisplayServiceDiagnosticCode::NativeFailure, native.diagnostic.message},
            nullptr);
        static_cast<void>(std::move(completion).fail(startupDiagnostic()));
        return;
    }
    core->display = std::move(native.display);

    auto predicate = [core]() noexcept {
        return core->shutdownRequested.load(std::memory_order_acquire);
    };
    auto report = qualifyGpuNeutralDisplay(*built.handle(), *core->device, *core->display,
                                           color::CancellationPredicateRef(predicate));
    auto reportPointer =
        std::make_shared<const GpuNeutralDisplayQualificationReport>(std::move(report));

    if (!reportPointer->eligible()) {
        // The unqualified pipeline is retired on its owner thread before any state is published.
        retirePipeline(core, GpuPreviewDisplayServiceDiagnosticCode::QualificationUnavailable,
                       reportPointer->diagnostic().message.empty()
                           ? std::string("The pinned Bloom Neutral v1 display did not qualify.")
                           : reportPointer->diagnostic().message,
                       reportPointer);
        static_cast<void>(std::move(completion).fail(startupDiagnostic()));
        return;
    }

    core->publishState(GpuPreviewDisplayServiceState::Ready, true,
                       {GpuPreviewDisplayServiceDiagnosticCode::None, {}}, reportPointer);
    if (context.isCancellationRequested() ||
        core->shutdownRequested.load(std::memory_order_acquire)) {
        static_cast<void>(std::move(completion).cancel());
    } else {
        static_cast<void>(std::move(completion).succeed(0));
    }
}
} // namespace

void runGpuStartup(const std::shared_ptr<PreviewDisplayServiceCore>& core, TaskContext& context,
                   GpuTaskCompletion<int> completion) {
    try {
        runGpuStartupImpl(core, context, completion);
    } catch (...) {
        retirePipeline(core, GpuPreviewDisplayServiceDiagnosticCode::NativeFailure,
                       "The GPU preview display startup raised an exception.", core->qualification);
        if (completion.isValid()) {
            static_cast<void>(std::move(completion).fail(startupDiagnostic()));
        }
    }
}

// -----------------------------------------------------------------------------------------------
// GPU parent starter (service thread)
// -----------------------------------------------------------------------------------------------

void startGpuPreviewStage(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                          const bool cancellationRequested,
                          GpuTaskCompletion<PreviewPreparationResultHandle> completion,
                          PreviewStageSubmission submission) {
    auto stage = std::make_shared<PreviewDisplayStageRecord>();
    stage->completion = std::move(completion);
    stage->priority = submission.priority;
    stage->owner = submission.owner;
    stage->groupId = submission.groupId;
    stage->sourceVersion = submission.sourceVersion;
    stage->requestOwnedBytes = submission.pixelStorageByteLimit;
    stage->pixelStorageByteLimit = submission.pixelStorageByteLimit;
    stage->report = core->qualificationReport();
    if (submission.coalescingKey.has_value() && !submission.coalescingKey->empty()) {
        stage->childCoalescingKey = *submission.coalescingKey + ".cpu";
    }

    if (cancellationRequested) {
        completeCancelled(stage);
        return;
    }

    TaskRequest childRequest("GPU preview CPU stage", submission.owner, submission.priority,
                             TaskExecutor::Cpu);
    childRequest.groupId = submission.groupId;
    childRequest.sourceVersion = submission.sourceVersion;
    childRequest.coalescingKey = stage->childCoalescingKey;

    try {
        auto child = core->scheduler->submit<PreviewCpuStageOutcomeHandle>(
            std::move(childRequest),
            [core, snapshot = std::move(submission.snapshot),
             identity = std::move(submission.identity), limit = submission.pixelStorageByteLimit,
             overrides = std::move(submission.overrides)](
                TaskContext& context) -> TaskResult<PreviewCpuStageOutcomeHandle> {
                using Result = TaskResult<PreviewCpuStageOutcomeHandle>;
                if (context.isCancellationRequested()) {
                    return Result::cancelled();
                }
                auto result = core->stageFunction(snapshot, identity, limit, overrides, context);
                core->notify();
                return result;
            });
        if (!child.accepted()) {
            completeFailed(stage, child.diagnostic.has_value()
                                      ? std::vector<TaskDiagnostic>{*child.diagnostic}
                                      : std::vector<TaskDiagnostic>{previewDiagnostic(
                                            "bloom.runtime.gpu-preview-child-rejected",
                                            "The CPU stage child was not admitted.")});
            return;
        }
        stage->stageChild = child.handle;
        core->stages.push_back(std::move(stage));
    } catch (...) {
        // Fail closed on the service thread: the parent token is consumed, never abandoned.
        completeFailed(stage, {previewDiagnostic("bloom.runtime.gpu-preview-child-threw",
                                                 "The CPU stage child submission raised.")});
    }
}

// -----------------------------------------------------------------------------------------------
// CPU display fallback child (service thread submission, CPU worker execution)
// -----------------------------------------------------------------------------------------------

void dispatchDisplayFallbackChild(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                  const std::shared_ptr<PreviewDisplayStageRecord>& stage) {
    if (core->stopping.load(std::memory_order_acquire) || stage->cancellationRequested) {
        completeCancelled(stage);
        return;
    }
    if (stage->stage == nullptr) {
        completeFailed(stage,
                       {previewDiagnostic("bloom.runtime.gpu-preview-missing-frame",
                                          "No evaluated frame exists for the CPU fallback.")});
        return;
    }

    TaskRequest request("GPU preview CPU display fallback", stage->owner, stage->priority,
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
                auto result = core->fallback(*stage->stage, context);
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
        completeFailed(stage, {previewDiagnostic("bloom.runtime.gpu-preview-fallback-threw",
                                                 "The CPU display fallback submission raised.")});
    }
}

// -----------------------------------------------------------------------------------------------
// Service-loop stage processing
// -----------------------------------------------------------------------------------------------

void processPreviewStages(const std::shared_ptr<PreviewDisplayServiceCore>& core) {
    const bool serviceCancelled = core->stopping.load(std::memory_order_acquire) ||
                                  core->shutdownRequested.load(std::memory_order_acquire);
    for (std::size_t index = 0; index < core->stages.size();) {
        auto stage = core->stages[index];
        const bool cancelled = serviceCancelled || stage->completion.cancellationRequested();
        if (cancelled && !stage->cancellationRequested) {
            stage->cancellationRequested = true;
            if (stage->stageChild.isValid()) {
                stage->stageChild.cancel();
            }
            if (stage->gpuStageChild.isValid()) {
                stage->gpuStageChild.cancel();
            }
            if (stage->fallbackChild.isValid()) {
                stage->fallbackChild.cancel();
            }
            if (stage->nativeDispatched) {
                stage->nativeDiscard = true;
                if (stage->resident) {
                    residentCancelNative(core);
                } else if (core->display != nullptr) {
                    core->display->cancel();
                }
            }
        }

        try {
            if (stage->phase == StagePhase::AwaitingStage && stage->gpuStageChild.isValid()) {
                if (auto result = stage->gpuStageChild.tryTakeResult()) {
                    handleGpuStageChildResult(core, stage, *result);
                }
            }

            if (stage->phase == StagePhase::AwaitingStage && stage->stageChild.isValid()) {
                if (auto result = stage->stageChild.tryTakeResult()) {
                    handleStageChildResult(core, stage, *result);
                }
            }

            if (stage->phase == StagePhase::AwaitingFallback && stage->fallbackChild.isValid()) {
                if (auto result = stage->fallbackChild.tryTakeResult()) {
                    if (stage->cancellationRequested && result->state() != TaskState::Failed) {
                        completeCancelled(stage, result->diagnostics());
                    } else if (stage->completion.isValid()) {
                        static_cast<void>(
                            std::move(stage->completion).complete(std::move(*result)));
                        stage->phase = StagePhase::Done;
                    }
                }
            }
        } catch (...) {
            completeFailed(stage, {previewDiagnostic("bloom.runtime.gpu-preview-stage-threw",
                                                     "Stage processing raised.")});
        }

        if (stage->phase == StagePhase::Done) {
            core->stages.erase(core->stages.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        ++index;
    }
}

// -----------------------------------------------------------------------------------------------
// Native display pump (service thread, one job in flight)
// -----------------------------------------------------------------------------------------------

void processNativeDisplay(const std::shared_ptr<PreviewDisplayServiceCore>& core) {
    if (!core->nativeReady.empty() && (core->display == nullptr || !core->gpuAvailable)) {
        while (!core->nativeReady.empty()) {
            auto stage = core->nativeReady.front();
            core->nativeReady.pop_front();
            dispatchDisplayFallbackChild(core, stage);
        }
    }

    if (core->display == nullptr) {
        return;
    }

    if (!core->nativeInFlight) {
        auto stage = pickReadyStage(core);
        if (stage == nullptr) {
            return;
        }
        if (stage->cancellationRequested || core->stopping.load(std::memory_order_acquire)) {
            completeCancelled(stage);
            return;
        }
        const auto& pixels = stage->stage->processFrame()->processImage().pixels();
        const std::uint64_t allowance = stage->pixelStorageByteLimit != 0
                                            ? stage->pixelStorageByteLimit
                                            : core->options.previewByteAllowance;
        const auto diagnostic = core->display->begin(pixels, allowance);
        if (diagnostic.code == render::GpuNeutralDisplayDiagnosticCode::None) {
            stage->nativeDispatched = true;
            stage->nativeStartedAt = nativeNow(core);
            stage->phase = StagePhase::AwaitingNative;
            core->nativeActive = stage;
            core->nativeInFlight = true;
            return;
        }
        const bool deviceFailure =
            diagnostic.code == render::GpuNeutralDisplayDiagnosticCode::DeviceLost ||
            diagnostic.code == render::GpuNeutralDisplayDiagnosticCode::DeviceUnavailable;
        if (deviceFailure) {
            disableGpuAfterNativeFailure(core, diagnostic.message);
        }
        dispatchDisplayFallbackChild(core, stage);
        return;
    }

    auto stage = core->nativeActive;
    if (stage == nullptr) {
        core->nativeInFlight = false;
        return;
    }
    const auto poll = pollNative(core);
    if (poll == render::GpuNeutralDisplayPollResult::Pending) {
        if (nativeNow(core) - stage->nativeStartedAt >= core->options.nativeDispatchDeadline) {
            timeoutNativeDisplay(core, stage);
        }
        return;
    }

    core->nativeInFlight = false;
    core->nativeActive.reset();

    if (poll == render::GpuNeutralDisplayPollResult::Ready) {
        auto readback = core->display->readback();
        if (!readback.hasValue()) {
            const bool deviceFailure =
                readback.diagnostic.code == render::GpuNeutralDisplayDiagnosticCode::DeviceLost ||
                readback.diagnostic.code ==
                    render::GpuNeutralDisplayDiagnosticCode::DeviceUnavailable;
            if (deviceFailure) {
                disableGpuAfterNativeFailure(core, readback.diagnostic.message);
            }
            dispatchDisplayFallbackChild(core, stage);
            return;
        }
        if (stage->cancellationRequested || stage->nativeDiscard) {
            completeCancelled(stage);
            return;
        }
        if (stage->stage == nullptr || stage->report == nullptr) {
            dispatchDisplayFallbackChild(core, stage);
            return;
        }
        auto frame =
            makeGpuNeutralDisplayPreview(*stage->stage, stage->report, std::move(readback));
        if (!frame.has_value()) {
            dispatchDisplayFallbackChild(core, stage);
            return;
        }
        auto prepared = PreviewPreparationResult::prepared(
            std::make_shared<const PreparedPreviewFrame>(std::move(*frame)));
        if (!prepared.has_value()) {
            dispatchDisplayFallbackChild(core, stage);
            return;
        }
        auto handle = std::make_shared<const PreviewPreparationResult>(std::move(*prepared));
        if (stage->completion.isValid()) {
            static_cast<void>(std::move(stage->completion).succeed(std::move(handle)));
        }
        stage->phase = StagePhase::Done;
        return;
    }

    const auto diagnostic = core->display->diagnostic();
    const bool deviceFailure =
        diagnostic.code == render::GpuNeutralDisplayDiagnosticCode::DeviceLost ||
        diagnostic.code == render::GpuNeutralDisplayDiagnosticCode::DeviceUnavailable;
    if (deviceFailure) {
        disableGpuAfterNativeFailure(core, diagnostic.message);
    }
    dispatchDisplayFallbackChild(core, stage);
}

void disableGpuAfterNativeFailure(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                  const std::string& detail) {
    std::string message = "The GPU preview display pipeline was disabled after a native failure.";
    if (!detail.empty()) {
        message += " ";
        message += detail;
    }
    retirePipeline(core, GpuPreviewDisplayServiceDiagnosticCode::NativeFailure, std::move(message),
                   core->qualification);
    while (!core->nativeReady.empty()) {
        auto stage = core->nativeReady.front();
        core->nativeReady.pop_front();
        dispatchDisplayFallbackChild(core, stage);
    }
}

} // namespace detail
} // namespace bloom::runtime
