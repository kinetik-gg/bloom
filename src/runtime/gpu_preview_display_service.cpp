#include "gpu_preview_display_service_private.hpp"
#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/runtime/gpu_preview_display_product.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::runtime {
namespace {

[[nodiscard]] TaskDiagnostic previewDiagnostic(std::string code, std::string summary,
                                               std::string detail = {}) {
    return {.code = std::move(code),
            .severity = DiagnosticSeverity::Error,
            .summary = std::move(summary),
            .detail = std::move(detail),
            .suggestedAction = "Review the GPU preview display service diagnostics."};
}

[[nodiscard]] GpuServiceGeneration allocateGpuServiceGeneration() noexcept {
    static std::atomic<std::uint64_t> next{1};
    for (;;) {
        const std::uint64_t raw = next.fetch_add(1, std::memory_order_relaxed);
        if (raw == 0) {
            continue;
        }
        if (auto generation = GpuServiceGeneration::fromRaw(raw)) {
            return *generation;
        }
    }
}

[[nodiscard]] TaskOwner startupOwner(const GpuServiceGeneration generation) {
    return {.kind = TaskOwnerKind::Application, .id = TaskOwnerId::fromRaw(generation.value())};
}

// The CPU half of a submission: compile -> evaluate -> select, then apply the display product to
// the same evaluated frame. This is the exact composition
// makeCompositionPreviewPipelineFromCpuStage performs; the GPU path never replaces it, only draws
// it on a CPU worker when the GPU half cannot run.
[[nodiscard]] TaskResult<PreviewPreparationResultHandle>
runCpuPipeline(const std::shared_ptr<detail::PreviewDisplayServiceCore>& core,
               const document::Snapshot& snapshot, const PreviewRequestIdentity& identity,
               const std::size_t pixelStorageByteLimit,
               const std::vector<SnapshotParameterOverride>& overrides, TaskContext& context) {
    using Result = TaskResult<PreviewPreparationResultHandle>;
    if (context.isCancellationRequested()) {
        return Result::cancelled();
    }
    auto stageResult =
        core->stageFunction(snapshot, identity, pixelStorageByteLimit, overrides, context);
    if (stageResult.state() == TaskState::Cancelled) {
        return Result::cancelled(stageResult.diagnostics());
    }
    if (stageResult.state() == TaskState::Failed) {
        return Result::failed(stageResult.diagnostics());
    }
    const auto& outcome = stageResult.value();
    if (!outcome.has_value() || *outcome == nullptr) {
        core->notify();
        return Result::failed(previewDiagnostic("bloom.runtime.gpu-preview-missing-stage",
                                                "The CPU stage returned no outcome"));
    }
    if ((*outcome)->status == PreviewCpuStageStatus::Unsupported) {
        auto unsupported = std::make_shared<const PreviewPreparationResult>(
            PreviewPreparationResult::unsupported());
        auto result = Result::succeeded(std::move(unsupported), (*outcome)->diagnostics);
        core->notify();
        return result;
    }
    if ((*outcome)->stage == nullptr) {
        core->notify();
        return Result::failed(previewDiagnostic("bloom.runtime.gpu-preview-missing-frame",
                                                "The CPU stage produced no evaluated frame"));
    }
    auto result = core->fallback(*(*outcome)->stage, context);
    core->notify();
    return result;
}

[[nodiscard]] TaskSubmission<PreviewPreparationResultHandle> rootAdmissionRejected() {
    TaskSubmission<PreviewPreparationResultHandle> submission;
    submission.status = TaskSubmissionStatus::QueueFull;
    submission.diagnostic = previewDiagnostic(
        "bloom.runtime.gpu-preview-admission-closed",
        "The GPU preview display service is shutting down or its root admission bound is reached.");
    return submission;
}

// Submits on an already-held root admission reservation and consumes that reservation exactly once.
[[nodiscard]] TaskSubmission<PreviewPreparationResultHandle>
submitCpuRootReserved(const std::shared_ptr<detail::PreviewDisplayServiceCore>& core,
                      TaskRequest request, const document::Snapshot& snapshot,
                      const PreviewRequestIdentity& identity,
                      const std::size_t pixelStorageByteLimit,
                      const std::vector<SnapshotParameterOverride>& overrides) {
    request.executor = TaskExecutor::Cpu;
    try {
        auto submission = core->scheduler->submit<PreviewPreparationResultHandle>(
            std::move(request),
            [core, snapshot, identity, pixelStorageByteLimit, overrides](TaskContext& context) {
                return runCpuPipeline(core, snapshot, identity, pixelStorageByteLimit, overrides,
                                      context);
            });
        core->finishRootAdmission(submission.accepted() ? submission.handle.id() : TaskId{},
                                  submission.accepted());
        return submission;
    } catch (...) {
        core->abandonRootAdmission();
        throw;
    }
}

[[nodiscard]] std::chrono::milliseconds
serviceWaitInterval(const std::shared_ptr<detail::PreviewDisplayServiceCore>& core) {
    const bool active = !core->stages.empty() || core->nativeInFlight;
    return active ? std::chrono::milliseconds(2) : std::chrono::milliseconds(50);
}

// Samples no state; the caller captures the wake generation BEFORE doing work, so a notification
// that arrives during that work cannot be missed by the subsequent wait.
void waitForWake(const std::shared_ptr<detail::PreviewDisplayServiceCore>& core,
                 const std::uint64_t observedGeneration, const bool wakeOnStopping) {
    std::unique_lock lock(core->wakeMutex);
    core->wakeCondition.wait_for(lock, serviceWaitInterval(core), [&] {
        return (wakeOnStopping && core->stopping.load(std::memory_order_acquire)) ||
               core->wakeGeneration != observedGeneration;
    });
}

void retireNativeOnOwner(const std::shared_ptr<detail::PreviewDisplayServiceCore>& core) {
    core->nativeReady.clear();
    core->nativeActive.reset();
    core->nativeInFlight = false;
    // Native teardown drains/quarantines on this owner thread inside GpuNeutralDisplay.
    core->display.reset();
    core->device.reset();
    std::lock_guard lock(core->stateMutex);
    core->state = GpuPreviewDisplayServiceState::Stopped;
    core->gpuAvailable = false;
}

// The single drain path, including the exception path: no ownership deadline and no early release.
// Native is cancelled/quarantined first, then the loop waits for actual child terminality and
// native retirement before any completion is consumed. The poll interval is responsiveness only,
// and the drain wait still honors notifications even though stopping is already set.
void drainService(const std::shared_ptr<detail::PreviewDisplayServiceCore>& core) noexcept {
    // Request cooperative cancellation; ownership is retained until every child is terminal and the
    // native submission has actually retired. This single path also serves the exception path.
    for (const auto& stage : core->stages) {
        stage->cancellationRequested = true;
        if (stage->stageChild.isValid()) {
            stage->stageChild.cancel();
        }
        if (stage->fallbackChild.isValid()) {
            stage->fallbackChild.cancel();
        }
        if (stage->nativeDispatched) {
            stage->nativeDiscard = true;
        }
    }
    if (core->display != nullptr && core->nativeInFlight) {
        core->display->cancel();
    }

    // Never return with stages remaining: a wait/cancellation/lock failure is caught inside the
    // loop and the iteration is retried after yielding.
    while (!core->stages.empty()) {
        try {
            std::uint64_t observed = 0;
            {
                std::lock_guard lock(core->wakeMutex);
                observed = core->wakeGeneration;
            }
            if (core->lease != nullptr) {
                static_cast<void>(core->lease->dispatchOne());
            }
            detail::processPreviewStages(core);
            detail::processNativeDisplay(core);
            waitForWake(core, observed, /*wakeOnStopping=*/false);
        } catch (...) {
            std::this_thread::yield();
        }
    }
    retireNativeOnOwner(core);
}

void runServiceLoop(const std::shared_ptr<detail::PreviewDisplayServiceCore>& core) {
    if (core->lease == nullptr) {
        return;
    }
    try {
        while (true) {
            std::uint64_t observed = 0;
            {
                std::lock_guard lock(core->wakeMutex);
                if (core->stopping.load(std::memory_order_acquire)) {
                    break;
                }
                observed = core->wakeGeneration;
            }
            try {
                static_cast<void>(core->lease->dispatchOne());
                detail::processPreviewStages(core);
                detail::processNativeDisplay(core);
            } catch (...) {
                // A single iteration fault must not terminate the service thread.
            }
            waitForWake(core, observed, /*wakeOnStopping=*/true);
        }
        drainService(core);
    } catch (...) {
        // The exception path uses the same ownership contract as the normal drain.
        drainService(core);
    }
}

} // namespace

