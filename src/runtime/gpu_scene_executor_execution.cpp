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
using gpu_scene_executor_detail::diagnosticFromAffine;
using gpu_scene_executor_detail::diagnosticFromBlend;
using gpu_scene_executor_detail::diagnosticFromComposite;
using gpu_scene_executor_detail::diagnosticFromSolid;
using gpu_scene_executor_detail::diagnosticFromUpload;
using gpu_scene_executor_detail::makeDiagnostic;
using gpu_scene_executor_detail::mapAffine;
using gpu_scene_executor_detail::mapBlend;
using gpu_scene_executor_detail::mapComposite;
using gpu_scene_executor_detail::mapPointResample;
using gpu_scene_executor_detail::mapSolid;
using gpu_scene_executor_detail::mapUpload;
using gpu_scene_executor_detail::NativePoll;

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
    const bool affineUnretired = affine != nullptr && affine->hasUnretiredSubmission();
    const bool blendUnretired = blend != nullptr && blend->hasUnretiredSubmission();
    const bool pointResampleUnretired =
        pointResample != nullptr && pointResample->hasUnretiredSubmission();
    return solidUnretired || compositeUnretired || uploadUnretired || affineUnretired ||
           blendUnretired || pointResampleUnretired || ocioProgramsUnretired();
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
    } else if (nativeKind == NativeKind::Affine && affine != nullptr) {
        static_cast<void>(affine->poll());
    } else if (nativeKind == NativeKind::Blend && blend != nullptr) {
        static_cast<void>(blend->poll());
    } else if (nativeKind == NativeKind::PointResample && pointResample != nullptr) {
        static_cast<void>(pointResample->poll());
    } else if (nativeKind == NativeKind::Ocio) {
        drainOcioPrograms();
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
    case GpuSceneExecutorStepKind::Affine: {
        if (step.input == kInvalidGpuSceneCommand ||
            static_cast<std::size_t>(step.input) >= images.size() ||
            images[step.input] == nullptr || !step.outputWindow.has_value()) {
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                                  "an affine step is incomplete before dispatch");
        }
        const render::GpuAffineMatrixParameters parameters{images[step.input], *step.outputWindow,
                                                           step.affineMatrix, step.affineOpacity};
        const auto native = affine->beginAffineMatrix(parameters, remaining);
        if (native.code != render::GpuAffineDiagnosticCode::None) {
            return diagnosticFromAffine(native);
        }
        ++counters.affineDispatches;
        break;
    }
    case GpuSceneExecutorStepKind::Blend: {
        if (step.input == kInvalidGpuSceneCommand ||
            static_cast<std::size_t>(step.input) >= images.size() ||
            images[step.input] == nullptr || step.destination == kInvalidGpuSceneCommand ||
            static_cast<std::size_t>(step.destination) >= images.size() ||
            images[step.destination] == nullptr) {
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                                  "a blend step is incomplete before dispatch");
        }
        const render::GpuBlendParameters parameters{images[step.input], images[step.destination],
                                                    step.blendMode};
        const auto native = blend->beginBlend(parameters, remaining);
        if (native.code != render::GpuBlendDiagnosticCode::None) {
            return diagnosticFromBlend(native);
        }
        ++counters.blendDispatches;
        break;
    }
    case GpuSceneExecutorStepKind::OcioEffect: {
        const auto ocioDiagnostic = startOcioStep(step);
        if (ocioDiagnostic.code != GpuSceneExecutorDiagnosticCode::None) {
            return ocioDiagnostic;
        }
        break;
    }
    case GpuSceneExecutorStepKind::PointResample: {
        const auto resampleDiagnostic = startPointResampleStep(step);
        if (resampleDiagnostic.code != GpuSceneExecutorDiagnosticCode::None) {
            return resampleDiagnostic;
        }
        break;
    }
    }
    ++counters.dispatches;
    if (step.kind == GpuSceneExecutorStepKind::Solid ||
        step.kind == GpuSceneExecutorStepKind::CoveredSolid) {
        nativeKind = NativeKind::Solid;
    } else if (step.kind == GpuSceneExecutorStepKind::Upload) {
        nativeKind = NativeKind::Upload;
    } else if (step.kind == GpuSceneExecutorStepKind::Affine) {
        nativeKind = NativeKind::Affine;
    } else if (step.kind == GpuSceneExecutorStepKind::Blend) {
        nativeKind = NativeKind::Blend;
    } else if (step.kind == GpuSceneExecutorStepKind::PointResample) {
        nativeKind = NativeKind::PointResample;
    } else if (step.kind == GpuSceneExecutorStepKind::OcioEffect) {
        nativeKind = NativeKind::Ocio;
    } else {
        nativeKind = NativeKind::Composite;
    }
    nativeInFlight = true;
    cancelIssued = false;
    timeoutRequested = false;
    nativeStart = std::chrono::steady_clock::now();
    return {};
}

GpuSceneExecutor::Impl::~Impl() {
    // Owner thread. Destroy the owned native pipelines FIRST, while the scene and the executor's
    // pins are still alive. Each pipeline's releaseImpl drains the in-flight submission within its
    // bounded budget or quarantines the ENTIRE native Impl; that native Impl strongly retains the
    // source/destination GpuImage pins it was handed and the allocator control, so it owns the
    // resources for as long as the submission may reference them. Only after the pipelines are gone
    // does ordinary member destruction release the executor's own metadata and pins. No separate
    // global registry, leak list, or allocation from this noexcept destructor is used.
    ocioPrograms.clear();
    upload.reset();
    affine.reset();
    blend.reset();
    pointResample.reset();
    solid.reset();
    composite.reset();
}

} // namespace bloom::runtime
