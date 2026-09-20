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
using gpu_scene_executor_detail::mapOcio;
using gpu_scene_executor_detail::mapPointResample;
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
[[nodiscard]] bool affineDeviceLost(const render::GpuAffineDiagnostic& diagnostic) noexcept {
    return diagnostic.code == render::GpuAffineDiagnosticCode::DeviceLost;
}
[[nodiscard]] bool affineCancelled(const render::GpuAffineDiagnostic& diagnostic) noexcept {
    return diagnostic.code == render::GpuAffineDiagnosticCode::Cancelled;
}
[[nodiscard]] bool affineStatusRejected(const render::GpuAffineDiagnostic& diagnostic) noexcept {
    return diagnostic.code == render::GpuAffineDiagnosticCode::StatusFlagRejected;
}
[[nodiscard]] bool blendDeviceLost(const render::GpuBlendDiagnostic& diagnostic) noexcept {
    return diagnostic.code == render::GpuBlendDiagnosticCode::DeviceLost;
}
[[nodiscard]] bool blendCancelled(const render::GpuBlendDiagnostic& diagnostic) noexcept {
    return diagnostic.code == render::GpuBlendDiagnosticCode::Cancelled;
}
[[nodiscard]] bool blendStatusRejected(const render::GpuBlendDiagnostic& diagnostic) noexcept {
    return diagnostic.code == render::GpuBlendDiagnosticCode::StatusFlagRejected;
}

} // namespace

GpuSceneExecutorDiagnostic GpuSceneExecutor::Impl::finishNative(render::GpuImage produced) {
    // The op's own actual retained bytes (image plus its native metadata). The true during-op live
    // peak is the pins at dispatch plus this, so the request budget bounds the temporary peak too.
    const std::uint64_t opPeakBytes =
        nativeKind == NativeKind::Solid           ? solid->lastJobAllocationBytes()
        : nativeKind == NativeKind::Upload        ? upload->lastJobAllocationBytes()
        : nativeKind == NativeKind::Affine        ? affine->lastJobAllocationBytes()
        : nativeKind == NativeKind::Blend         ? blend->lastJobAllocationBytes()
        : nativeKind == NativeKind::PointResample ? pointResample->lastJobAllocationBytes()
        : nativeKind == NativeKind::Ocio          ? nativeOcioLastJobBytes
                                                  : composite->lastJobAllocationBytes();
    nativeKind = NativeKind::None;
    nativeInFlight = false;
    if (!produced.isValid()) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                              "a native operation produced no image");
    }
    const GpuSceneExecutorStep& step = steps[cursor];
    const render::GpuImage* inputImage = nullptr;
    if ((step.kind == GpuSceneExecutorStepKind::Translation ||
         step.kind == GpuSceneExecutorStepKind::Affine ||
         step.kind == GpuSceneExecutorStepKind::PointResample) &&
        step.input != kInvalidGpuSceneCommand &&
        static_cast<std::size_t>(step.input) < images.size()) {
        inputImage = images[step.input].get();
    }
    const render::GpuImage* backdropImage = nullptr;
    if (step.kind == GpuSceneExecutorStepKind::Blend &&
        step.destination != kInvalidGpuSceneCommand &&
        static_cast<std::size_t>(step.destination) < images.size()) {
        backdropImage = images[step.destination].get();
    }
    if (step.command != kInvalidGpuSceneCommand &&
        !descriptorMatches(scene->commands()[step.command], produced, inputImage, backdropImage)) {
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
    if (nativeKind == NativeKind::Affine) {
        return finishNative(affine->takeImage());
    }
    if (nativeKind == NativeKind::Blend) {
        return finishNative(blend->takeImage());
    }
    if (nativeKind == NativeKind::PointResample) {
        auto output = takePointResampleOutput();
        if (!output.has_value()) {
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                                  "a point-resample operation produced no image");
        }
        return finishNative(std::move(*output));
    }
    if (nativeKind == NativeKind::Ocio) {
        auto output = takeOcioOutput();
        if (!output.has_value()) {
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                                  "an OCIO effect operation produced no image");
        }
        return finishNative(std::move(*output));
    }
    return finishNative(composite->takeImage());
}

