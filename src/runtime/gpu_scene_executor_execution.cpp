#include <bloom/runtime/gpu_scene_executor.hpp>

#include "gpu_scene_executor_private.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bloom::runtime {
namespace {

using gpu_scene_executor_detail::descriptorMatches;
using gpu_scene_executor_detail::diagnosticFromComposite;
using gpu_scene_executor_detail::diagnosticFromSolid;
using gpu_scene_executor_detail::diagnosticFromUpload;
using gpu_scene_executor_detail::makeDiagnostic;
using gpu_scene_executor_detail::mapComposite;
using gpu_scene_executor_detail::mapSolid;
using gpu_scene_executor_detail::mapUpload;
using gpu_scene_executor_detail::NativePoll;

[[nodiscard]] bool solidDeviceLost(const render::GpuSolidDiagnostic& diagnostic) noexcept {
    return diagnostic.code == render::GpuSolidDiagnosticCode::DeviceLost;
}
[[nodiscard]] bool solidCancelled(const render::GpuSolidDiagnostic& diagnostic) noexcept {
    return diagnostic.code == render::GpuSolidDiagnosticCode::Cancelled;
}
[[nodiscard]] bool compositeDeviceLost(const render::GpuCompositeDiagnostic& diagnostic) noexcept {
    return diagnostic.code == render::GpuCompositeDiagnosticCode::DeviceLost;
}
[[nodiscard]] bool compositeCancelled(const render::GpuCompositeDiagnostic& diagnostic) noexcept {
    return diagnostic.code == render::GpuCompositeDiagnosticCode::Cancelled;
}
[[nodiscard]] bool
compositeStatusRejected(const render::GpuCompositeDiagnostic& diagnostic) noexcept {
    return diagnostic.code == render::GpuCompositeDiagnosticCode::StatusFlagRejected;
}
[[nodiscard]] bool uploadDeviceLost(const render::GpuImageUploadDiagnostic& diagnostic) noexcept {
    return diagnostic.code == render::GpuImageUploadDiagnosticCode::DeviceLost;
}
[[nodiscard]] bool uploadCancelled(const render::GpuImageUploadDiagnostic& diagnostic) noexcept {
    return diagnostic.code == render::GpuImageUploadDiagnosticCode::Cancelled;
}

} // namespace

void GpuSceneExecutor::Impl::clearJob() noexcept {
    scene.reset();
    steps.clear();
    for (auto& pin : images) {
        pin.reset();
    }
    images.clear();
    liveEntries.clear();
    liveBytes = 0;
    peakLiveBytes = 0;
    liveBytesAtDispatch = 0;
    remainingUses.clear();
    cursor = 0;
    requestBudget = 0;
    coverageBytes = 0;
    nativeKind = NativeKind::None;
    nativeInFlight = false;
    cancelRequested = false;
    cancelIssued = false;
    timeoutRequested = false;
    drainRequired = false;
}

bool GpuSceneExecutor::Impl::hasUnretiredNative() const noexcept {
    const bool solidUnretired = solid != nullptr && solid->hasUnretiredSubmission();
    const bool compositeUnretired = composite != nullptr && composite->hasUnretiredSubmission();
    const bool uploadUnretired = upload != nullptr && upload->hasUnretiredSubmission();
    return solidUnretired || compositeUnretired || uploadUnretired;
}

std::uint64_t GpuSceneExecutor::Impl::remainingBudget() const noexcept {
    if (liveBytes >= requestBudget) {
        return 0;
    }
    const std::uint64_t headroom = requestBudget - liveBytes;
    return headroom < budgets.maxImageBytes ? headroom : budgets.maxImageBytes;
}