namespace detail {

bool gpuPreviewDisplayRequestIsNeutral(const PreviewRequestIdentity& identity) noexcept {
    return identity.requestGeneration != 0 && identity.output == PreviewOutput::Composition &&
           identity.colorIntent.workingColorSpaceId == kLinearRec709SceneColorSpaceId &&
           (identity.colorIntent.ocioConfigRevision == core::Sha256Digest{} ||
            identity.colorIntent.ocioConfigRevision == color::kBloomNeutralV1ConfigDigest) &&
           identity.viewAdjust == ViewAdjust{} &&
           (identity.displayName.empty() ||
            identity.displayName == kGpuNeutralDisplayDisplayName) &&
           (identity.viewName.empty() || identity.viewName == kGpuNeutralDisplayViewName);
}

std::optional<std::uint64_t>
gpuPreviewDisplayEstimatePixels(const PreviewRequestIdentity& identity) noexcept {
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    if (identity.roi.has_value()) {
        width = identity.roi->extent().width();
        height = identity.roi->extent().height();
    } else if (const auto* proxy = std::get_if<ProxyResolution>(&identity.resolution)) {
        width = proxy->extent.width();
        height = proxy->extent.height();
    } else {
        return std::nullopt;
    }
    if (width == 0 || height == 0) {
        return std::nullopt;
    }
    switch (identity.resolutionPolicy) {
    case PreviewResolutionPolicy::Half:
        return (width / 2) * (height / 2);
    case PreviewResolutionPolicy::Quarter:
        return (width / 4) * (height / 4);
    case PreviewResolutionPolicy::Auto:
    case PreviewResolutionPolicy::Full:
        return width * height;
    }
    return width * height;
}

std::uint64_t gpuPreviewDisplayHandoffOverheadMicros() noexcept {
    return kGpuPreviewActivePollMicros * 2;
}

bool gpuPreviewDisplaySelectsNative(const GpuNeutralDisplayQualificationReport& report,
                                    const std::uint64_t pixels,
                                    const std::uint64_t overheadMicros) noexcept {
    if (!report.eligible() || pixels == 0 || pixels > kGpuNeutralDisplayMaxPixels) {
        return false;
    }
    const auto& interval = report.eligibleInterval();
    if (!interval.has_value() || pixels < interval->min_pixels || pixels > interval->max_pixels) {
        return false;
    }
    const auto& timings = report.timings();
    if (timings.empty()) {
        return false;
    }
    const auto interpolate = [&timings](const std::uint64_t target, const bool native) {
        const GpuNeutralDisplayTimingSample* below = nullptr;
        const GpuNeutralDisplayTimingSample* above = nullptr;
        for (const auto& sample : timings) {
            if (sample.pixel_count == target) {
                return native ? sample.native_full_ms : sample.cpu_full_ms;
            }
            if (sample.pixel_count < target) {
                if (below == nullptr || sample.pixel_count > below->pixel_count) {
                    below = &sample;
                }
            } else if (above == nullptr || sample.pixel_count < above->pixel_count) {
                above = &sample;
            }
        }
        const auto value = [native](const GpuNeutralDisplayTimingSample& sample) {
            return native ? sample.native_full_ms : sample.cpu_full_ms;
        };
        if (below == nullptr) {
            return above == nullptr ? 0.0 : value(*above);
        }
        if (above == nullptr) {
            return value(*below);
        }
        const double span = static_cast<double>(above->pixel_count - below->pixel_count);
        if (span <= 0.0) {
            return value(*below);
        }
        const double t =
            (static_cast<double>(target) - static_cast<double>(below->pixel_count)) / span;
        return value(*below) + t * (value(*above) - value(*below));
    };
    const double nativeMs = interpolate(pixels, true);
    const double cpuMs = interpolate(pixels, false);
    const double overheadMs = static_cast<double>(overheadMicros) / 1000.0;
    return nativeMs + overheadMs < cpuMs;
}

namespace {
void pruneTerminalRootsLocked(std::vector<TaskId>& roots, TaskScheduler& scheduler) {
    std::erase_if(roots, [&scheduler](const TaskId id) {
        const auto snapshot = scheduler.snapshot(id);
        return !snapshot.has_value() || isTerminal(snapshot->state);
    });
}
} // namespace

bool PreviewDisplayServiceCore::beginRootAdmission() noexcept {
    std::lock_guard lock(rootsMutex);
    if (shuttingDown) {
        return false;
    }
    if (scheduler != nullptr) {
        pruneTerminalRootsLocked(acceptedRoots, *scheduler);
    }
    if (acceptedRoots.size() + pendingAdmissions >= kMaxAcceptedServiceRoots) {
        return false;
    }
    ++pendingAdmissions;
    return true;
}

void PreviewDisplayServiceCore::finishRootAdmission(const TaskId id, const bool accepted) noexcept {
    bool cancelNow = false;
    {
        std::lock_guard lock(rootsMutex);
        if (pendingAdmissions > 0) {
            --pendingAdmissions;
        }
        if (accepted) {
            if (shuttingDown) {
                cancelNow = true;
            } else {
                acceptedRoots.push_back(id);
            }
        }
    }
    if (cancelNow && scheduler != nullptr) {
        static_cast<void>(scheduler->cancel(id));
    }
}

void PreviewDisplayServiceCore::abandonRootAdmission() noexcept {
    std::lock_guard lock(rootsMutex);
    if (pendingAdmissions > 0) {
        --pendingAdmissions;
    }
}

void PreviewDisplayServiceCore::trackRoot(const TaskId id) noexcept {
    bool cancelNow = false;
    {
        std::lock_guard lock(rootsMutex);
        if (shuttingDown) {
            cancelNow = true;
        } else {
            acceptedRoots.push_back(id);
        }
    }
    if (cancelNow && scheduler != nullptr) {
        static_cast<void>(scheduler->cancel(id));
    }
}

void PreviewDisplayServiceCore::beginShutdownRoots(std::vector<TaskId>& out) noexcept {
    std::lock_guard lock(rootsMutex);
    shuttingDown = true;
    if (scheduler != nullptr) {
        pruneTerminalRootsLocked(acceptedRoots, *scheduler);
    }
    out.swap(acceptedRoots);
}

} // namespace detail

