#include "gpu_preview_display_service_loop.hpp"
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

[[nodiscard]] std::chrono::milliseconds
serviceWaitInterval(const std::shared_ptr<detail::PreviewDisplayServiceCore>& core) {
    // Any preview stage, queued native job, or in-flight native submission needs the short poll.
    const bool previewActive = !core->stages.empty() || core->nativeInFlight ||
                               core->residentNativeInFlight || !core->nativeReady.empty() ||
                               !core->residentNativeReady.empty();
    if (previewActive) {
        return std::chrono::milliseconds(2);
    }
    // A live presentation generation keeps the short interval only while a target still needs the
    // owner to drive it (attach/resize/retire progress or an extracted/un-ingested request). A
    // stable idle Active target is woken by the coordinator's own wake hook the moment a new frame,
    // resize, or retirement arrives, so it waits on the notification instead of polling at 500 Hz.
    if (core->presentation != nullptr && core->presentation->available &&
        !core->presentationRetired && core->presentation->coordinator != nullptr &&
        core->presentation->coordinator->hasPendingWork()) {
        return std::chrono::milliseconds(2);
    }
    // Bounded idle wait: notifications still interrupt it immediately, so this only caps how long
    // an otherwise idle owner sleeps before re-checking.
    return std::chrono::milliseconds(50);
}

// Samples no state; the caller captures the wake generation BEFORE doing work, so a notification
// that arrives during that work cannot be missed by the subsequent wait. The interval is computed
// before the wake mutex is taken so the presentation coordinator's mailbox lock is never nested
// inside the service wake lock.
void waitForWake(const std::shared_ptr<detail::PreviewDisplayServiceCore>& core,
                 const std::uint64_t observedGeneration, const bool wakeOnStopping) {
    const auto interval = serviceWaitInterval(core);
    std::unique_lock lock(core->wakeMutex);
    core->wakeCondition.wait_for(lock, interval, [&] {
        return (wakeOnStopping && core->stopping.load(std::memory_order_acquire)) ||
               core->wakeGeneration != observedGeneration;
    });
}

void retireNativeOnOwner(const std::shared_ptr<detail::PreviewDisplayServiceCore>& core) {
    core->nativeReady.clear();
    core->nativeActive.reset();
    core->nativeInFlight = false;
    // Presentation owns the resident-frame lease registry and the coordinator on this same device.
    // Retire it first: coordinator, then registry, then the compute pipeline, then the device. The
    // helper keeps pumping through pending retirement and reports an unproven/quarantined target
    // rather than a false safe ack.
    detail::retireServicePresentation(core);
    // Resident native ownership (executor -> display -> scene cache) drains/cancels on this owner
    // thread; any unproven submission is retained by the owned pipeline until it proves retirement
    // or is destroyed. Then the packed native teardown drains/quarantines in GpuNeutralDisplay.
    core->residentNativeReady.clear();
    core->residentNativeActive.reset();
    core->residentNativeInFlight = false;
    detail::retireResidentRoute(core);
    core->display.reset();
    if (core->presentationRetirementUnproven) {
        // A native presentation generation could not be proven retired. The process quarantine now
        // holds raw native targets that reference this device, so the device is deliberately
        // retained rather than destroyed out from under them. This is an explicit, reported
        // retention (never a fabricated safe ack) and the host must not tear down its Qt surfaces.
        static_cast<void>(core->device.release()); // NOLINT(bugprone-unused-return-value)
    } else {
        core->device.reset();
    }
    std::lock_guard lock(core->stateMutex);
    core->state = GpuPreviewDisplayServiceState::Stopped;
    core->gpuAvailable = false;
}