void GpuSceneExecutor::Impl::assignImage(const GpuSceneCommandIndex index,
                                         std::shared_ptr<const render::GpuImage> pin) {
    if (index == kInvalidGpuSceneCommand || static_cast<std::size_t>(index) >= images.size()) {
        return;
    }
    if (pin == nullptr) {
        releaseImage(index);
        return;
    }
    if (images[index] != nullptr && images[index].get() == pin.get()) {
        images[index] = std::move(pin);
        return;
    }
    releaseImage(index);
    const auto* const raw = pin.get();
    auto& entry = liveEntries[raw];
    if (entry.owners == 0) {
        entry.bytes = pin->allocationBytes();
        liveBytes += entry.bytes;
    } else {
        // This exact image is already pinned under another command index; charge it once and record
        // the bytes that a naive per-index charge would have double-counted.
        counters.aliasedImagePinBytes += entry.bytes;
    }
    ++entry.owners;
    images[index] = std::move(pin);
    peakLiveBytes = std::max(peakLiveBytes, liveBytes);
    counters.currentLiveImageBytes = liveBytes;
    counters.peakLiveImageBytes = peakLiveBytes;
}

void GpuSceneExecutor::Impl::releaseImage(const GpuSceneCommandIndex index) noexcept {
    if (index == kInvalidGpuSceneCommand || static_cast<std::size_t>(index) >= images.size()) {
        return;
    }
    auto& pin = images[index];
    if (pin == nullptr) {
        return;
    }
    const auto* const raw = pin.get();
    const auto entry = liveEntries.find(raw);
    if (entry != liveEntries.end()) {
        if (--entry->second.owners == 0) {
            liveBytes -= entry->second.bytes;
            liveEntries.erase(entry);
        }
    }
    pin.reset();
    counters.currentLiveImageBytes = liveBytes;
}

void GpuSceneExecutor::Impl::consume(const GpuSceneCommandIndex index) noexcept {
    if (scene == nullptr || index == kInvalidGpuSceneCommand || index == scene->outputCommand()) {
        return;
    }
    if (static_cast<std::size_t>(index) >= remainingUses.size() || remainingUses[index] == 0) {
        return;
    }
    --remainingUses[index];
    if (remainingUses[index] == 0) {
        releaseImage(index);
        ++counters.intermediatePinsReleased;
    }
}

void GpuSceneExecutor::Impl::fail(const GpuSceneExecutorDiagnosticCode code, std::string message,
                                  const bool requireDrain) noexcept {
    diagnostic = makeDiagnostic(code, std::move(message));
    state = GpuSceneExecutorJobState::Failure;
    if (code == GpuSceneExecutorDiagnosticCode::Cancelled) {
        ++counters.scenesCancelled;
    }
    ++counters.scenesFailed;
    drainRequired = requireDrain;
    if (!requireDrain) {
        nativeInFlight = false;
        nativeKind = NativeKind::None;
        clearJob();
    }
    counters.currentLiveImageBytes = liveBytes;
}

void GpuSceneExecutor::Impl::finishReady() noexcept {
    if (scene == nullptr || images.empty() || images[scene->outputCommand()] == nullptr) {
        fail(GpuSceneExecutorDiagnosticCode::InternalInvariant, "the output image is missing",
             false);
        return;
    }
    state = GpuSceneExecutorJobState::Ready;
    diagnostic = {};
    ++counters.scenesCompleted;
    for (std::size_t i = 0; i < images.size(); ++i) {
        if (static_cast<GpuSceneCommandIndex>(i) != scene->outputCommand()) {
            releaseImage(static_cast<GpuSceneCommandIndex>(i));
        }
    }
}

void GpuSceneExecutor::Impl::advanceDrain() noexcept {
    if (nativeKind == NativeKind::Solid && solid != nullptr) {
        static_cast<void>(solid->poll());
    } else if (nativeKind == NativeKind::Composite && composite != nullptr) {
        static_cast<void>(composite->poll());
    } else if (nativeKind == NativeKind::Upload && upload != nullptr) {
        static_cast<void>(upload->poll());
    }
    if (!hasUnretiredNative()) {
        nativeInFlight = false;
        nativeKind = NativeKind::None;
        drainRequired = false;
        clearJob();
        state = GpuSceneExecutorJobState::Idle;
    }
}

