// Resident-route generation for GpuPreviewDisplayService. See
// gpu_preview_display_service_resident_private.hpp for the contract. Every call here runs on the
// service owner thread with the service's own device; no second thread/device/service exists.

#include "gpu_preview_display_service_private.hpp"

#include <bloom/color/ocio_cpu_display_frame.hpp>
#include <bloom/render/gpu_composite.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/gpu_resident_preview_product.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace bloom::runtime::detail {
namespace {

void publishResidentDetail(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                           std::string detail) {
    std::lock_guard lock(core->stateMutex);
    core->publishedResidentDetail = std::move(detail);
}

} // namespace

bool createAndQualifyResidentRoute(
    const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    if (core->gpuStageFunction == nullptr) {
        publishResidentStatus(core);
        return false;
    }
    if (core->device == nullptr || !core->device->isOwnerThread()) {
        publishResidentStatus(core);
        return false;
    }
    if (core->processor == nullptr) {
        publishResidentDetail(core, "the CPU display processor is unavailable");
        publishResidentStatus(core);
        return false;
    }
    try {
        auto cache = GpuSceneCache::create(*core->device, core->options.residentSceneCacheBudgets);
        if (!cache) {
            publishResidentDetail(core,
                                  cache.diagnostic.message.empty()
                                      ? std::string("the resident scene cache could not be created")
                                      : cache.diagnostic.message);
            publishResidentStatus(core);
            return false;
        }
        core->residentSceneCache = std::move(cache.cache);
        auto executor = GpuSceneExecutor::create(*core->device, *core->residentSceneCache,
                                                 core->options.residentExecutorBudgets);
        if (!executor) {
            retireResidentRoute(core);
            publishResidentDetail(
                core, executor.diagnostic.message.empty()
                          ? std::string("the resident scene executor could not be created")
                          : executor.diagnostic.message);
            publishResidentStatus(core);
            return false;
        }
        core->residentExecutor = std::move(executor.executor);
        auto display =
            render::GpuResidentDisplay::create(*core->device, core->options.residentDisplayBudgets);
        if (!display) {
            retireResidentRoute(core);
            publishResidentDetail(core,
                                  display.diagnostic.message.empty()
                                      ? std::string("the resident display could not be created")
                                      : display.diagnostic.message);
            publishResidentStatus(core);
            return false;
        }
        core->residentDisplay = std::move(display.display);

        // Transient qualification pipelines. The persistent resident display is the service's own.
        auto solid = render::GpuSolid::create(*core->device);
        auto upload = render::GpuImageUpload::create(*core->device);
        auto composite = render::GpuComposite::create(*core->device);
        if (!solid || !upload || !composite) {
            retireResidentRoute(core);
            publishResidentDetail(core,
                                  "the resident qualification pipelines could not be created");
            publishResidentStatus(core);
            return false;
        }
        GpuResidentPreviewPipelines pipelines{solid.solid.get(), upload.upload.get(),
                                              composite.composite.get(),
                                              core->residentDisplay.get()};
        auto predicate = [core]() noexcept {
            return core->shutdownRequested.load(std::memory_order_acquire);
        };
        const auto started = std::chrono::steady_clock::now();
        auto report = qualifyResidentPreview(*core->processor, *core->device, pipelines,
                                             core->options.residentQualificationBudgets,
                                             color::CancellationPredicateRef(predicate));
        const auto elapsed = std::chrono::steady_clock::now() - started;
        core->counterResidentQualificationMicros.store(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()),
            std::memory_order_relaxed);
        auto pointer =
            std::make_shared<const GpuResidentPreviewQualificationReport>(std::move(report));
        core->residentQualification = pointer;
        if (!pointer->eligible()) {
            // A deadline/cancellation/unproven failure during qualification may leave the
            // persistent resident display owning an unretired submission. Drain/destroy the
            // owner-thread pipelines explicitly before publishing an uneligible report, rather than
            // storing the report and keeping an unretired pipeline around for reuse.
            residentCancelNative(core);
            retireResidentRoute(core);
            publishResidentDetail(core, pointer->diagnostic().message.empty()
                                            ? std::string("the resident route did not qualify")
                                            : pointer->diagnostic().message);
        }
        publishResidentStatus(core);
        return pointer->eligible();
    } catch (...) {
        residentCancelNative(core);
        retireResidentRoute(core);
        publishResidentDetail(core, "the resident qualification raised");
        publishResidentStatus(core);
        return false;
    }
}