// The single drain path, including the exception path: no ownership deadline and no early release.
// Native is cancelled/quarantined first, then the loop waits for actual child terminality and
// native retirement before any completion is consumed. The poll interval is responsiveness only,
// and the drain wait still honors notifications even though stopping is already set.
void drainService(const std::shared_ptr<detail::PreviewDisplayServiceCore>& core) noexcept {
    // Begin presentation retirement immediately on the owner thread so its pump runs alongside the
    // preview drain. This refuses new native targets and asks every live target to retire; it never
    // publishes a false safe ack.
    detail::beginServicePresentationShutdown(core);
    // Request cooperative cancellation; ownership is retained until every child is terminal and the
    // native submission has actually retired. This single path also serves the exception path.
    for (const auto& stage : core->stages) {
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
        }
    }
    if (core->display != nullptr && core->nativeInFlight) {
        core->display->cancel();
    }
    if (core->residentNativeInFlight) {
        detail::residentCancelNative(core);
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
            detail::processResidentNativeDisplay(core);
            detail::pumpServicePresentation(core);
            waitForWake(core, observed, /*wakeOnStopping=*/false);
        } catch (...) {
            std::this_thread::yield();
        }
    }
    // Keep the owner pumping through pending presentation retirement even when no preview stage is
    // left. The coordinator's own bounded drain budget terminates this loop.
    while (detail::servicePresentationNeedsPump(core)) {
        try {
            if (core->lease != nullptr) {
                static_cast<void>(core->lease->dispatchOne());
            }
            detail::pumpServicePresentation(core);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
                detail::processResidentNativeDisplay(core);
                // Presentation must be pumped on every iteration, even when no preview task is
                // outstanding, so native acquire/present/retire progresses and the shutdown
                // snapshot stays current.
                detail::pumpServicePresentation(core);
            } catch (...) { // NOLINT(bugprone-empty-catch)
                // A single iteration fault must not terminate the service thread; there is no
                // reporting channel on this owner loop, so the fault is deliberately contained.
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
    // A request ROI and an explicit ProxyResolution are already fully resolved output geometry: the
    // resolutionPolicy is the REQUESTED policy used to derive the proxy from the composition
    // format, not a second reduction of an already-resolved extent. Applying Half/Quarter again
    // here double-halves a Half/Quarter proxy, which is the narrow bug this fixes. Only the
    // CompositionFormatResolution case would need the policy, and its exact size is not known from
    // the identity alone, so it stays std::nullopt (the caller then does not use the estimate as a
    // refusal gate).
    if (identity.roi.has_value()) {
        const std::uint64_t width = identity.roi->extent().width();
        const std::uint64_t height = identity.roi->extent().height();
        return (width == 0 || height == 0) ? std::nullopt
                                           : std::optional<std::uint64_t>(width * height);
    }
    if (const auto* proxy = std::get_if<ProxyResolution>(&identity.resolution)) {
        const std::uint64_t width = proxy->extent.width();
        const std::uint64_t height = proxy->extent.height();
        return (width == 0 || height == 0) ? std::nullopt
                                           : std::optional<std::uint64_t>(width * height);
    }
    return std::nullopt;
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

bool detail::GpuPreviewDisplayServiceTestAccess::requestPresentationTestLease(
    GpuPreviewDisplayService& service, PresentationTestLeaseResult& out,
    const std::chrono::milliseconds timeout, const bool foreign) {
    auto core = coreOf(service);
    if (core == nullptr) {
        out = PresentationTestLeaseResult{};
        out.diagnostic = "the service has no core";
        return false;
    }
    return detail::requestServicePresentationTestLease(core, out, timeout, foreign);
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
    core->effectiveResidentLeaseBudgets = core->options.residentLeaseBudgets;
    core->effectiveResidentSceneCacheBudgets = core->options.residentSceneCacheBudgets;
    core->effectivePreviewByteAllowance.store(core->options.previewByteAllowance,
                                              std::memory_order_relaxed);
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

GpuPreviewDisplayService::GpuPreviewDisplayService(TaskScheduler& scheduler,
                                                   PreviewGpuSceneStageFunction gpuStageFunction,
                                                   PreviewCpuStageFunction cpuStageFunction,
                                                   PreviewCpuDisplayFallback displayFallback,
                                                   GpuPreviewDisplayServiceOptions options)
    : impl_(std::make_unique<Impl>()) {
    auto core = std::make_shared<detail::PreviewDisplayServiceCore>();
    core->scheduler = &scheduler;
    core->options = std::move(options);
    core->stageFunction = std::move(cpuStageFunction);
    core->gpuStageFunction = std::move(gpuStageFunction);
    core->fallback = std::move(displayFallback);
    core->effectiveResidentLeaseBudgets = core->options.residentLeaseBudgets;
    core->effectiveResidentSceneCacheBudgets = core->options.residentSceneCacheBudgets;
    core->effectivePreviewByteAllowance.store(core->options.previewByteAllowance,
                                              std::memory_order_relaxed);
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
    result.presentationAvailability = core.publishedPresentationAvailability;
    result.presentationDetail = core.publishedPresentationDetail;
    result.presentationClient = core.publishedPresentationClient;
    result.presentationShutdown = core.publishedPresentationShutdown;
    result.residentQualification = core.publishedResidentQualification;
    result.residentDetail = core.publishedResidentDetail;
    result.residentCapacityPlan = core.publishedResidentCapacityPlan;
    result.counters.residentGraphJobs =
        core.counterResidentGraphJobs.load(std::memory_order_relaxed);
    result.counters.nativeDispatches = core.counterNativeDispatches.load(std::memory_order_relaxed);
    result.counters.residentCompletions =
        core.counterResidentCompletions.load(std::memory_order_relaxed);
    result.counters.residentFailures = core.counterResidentFailures.load(std::memory_order_relaxed);
    result.counters.gpuCacheHits = core.counterGpuCacheHits.load(std::memory_order_relaxed);
    result.counters.gpuCacheMisses = core.counterGpuCacheMisses.load(std::memory_order_relaxed);
    result.counters.cpuFallbacks = core.counterCpuFallbacks.load(std::memory_order_relaxed);
    result.counters.fullFrameReadbacks =
        core.counterFullFrameReadbacks.load(std::memory_order_relaxed);
    result.counters.displayStatusReads =
        core.counterDisplayStatusReads.load(std::memory_order_relaxed);
    result.counters.residentLeaseRefusals =
        core.counterResidentLeaseRefusals.load(std::memory_order_relaxed);
    result.counters.retirementUnprovenTeardowns =
        core.counterRetirementUnprovenTeardowns.load(std::memory_order_relaxed);
    result.counters.residentQualificationMicros =
        core.counterResidentQualificationMicros.load(std::memory_order_relaxed);
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
    const bool residentMode = core->gpuStageFunction != nullptr;
    bool gpuMode = false;
    if (residentMode) {
        // Resident selection requires a usable presentation generation and a live route. It is
        // deliberately NOT gated on the Neutral-specific qualification (its measured pixel interval
        // and packed parity are a performance/parity gate for the Neutral shader, not a global
        // support gate): the per-request stage decides between a prepared general display program,
        // the startup Neutral fast path, and the CPU fallback. The packed readback qualification,
        // packed bandwidth, a fake qualification, and the raw document revision are all irrelevant.
        gpuMode = current.state == GpuPreviewDisplayServiceState::Ready && current.gpuAvailable &&
                  !core->residentRouteTerminal && core->presentation != nullptr &&
                  core->presentation->available && core->presentation->registry != nullptr;
    } else {
        gpuMode = current.state == GpuPreviewDisplayServiceState::Ready && current.gpuAvailable &&
                  current.qualification != nullptr && current.qualification->eligible();
    }
    // Two DISTINCT budgets. `pixelStorageByteLimit` is the caller's HOST pixel-storage ceiling for
    // decoding and the CPU fallback and is never lowered here. The device stage admits the request
    // with the host ceiling clamped down to the owner-resolved GPU request ceiling: a large host
    // ceiling is clamped, never treated as an oversize GPU request. A zero device ceiling (or zero
    // host ceiling) is zero device admission, which honestly takes the CPU path.
    const std::size_t gpuAllowance = static_cast<std::size_t>(
        gpuResidentRequestBudget(static_cast<std::uint64_t>(pixelStorageByteLimit),
                                 static_cast<std::uint64_t>(core->previewByteAllowance())));
    // In resident mode the per-request stage decides between the general display program (which may
    // be a non-default display/view and a non-neutral ViewAdjust), the startup Neutral fast path,
    // and the CPU fallback. The submit gate therefore must NOT preempt a non-neutral request: doing
    // so would re-introduce the old neutral-only gate and silently drop every prepared general
    // display program. The packed (non-resident) arm keeps the neutral requirement.
    const bool neutralRequired = !residentMode;
    if (!gpuMode || gpuAllowance == 0 ||
        (neutralRequired && !detail::gpuPreviewDisplayRequestIsNeutral(identity))) {
        // Reference/unavailable/disabled, no device admission, or a packed non-neutral request:
        // ordinary CPU task before any stage work on the held reservation. For the resident arm an
        // unavailable presentation generation takes this honest CPU fallback.
        return submitCpuRootReserved(core, std::move(request), snapshot, identity,
                                     pixelStorageByteLimit, overrides);
    }
    if (!residentMode) {
        if (const auto estimatedPixels = detail::gpuPreviewDisplayEstimatePixels(identity);
            estimatedPixels.has_value() && !detail::gpuPreviewDisplaySelectsNative(
                                               *current.qualification, *estimatedPixels,
                                               detail::gpuPreviewDisplayHandoffOverheadMicros())) {
            // Obviously tiny (or otherwise not worth the two service handoff intervals).
            return submitCpuRootReserved(core, std::move(request), snapshot, identity,
                                         pixelStorageByteLimit, overrides);
        }
    }

    detail::PreviewStageSubmission stageSubmission(
        snapshot, identity, pixelStorageByteLimit, gpuAllowance, overrides, request.priority,
        request.owner, request.groupId, request.sourceVersion, request.coalescingKey);
    TaskRequest cpuRequest = request;
    try {
        auto submission = core->scheduler->submitGpu<PreviewPreparationResultHandle>(
            std::move(request), core->generation,
            GpuTaskAdmission{.queuedCommandBytes = 0, .requestOwnedBytes = gpuAllowance},
            [core, residentMode, stageSubmission = std::move(stageSubmission)](
                TaskContext& context,
                GpuTaskCompletion<PreviewPreparationResultHandle> completion) mutable {
                if (residentMode) {
                    detail::startResidentPreviewStage(core, context.isCancellationRequested(),
                                                      std::move(completion),
                                                      std::move(stageSubmission));
                } else {
                    detail::startGpuPreviewStage(core, context.isCancellationRequested(),
                                                 std::move(completion), std::move(stageSubmission));
                }
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
    // The owner loop observes this and issues the coordinator's owner-thread beginShutdown(); the
    // client's own mailbox admission closes immediately through the coordinator's pump.
    core->presentationShutdownRequested.store(true, std::memory_order_release);
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