GpuSceneExecutorDiagnostic GpuSceneExecutor::Impl::startStep(const GpuSceneExecutorStep& step) {
    const std::uint64_t remaining = remainingBudget();
    liveBytesAtDispatch = liveBytes;
    const auto solidParameters = [&step]() -> std::optional<render::GpuSolidParameters> {
        if (!step.solidDataWindow.has_value() || !step.solidDisplayWindow.has_value()) {
            return std::nullopt;
        }
        return render::GpuSolidParameters{step.solidPixel, *step.solidDataWindow,
                                          *step.solidDisplayWindow, step.solidPixelAspect};
    };
    switch (step.kind) {
    case GpuSceneExecutorStepKind::Solid: {
        const auto parameters = solidParameters();
        if (!parameters.has_value()) {
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                                  "a solid step has no window");
        }
        const auto native = solid->begin(*parameters, remaining);
        if (native.code != render::GpuSolidDiagnosticCode::None) {
            return diagnosticFromSolid(native);
        }
        ++counters.solidDispatches;
        break;
    }
    case GpuSceneExecutorStepKind::CoveredSolid: {
        const auto parameters = solidParameters();
        if (!parameters.has_value()) {
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                                  "a covered step has no window");
        }
        const auto native =
            solid->beginCovered(*parameters, step.coverage, step.coveredOpacity, remaining);
        if (native.code != render::GpuSolidDiagnosticCode::None) {
            return diagnosticFromSolid(native);
        }
        ++counters.coveredSolidDispatches;
        break;
    }
    case GpuSceneExecutorStepKind::Upload: {
        if (step.uploadSource == nullptr) {
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                                  "an upload step has no converted source before dispatch");
        }
        const auto native =
            upload->begin(render::GpuImageUploadParameters{step.uploadSource}, remaining);
        if (native.code != render::GpuImageUploadDiagnosticCode::None) {
            return diagnosticFromUpload(native);
        }
        ++counters.uploads;
        break;
    }
    case GpuSceneExecutorStepKind::Translation: {
        if (step.input == kInvalidGpuSceneCommand ||
            static_cast<std::size_t>(step.input) >= images.size() ||
            images[step.input] == nullptr || !step.outputWindow.has_value()) {
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                                  "a translation step is incomplete before dispatch");
        }
        const render::GpuTranslationParameters parameters{images[step.input], *step.outputWindow,
                                                          step.translationX, step.translationY,
                                                          step.translationOpacity};
        const auto native = composite->beginTranslation(parameters, remaining);
        if (native.code != render::GpuCompositeDiagnosticCode::None) {
            return diagnosticFromComposite(native);
        }
        ++counters.translationDispatches;
        break;
    }
    case GpuSceneExecutorStepKind::SourceOver: {
        if (step.input == kInvalidGpuSceneCommand ||
            static_cast<std::size_t>(step.input) >= images.size() ||
            images[step.input] == nullptr) {
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                                  "a source-over foreground disappeared before dispatch");
        }
        const auto destination =
            step.destination == kInvalidGpuSceneCommand
                ? (static_cast<std::size_t>(step.command) < images.size() ? images[step.command]
                                                                          : nullptr)
                : (static_cast<std::size_t>(step.destination) < images.size()
                       ? images[step.destination]
                       : nullptr);
        if (destination == nullptr) {
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                                  "a source-over destination disappeared before dispatch");
        }
        const render::GpuSourceOverParameters parameters{images[step.input], destination};
        const auto native = composite->beginSourceOver(parameters, remaining);
        if (native.code != render::GpuCompositeDiagnosticCode::None) {
            return diagnosticFromComposite(native);
        }
        ++counters.sourceOverDispatches;
        break;
    }
    }
    ++counters.dispatches;
    if (step.kind == GpuSceneExecutorStepKind::Solid ||
        step.kind == GpuSceneExecutorStepKind::CoveredSolid) {
        nativeKind = NativeKind::Solid;
    } else if (step.kind == GpuSceneExecutorStepKind::Upload) {
        nativeKind = NativeKind::Upload;
    } else {
        nativeKind = NativeKind::Composite;
    }
    nativeInFlight = true;
    cancelIssued = false;
    timeoutRequested = false;
    nativeStart = std::chrono::steady_clock::now();
    return {};
}