struct GpuPreviewDisplayService::Impl final {
    std::shared_ptr<detail::PreviewDisplayServiceCore> core;
    std::jthread thread;
    bool attached = false;
};

std::shared_ptr<detail::PreviewDisplayServiceCore>
detail::GpuPreviewDisplayServiceTestAccess::coreOf(const GpuPreviewDisplayService& service) {
    return service.impl_ == nullptr ? nullptr : service.impl_->core;
}

GpuPreviewDisplayService::GpuPreviewDisplayService(TaskScheduler& scheduler,
                                                   PreviewCpuStageFunction stageFunction,
                                                   PreviewCpuDisplayFallback displayFallback,
                                                   GpuPreviewDisplayServiceOptions options)
    : impl_(std::make_unique<Impl>()) {
    auto core = std::make_shared<detail::PreviewDisplayServiceCore>();
    core->scheduler = &scheduler;
    core->options = std::move(options);
    core->stageFunction = std::move(stageFunction);
    core->fallback = std::move(displayFallback);
    impl_->core = core;

    if (!core->options.enabled) {
        core->publishState(GpuPreviewDisplayServiceState::Unavailable, false,
                           {GpuPreviewDisplayServiceDiagnosticCode::Disabled,
                            "The GPU preview display service is disabled."},
                           nullptr);
        return;
    }

    core->generation = allocateGpuServiceGeneration();
    auto attachment = scheduler.attachGpuExecutor(core->generation, [core] { core->notify(); });
    if (!attachment.attached()) {
        core->publishState(GpuPreviewDisplayServiceState::Unavailable, false,
                           {GpuPreviewDisplayServiceDiagnosticCode::LoaderUnavailable,
                            attachment.diagnostic.has_value()
                                ? attachment.diagnostic->summary
                                : std::string("The scheduler GPU executor could not be attached.")},
                           nullptr);
        return;
    }
    core->lease = std::make_shared<GpuExecutorLease>(std::move(attachment.lease));
    impl_->attached = true;

    TaskRequest startupRequest("GPU preview display startup", startupOwner(core->generation),
                               TaskPriority::Interactive, TaskExecutor::Gpu);
    startupRequest.coalescingKey = std::string("bloom.preview.gpu.startup");
    auto startup = scheduler.submitGpu<int>(
        std::move(startupRequest), core->generation, GpuTaskAdmission{0, 0},
        [core](TaskContext& context, GpuTaskCompletion<int> completion) {
            detail::runGpuStartup(core, context, std::move(completion));
        });
    if (!startup.accepted()) {
        core->lease.reset();
        impl_->attached = false;
        core->publishState(GpuPreviewDisplayServiceState::Unavailable, false,
                           {GpuPreviewDisplayServiceDiagnosticCode::AdmissionUnavailable,
                            "The GPU preview display startup task was not admitted."},
                           nullptr);
        return;
    }
    core->trackRoot(startup.handle.id());

    impl_->thread = std::jthread([core] { runServiceLoop(core); });
}