GpuSceneExecutorPollResult GpuSceneExecutor::Impl::pollNative() {
    NativePoll status = NativePoll::Failure;
    if (nativeKind == NativeKind::Solid) {
        status = mapSolid(solid->poll());
    } else if (nativeKind == NativeKind::Upload) {
        status = mapUpload(upload->poll());
    } else if (nativeKind == NativeKind::Affine) {
        status = mapAffine(affine->poll());
    } else if (nativeKind == NativeKind::Blend) {
        status = mapBlend(blend->poll());
    } else if (nativeKind == NativeKind::PointResample) {
        status = mapPointResample(pointResample->poll());
    } else if (nativeKind == NativeKind::Ocio) {
        status =
            nativeOcioProgram != nullptr ? mapOcio(nativeOcioProgram->poll()) : NativePoll::Failure;
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
            } else if (nativeKind == NativeKind::Affine) {
                affine->cancel();
            } else if (nativeKind == NativeKind::Blend) {
                blend->cancel();
            } else if (nativeKind == NativeKind::PointResample) {
                cancelPointResample();
            } else if (nativeKind == NativeKind::Ocio) {
                if (nativeOcioProgram != nullptr) {
                    nativeOcioProgram->cancel();
                }
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
            } else if (nativeKind == NativeKind::Affine) {
                static_cast<void>(affine->takeImage());
            } else if (nativeKind == NativeKind::Blend) {
                static_cast<void>(blend->takeImage());
            } else if (nativeKind == NativeKind::PointResample) {
                discardPointResampleOutput();
            } else if (nativeKind == NativeKind::Ocio) {
                if (nativeOcioProgram != nullptr) {
                    static_cast<void>(nativeOcioProgram->takeEffectOutput());
                }
            } else {
                static_cast<void>(composite->takeImage());
            }
            nativeKind = NativeKind::None;
            nativeOcioProgram = nullptr;
            nativeOcioLastJobBytes = 0;
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
    if (nativeKind == NativeKind::Ocio) {
        const auto ocioCode = nativeOcioProgram != nullptr
                                  ? nativeOcioProgram->diagnostic().code
                                  : render::GpuOcioProgramDiagnosticCode::DeviceUnavailable;
        if (ocioCode == render::GpuOcioProgramDiagnosticCode::DeviceLost) {
            deviceLost = true;
            nativeInFlight = false;
            nativeKind = NativeKind::None;
            nativeOcioProgram = nullptr;
            ++counters.nativeJobFailures;
            fail(GpuSceneExecutorDiagnosticCode::DeviceLost,
                 "the device generation was lost while polling", false);
            return GpuSceneExecutorPollResult::Failure;
        }
        if (!hasUnretiredNative()) {
            nativeInFlight = false;
            nativeKind = NativeKind::None;
            nativeOcioProgram = nullptr;
            if (ocioCode == render::GpuOcioProgramDiagnosticCode::Cancelled) {
                ++counters.nativeJobCancellations;
                fail(GpuSceneExecutorDiagnosticCode::Cancelled,
                     "the OCIO effect job was cancelled after a proven retirement", false);
                return GpuSceneExecutorPollResult::Failure;
            }
            ++counters.nativeJobFailures;
            fail(GpuSceneExecutorDiagnosticCode::DispatchRefused,
                 "the OCIO effect job failed after a proven retirement", false);
            return GpuSceneExecutorPollResult::Failure;
        }
        ++counters.nativeJobFailures;
        fail(GpuSceneExecutorDiagnosticCode::NativeUnproven,
             "an OCIO failure did not prove fence retirement; owner drain is required", true);
        return GpuSceneExecutorPollResult::Failure;
    }
    const bool deviceLostNow =
        nativeKind == NativeKind::Solid           ? solidDeviceLost(solid->diagnostic())
        : nativeKind == NativeKind::Upload        ? uploadDeviceLost(upload->diagnostic())
        : nativeKind == NativeKind::Affine        ? affineDeviceLost(affine->diagnostic())
        : nativeKind == NativeKind::Blend         ? blendDeviceLost(blend->diagnostic())
        : nativeKind == NativeKind::PointResample ? pointResampleDeviceLost()
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
            nativeKind == NativeKind::Solid           ? solidCancelled(solid->diagnostic())
            : nativeKind == NativeKind::Upload        ? uploadCancelled(upload->diagnostic())
            : nativeKind == NativeKind::Affine        ? affineCancelled(affine->diagnostic())
            : nativeKind == NativeKind::Blend         ? blendCancelled(blend->diagnostic())
            : nativeKind == NativeKind::PointResample ? pointResampleCancelled()
                                                      : compositeCancelled(composite->diagnostic());
        const bool statusRejected =
            nativeKind == NativeKind::Composite ? compositeStatusRejected(composite->diagnostic())
            : nativeKind == NativeKind::Affine  ? affineStatusRejected(affine->diagnostic())
            : nativeKind == NativeKind::Blend   ? blendStatusRejected(blend->diagnostic())
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

} // namespace bloom::runtime