GpuSceneExecutorDiagnostic GpuSceneExecutor::Impl::finishNative(render::GpuImage produced) {
    // The op's own actual retained bytes (image plus its native metadata). The true during-op live
    // peak is the pins at dispatch plus this, so the request budget bounds the temporary peak too.
    const std::uint64_t opPeakBytes =
        nativeKind == NativeKind::Solid    ? solid->lastJobAllocationBytes()
        : nativeKind == NativeKind::Upload ? upload->lastJobAllocationBytes()
                                           : composite->lastJobAllocationBytes();
    nativeKind = NativeKind::None;
    nativeInFlight = false;
    if (!produced.isValid()) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                              "a native operation produced no image");
    }
    const GpuSceneExecutorStep& step = steps[cursor];
    const render::GpuImage* translationInput = nullptr;
    if (step.kind == GpuSceneExecutorStepKind::Translation &&
        step.input != kInvalidGpuSceneCommand &&
        static_cast<std::size_t>(step.input) < images.size()) {
        translationInput = images[step.input].get();
    }
    if (step.command != kInvalidGpuSceneCommand &&
        !descriptorMatches(scene->commands()[step.command], produced, translationInput)) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DescriptorMismatch,
                              "a native image descriptor did not match scene command " +
                                  std::to_string(step.command));
    }
    const std::uint64_t actual = produced.allocationBytes();
    if (actual >
        std::numeric_limits<std::uint64_t>::max() - counters.cumulativeProducedImageBytes) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::OverBudget,
                              "produced image byte accounting overflowed");
    }
    counters.cumulativeProducedImageBytes += actual;
    counters.peakStepImageBytes = std::max(counters.peakStepImageBytes, opPeakBytes);
    ++counters.producedImages;
    peakLiveBytes = std::max(peakLiveBytes, liveBytesAtDispatch + opPeakBytes);
    counters.peakLiveImageBytes = peakLiveBytes;

    auto shared = std::make_shared<const render::GpuImage>(std::move(produced));
    if (step.command != kInvalidGpuSceneCommand) {
        assignImage(step.command, shared);
    }
    consume(step.input);
    if (step.destination != kInvalidGpuSceneCommand) {
        consume(step.destination);
    }
    if (liveBytes > requestBudget) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::OverBudget,
                              "the live pinned image bytes exceeded the request budget");
    }
    if (step.cacheOnComplete && !step.cacheKey.empty()) {
        const auto inserted = cache->insert(step.cacheKey, shared);
        if (inserted == GpuSceneCacheInsertResult::Inserted) {
            ++counters.cacheInsertions;
        } else {
            ++counters.cacheRefusals;
        }
    }
    ++counters.commandsExecuted;
    ++cursor;
    return {};
}

GpuSceneExecutorDiagnostic GpuSceneExecutor::Impl::completeNative() {
    if (nativeKind == NativeKind::Solid) {
        return finishNative(solid->takeImage());
    }
    if (nativeKind == NativeKind::Upload) {
        return finishNative(upload->takeImage());
    }
    return finishNative(composite->takeImage());
}