GpuPreviewDisplayService::~GpuPreviewDisplayService() {
    if (impl_ == nullptr) {
        return;
    }
    beginShutdown();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    if (impl_->core != nullptr) {
        impl_->core->lease.reset();
    }
}

GpuPreviewDisplayServiceStatus GpuPreviewDisplayService::status() const {
    GpuPreviewDisplayServiceStatus result;
    if (impl_ == nullptr || impl_->core == nullptr) {
        return result;
    }
    const auto& core = *impl_->core;
    std::lock_guard lock(core.stateMutex);
    result.state = core.state;
    result.gpuAvailable = core.gpuAvailable;
    result.qualification = core.qualification;
    result.diagnostic = core.diagnostic;
    return result;
}

TaskSubmission<PreviewPreparationResultHandle>
GpuPreviewDisplayService::submit(TaskRequest request, const document::Snapshot& snapshot,
                                 const PreviewRequestIdentity& identity,
                                 const std::size_t pixelStorageByteLimit,
                                 const std::vector<SnapshotParameterOverride>& overrides) {
    auto core = impl_->core;
    // Admission is synchronized with beginShutdown(): a submission that races shutdown is rejected
    // here, never converted into new work.
    if (!core->beginRootAdmission()) {
        return rootAdmissionRejected();
    }

    const auto current = status();
    const bool gpuMode = current.state == GpuPreviewDisplayServiceState::Ready &&
                         current.gpuAvailable && current.qualification != nullptr &&
                         current.qualification->eligible();
    const std::size_t allowance =
        pixelStorageByteLimit != 0 ? pixelStorageByteLimit : core->options.previewByteAllowance;
    if (!gpuMode || pixelStorageByteLimit == 0 || allowance > core->options.previewByteAllowance ||
        !detail::gpuPreviewDisplayRequestIsNeutral(identity)) {
        // Reference/unavailable/disabled, oversize admission, or non-neutral: ordinary CPU task
        // before any stage work on the held reservation.
        return submitCpuRootReserved(core, std::move(request), snapshot, identity,
                                     pixelStorageByteLimit, overrides);
    }
    if (const auto estimatedPixels = detail::gpuPreviewDisplayEstimatePixels(identity);
        estimatedPixels.has_value() &&
        !detail::gpuPreviewDisplaySelectsNative(*current.qualification, *estimatedPixels,
                                                detail::gpuPreviewDisplayHandoffOverheadMicros())) {
        // Obviously tiny (or otherwise not worth the two service handoff intervals).
        return submitCpuRootReserved(core, std::move(request), snapshot, identity,
                                     pixelStorageByteLimit, overrides);
    }

    detail::PreviewStageSubmission stageSubmission(
        snapshot, identity, pixelStorageByteLimit, overrides, request.priority, request.owner,
        request.groupId, request.sourceVersion, request.coalescingKey);
    TaskRequest cpuRequest = request;
    try {
        auto submission = core->scheduler->submitGpu<PreviewPreparationResultHandle>(
            std::move(request), core->generation,
            GpuTaskAdmission{.queuedCommandBytes = 0, .requestOwnedBytes = allowance},
            [core, stageSubmission = std::move(stageSubmission)](
                TaskContext& context,
                GpuTaskCompletion<PreviewPreparationResultHandle> completion) mutable {
                detail::startGpuPreviewStage(core, context.isCancellationRequested(),
                                             std::move(completion), std::move(stageSubmission));
            });
        if (submission.accepted()) {
            core->finishRootAdmission(submission.handle.id(), true);
            return submission;
        }
        if (submission.status == TaskSubmissionStatus::ShuttingDown) {
            core->finishRootAdmission({}, false);
            return submission;
        }
        // Admission unavailable before any stage work: ordinary CPU task on the held reservation.
        return submitCpuRootReserved(core, std::move(cpuRequest), snapshot, identity,
                                     pixelStorageByteLimit, overrides);
    } catch (...) {
        core->abandonRootAdmission();
        throw;
    }
}

void GpuPreviewDisplayService::beginShutdown() noexcept {
    if (impl_ == nullptr || impl_->core == nullptr) {
        return;
    }
    auto core = impl_->core;
    core->shutdownRequested.store(true, std::memory_order_release);
    {
        std::lock_guard lock(core->wakeMutex);
        core->stopping.store(true, std::memory_order_release);
        ++core->wakeGeneration;
    }
    core->wakeCondition.notify_all();
    {
        std::lock_guard lock(core->stateMutex);
        if (core->state != GpuPreviewDisplayServiceState::Stopped) {
            core->state = GpuPreviewDisplayServiceState::Stopping;
        }
    }

    // Cancel only this service's own accepted roots (ordinary CPU fallbacks, queued GPU parents,
    // and the queued startup), synchronized with admission above.
    std::vector<TaskId> roots;
    core->beginShutdownRoots(roots);
    if (core->scheduler != nullptr) {
        for (const TaskId id : roots) {
            static_cast<void>(core->scheduler->cancel(id));
        }
    }
}

} // namespace bloom::runtime