void publishResidentStatus(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    std::string detail;
    if (core->gpuStageFunction == nullptr) {
        detail = "the resident route was not requested";
    } else if (core->residentQualification == nullptr) {
        detail = "the resident qualification has not run";
    } else if (!core->residentQualification->eligible()) {
        detail = core->residentQualification->diagnostic().message.empty()
                     ? std::string("the resident route did not qualify")
                     : core->residentQualification->diagnostic().message;
    }
    std::lock_guard lock(core->stateMutex);
    core->publishedResidentQualification = core->residentQualification;
    if (!detail.empty()) {
        core->publishedResidentDetail = std::move(detail);
    }
}

void retireResidentRoute(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    if (core->residentDisplay != nullptr) {
        core->residentDisplay->cancel();
        core->residentDisplay.reset();
    }
    if (core->residentExecutor != nullptr) {
        core->residentExecutor->cancel();
        for (int pump = 0; pump < 64 && core->residentExecutor->ownerDrainRequired(); ++pump) {
            static_cast<void>(core->residentExecutor->poll());
        }
        core->residentExecutor.reset();
    }
    core->residentSceneCache.reset();
}

bool residentStageIsEligible(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                             const PreviewDisplayStageRecord& stage, std::string& reason) noexcept {
    if (core->gpuStageFunction == nullptr || core->residentExecutor == nullptr ||
        core->residentDisplay == nullptr || core->residentRouteTerminal) {
        reason = core->residentRouteTerminal
                     ? "the resident route is terminal for this service generation"
                     : "the resident route is not available for this service generation";
        return false;
    }
    if (stage.gpuStage == nullptr || stage.gpuStage->scene() == nullptr) {
        reason = "the resident stage carries no prepared GPU scene";
        return false;
    }
    if (core->device == nullptr || !core->device->isOwnerThread()) {
        reason = "the resident selection ran off the device owner thread";
        return false;
    }
    if (core->processor == nullptr || stage.gpuStage->displayProcessor() == nullptr) {
        reason = "no qualified display processor is selected for the resident route";
        return false;
    }
    if (core->residentQualification == nullptr || !core->residentQualification->eligible() ||
        !core->residentQualification->eligibleFor(*core->device, *core->processor)) {
        reason = "the resident qualification report is not eligible for this device and processor";
        return false;
    }
    if (!gpuResidentPreviewProcessorIsEligible(*stage.gpuStage->displayProcessor(), reason)) {
        return false;
    }
    if (core->presentation == nullptr || !core->presentation->available ||
        core->presentation->registry == nullptr) {
        reason = "no usable presentation generation exists for the resident route";
        return false;
    }
    if (!gpuPreviewDisplayRequestIsNeutral(stage.gpuStage->desiredIdentity())) {
        reason = "the request is not neutral for the resident route";
        return false;
    }

    const auto* scene = stage.gpuStage->scene().get();
    const auto& descriptor = scene->outputDescriptor();
    const std::uint64_t width = descriptor.dataWindow().extent().width();
    const std::uint64_t height = descriptor.dataWindow().extent().height();
    if (width == 0 || height == 0) {
        reason = "the prepared scene output descriptor is empty";
        return false;
    }
    const std::uint64_t pixels = width * height;
    const auto& interval = core->residentQualification->eligibleInterval();
    if (!interval.has_value() || pixels < interval->min_pixels || pixels > interval->max_pixels) {
        reason = "the request area is outside the measured resident eligible interval";
        return false;
    }
    if (pixels > kGpuResidentDisplayMaxPixels) {
        reason = "the request area exceeds the 4K resident ceiling";
        return false;
    }
    const std::uint64_t requestedBytes =
        pixels * sizeof(render::Rgba32f) + pixels * sizeof(render::Rgba8);
    const std::uint64_t allowance = stage.pixelStorageByteLimit != 0
                                        ? stage.pixelStorageByteLimit
                                        : core->options.previewByteAllowance;
    if (allowance != 0 && requestedBytes > allowance) {
        reason = "the request byte allowance cannot hold the resident input and display";
        return false;
    }
    return true;
}