GpuSceneExecutorPollResult GpuSceneExecutor::Impl::pollNative() {
    NativePoll status = NativePoll::Failure;
    if (nativeKind == NativeKind::Solid) {
        status = mapSolid(solid->poll());
    } else if (nativeKind == NativeKind::Upload) {
        status = mapUpload(upload->poll());
    } else {
        status = mapComposite(composite->poll());
    }
    if (status == NativePoll::WrongThread) {
        fail(GpuSceneExecutorDiagnosticCode::WrongThread,
             "a native operation rejected the calling thread", true);
        return GpuSceneExecutorPollResult::Failure;
    }
    if (status == NativePoll::Pending) {
        const auto deadline = std::chrono::milliseconds(budgets.jobDeadlineMilliseconds);
        if (!timeoutRequested && std::chrono::steady_clock::now() - nativeStart >= deadline) {
            timeoutRequested = true;
            ++counters.nativeJobTimeouts;
            if (nativeKind == NativeKind::Solid) {
                solid->cancel();
            } else if (nativeKind == NativeKind::Upload) {
                upload->cancel();
            } else {
                composite->cancel();
            }
            fail(GpuSceneExecutorDiagnosticCode::NativeTimeout,
                 "the native job exceeded its deadline; the submission is retained until it "
                 "retires",
                 true);
            return GpuSceneExecutorPollResult::Failure;
        }
        return GpuSceneExecutorPollResult::Pending;
    }
    if (status == NativePoll::Ready) {
        if (cancelRequested) {
            if (nativeKind == NativeKind::Solid) {
                static_cast<void>(solid->takeImage());
            } else if (nativeKind == NativeKind::Upload) {
                static_cast<void>(upload->takeImage());
            } else {
                static_cast<void>(composite->takeImage());
            }
            nativeKind = NativeKind::None;
            nativeInFlight = false;
            ++counters.nativeJobCancellations;
            fail(GpuSceneExecutorDiagnosticCode::Cancelled,
                 "the scene execution was cancelled; no image was published", false);
            return GpuSceneExecutorPollResult::Failure;
        }
        ++counters.nativeJobsCompleted;
        const auto completed = completeNative();
        if (completed.code != GpuSceneExecutorDiagnosticCode::None) {
            fail(completed.code, completed.message, false);
            return GpuSceneExecutorPollResult::Failure;
        }
        if (cursor >= steps.size()) {
            finishReady();
            return state == GpuSceneExecutorJobState::Ready ? GpuSceneExecutorPollResult::Ready
                                                            : GpuSceneExecutorPollResult::Failure;
        }
        return GpuSceneExecutorPollResult::Pending;
    }
    // Failure: only a PROVEN fence retirement may release resources. A device loss is proven (the
    // native sets its submission not-outstanding); otherwise the native accessor decides.
    const bool deviceLostNow =
        nativeKind == NativeKind::Solid    ? solidDeviceLost(solid->diagnostic())
        : nativeKind == NativeKind::Upload ? uploadDeviceLost(upload->diagnostic())
                                           : compositeDeviceLost(composite->diagnostic());
    if (deviceLostNow) {
        deviceLost = true;
        nativeInFlight = false;
        nativeKind = NativeKind::None;
        ++counters.nativeJobFailures;
        fail(GpuSceneExecutorDiagnosticCode::DeviceLost,
             "the device generation was lost while polling", false);
        return GpuSceneExecutorPollResult::Failure;
    }
    if (!hasUnretiredNative()) {
        const bool wasCancelled =
            nativeKind == NativeKind::Solid    ? solidCancelled(solid->diagnostic())
            : nativeKind == NativeKind::Upload ? uploadCancelled(upload->diagnostic())
                                               : compositeCancelled(composite->diagnostic());
        const bool statusRejected = nativeKind == NativeKind::Composite
                                        ? compositeStatusRejected(composite->diagnostic())
                                        : false;
        nativeInFlight = false;
        nativeKind = NativeKind::None;
        if (wasCancelled) {
            ++counters.nativeJobCancellations;
            fail(GpuSceneExecutorDiagnosticCode::Cancelled,
                 "the native job was cancelled after a proven retirement", false);
            return GpuSceneExecutorPollResult::Failure;
        }
        if (statusRejected) {
            ++counters.nativeJobFailures;
            fail(GpuSceneExecutorDiagnosticCode::DescriptorMismatch,
                 "the native status flag rejected the frame after a proven retirement", false);
            return GpuSceneExecutorPollResult::Failure;
        }
        ++counters.nativeJobFailures;
        fail(GpuSceneExecutorDiagnosticCode::DispatchRefused,
             "the native job failed after a proven retirement", false);
        return GpuSceneExecutorPollResult::Failure;
    }
    ++counters.nativeJobFailures;
    fail(GpuSceneExecutorDiagnosticCode::NativeUnproven,
         "a native failure did not prove fence retirement; owner drain is required", true);
    return GpuSceneExecutorPollResult::Failure;
}

GpuSceneExecutor::Impl::~Impl() {
    // Owner thread. Destroy the owned native pipelines FIRST, while the scene and the executor's
    // pins are still alive. Each pipeline's releaseImpl drains the in-flight submission within its
    // bounded budget or quarantines the ENTIRE native Impl; that native Impl strongly retains the
    // source/destination GpuImage pins it was handed and the allocator control, so it owns the
    // resources for as long as the submission may reference them. Only after the pipelines are gone
    // does ordinary member destruction release the executor's own metadata and pins. No separate
    // global registry, leak list, or allocation from this noexcept destructor is used.
    upload.reset();
    solid.reset();
    composite.reset();
}

} // namespace bloom::runtime