bool residentExecutorBegin(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                           const PreviewDisplayStageRecord& stage, std::string& reason) noexcept {
    if (core->residentExecutor == nullptr) {
        reason = "the resident executor is unavailable";
        return false;
    }
    const std::uint64_t budget = stage.pixelStorageByteLimit != 0
                                     ? stage.pixelStorageByteLimit
                                     : core->options.previewByteAllowance;
    const auto diagnostic = core->residentExecutor->begin(stage.gpuStage->scene(), budget);
    if (diagnostic.code != GpuSceneExecutorDiagnosticCode::None) {
        reason = diagnostic.message.empty() ? std::string("the resident scene begin was refused")
                                            : diagnostic.message;
        return false;
    }
    // A graph job was admitted. This is NOT a native operation count; the actual native dispatch
    // count is read from the executor's own counters once it has run (a warm cache hit leaves it
    // unchanged).
    core->counterResidentGraphJobs.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool residentDisplayHasUnretiredSubmission(
    const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    if (core->residentDisplay == nullptr) {
        return false;
    }
    if (core->residentDisplayUnretiredOverride) {
        return core->residentDisplayUnretiredOverride(*core->residentDisplay);
    }
    return core->residentDisplay->hasUnretiredSubmission();
}

bool residentExecutorHasUnretiredSubmission(
    const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    if (core->residentExecutor == nullptr) {
        return false;
    }
    if (core->residentExecutorUnretiredOverride) {
        return core->residentExecutorUnretiredOverride(*core->residentExecutor);
    }
    return core->residentExecutor->hasUnretiredSubmission();
}

GpuSceneExecutorPollResult
residentExecutorPoll(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    if (core->residentExecutor == nullptr) {
        return GpuSceneExecutorPollResult::Failure;
    }
    if (core->residentExecutorPollOverride) {
        return core->residentExecutorPollOverride(*core->residentExecutor);
    }
    return core->residentExecutor->poll();
}

bool residentDisplayBegin(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                          const PreviewDisplayStageRecord& stage, std::string& reason) noexcept {
    if (core->residentDisplay == nullptr || core->residentExecutor == nullptr) {
        reason = "the resident display is unavailable";
        return false;
    }
    auto input = core->residentExecutor->takeImage();
    if (input == nullptr) {
        reason = "the resident scene executor produced no output image";
        return false;
    }
    const std::uint64_t budget = stage.pixelStorageByteLimit != 0
                                     ? stage.pixelStorageByteLimit
                                     : core->options.previewByteAllowance;
    const auto diagnostic = core->residentDisplay->begin(std::move(input), budget);
    if (diagnostic.code != render::GpuResidentDisplayDiagnosticCode::None) {
        reason = diagnostic.message.empty() ? std::string("the resident display begin was refused")
                                            : diagnostic.message;
        return false;
    }
    return true;
}

render::GpuResidentDisplayPollResult
residentDisplayPoll(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    if (core->residentDisplay == nullptr) {
        return render::GpuResidentDisplayPollResult::Failure;
    }
    if (core->residentDisplayPollOverride) {
        return core->residentDisplayPollOverride(*core->residentDisplay);
    }
    return core->residentDisplay->poll();
}

std::optional<PreparedPreviewFrame>
residentFinishFrame(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                    const PreviewDisplayStageRecord& stage, std::string& reason) noexcept {
    if (core->residentDisplay == nullptr || core->device == nullptr ||
        core->presentation == nullptr || core->presentation->registry == nullptr ||
        core->residentQualification == nullptr || stage.gpuStage == nullptr ||
        stage.gpuStage->scene() == nullptr || stage.residentProcessor == nullptr) {
        reason = "the resident product prerequisites are unavailable";
        return std::nullopt;
    }
    auto display = core->residentDisplay->takeImage();
    if (!display.isValid()) {
        reason = "the resident display produced no valid image";
        return std::nullopt;
    }
    auto sharedDisplay = std::make_shared<const render::GpuDisplayImage>(std::move(display));
    auto request = makeGpuResidentDisplayProductRequest(
        *stage.gpuStage->scene(), stage.gpuStage->desiredIdentity(), std::move(sharedDisplay));
    // The GPU-scene builder intentionally leaves the byte limit at 0 (a prepared scene carries the
    // per-request allowance separately), so the service charges the SAME per-request allowance the
    // scene was prepared under. Without this the product factory treats 0 as an exhausted budget
    // and refuses every resident frame.
    request.pixelStorageByteLimit = stage.pixelStorageByteLimit != 0
                                        ? stage.pixelStorageByteLimit
                                        : core->options.previewByteAllowance;
    // Pre-validate with the authoritative eligibility check so a refusal is diagnosable (the
    // factory rechecks and publishes nothing on refusal). A registry budget/pin refusal leaves
    // every live lease and pin untouched; the pump collects released tokens and this request takes
    // the CPU fallback. No permanent GPU poison is latched.
    std::string eligibilityReason;
    if (!gpuResidentDisplayProductIsEligible(*core->device, *core->presentation->registry,
                                             *stage.residentProcessor, *core->residentQualification,
                                             request, eligibilityReason)) {
        core->counterResidentLeaseRefusals.fetch_add(1, std::memory_order_relaxed);
        reason = eligibilityReason.empty() ? std::string("the resident product was not eligible")
                                           : eligibilityReason;
        publishResidentDetail(core, "resident product refused: " + reason);
        return std::nullopt;
    }
    auto frame = makeGpuResidentDisplayPreview(*core->device, *core->presentation->registry,
                                               *stage.residentProcessor,
                                               core->residentQualification, std::move(request));
    if (!frame.has_value()) {
        core->counterResidentLeaseRefusals.fetch_add(1, std::memory_order_relaxed);
        reason = "the resident product factory refused the prepared frame";
        publishResidentDetail(core, "resident product factory refused the prepared frame");
        return std::nullopt;
    }
    // The display job completed and its 4-byte status word was invalidated by the pipeline. That is
    // NOT a full-frame readback; full-frame readbacks stay zero.
    core->counterDisplayStatusReads.fetch_add(1, std::memory_order_relaxed);
    core->counterResidentCompletions.fetch_add(1, std::memory_order_relaxed);
    return frame;
}

void residentCancelNative(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    if (core->residentExecutor != nullptr) {
        core->residentExecutor->cancel();
    }
    if (core->residentDisplay != nullptr) {
        core->residentDisplay->cancel();
    }
}

bool residentOwnerDrainRequired(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    return core->residentExecutor != nullptr && core->residentExecutor->ownerDrainRequired();
}

bool residentDeviceLost(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    return core->residentExecutor != nullptr && core->residentExecutor->deviceLost();
}

void noteResidentExecutorCounters(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    if (core->residentExecutor == nullptr) {
        return;
    }
    const auto counters = core->residentExecutor->counters();
    core->counterGpuCacheHits.store(counters.outputCacheHits + counters.commandCacheHits,
                                    std::memory_order_relaxed);
    core->counterGpuCacheMisses.store(counters.outputCacheMisses + counters.commandCacheMisses,
                                      std::memory_order_relaxed);
    // ACTUAL native operations performed by the executor. A warm cache hit that cuts the whole
    // subtree performs none, so this stays unchanged. `readbacks` is asserted zero by the
    // executor's own contract; it is never a full-frame host transfer.
    core->counterNativeDispatches.store(counters.solidDispatches + counters.coveredSolidDispatches +
                                            counters.translationDispatches +
                                            counters.sourceOverDispatches + counters.uploads,
                                        std::memory_order_relaxed);
    core->counterFullFrameReadbacks.store(counters.readbacks, std::memory_order_relaxed);
}

} // namespace bloom::runtime::detail
