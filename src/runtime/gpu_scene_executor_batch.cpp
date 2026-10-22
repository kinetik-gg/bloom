#include "gpu_scene_executor_private.hpp"

#include <algorithm>
#include <array>

namespace bloom::runtime {
using gpu_scene_executor_detail::makeDiagnostic;
std::optional<GpuSceneExecutorDiagnostic> GpuSceneExecutor::Impl::tryStartBatch() {
    if (!budgets.enableNativeBatches || batchAttempted)
        return std::nullopt;
    batchAttempted = true;
    if (steps.size() - cursor < 2)
        return std::nullopt;
    std::vector<render::GpuBatchOperation> operations;
    operations.reserve(render::kGpuOperationBatchCapacity);
    std::unordered_map<GpuSceneCommandIndex, std::uint32_t> produced;
    const auto ref =
        [this,
         &produced](const GpuSceneCommandIndex index) -> std::optional<render::GpuBatchImageRef> {
        if (const auto found = produced.find(index); found != produced.end())
            return render::GpuBatchImageRef{nullptr, found->second};
        if (index < images.size() && images[index])
            return render::GpuBatchImageRef{images[index], render::kGpuBatchExternalImage};
        return std::nullopt;
    };
    for (std::size_t index = cursor;
         index < steps.size() && operations.size() < render::kGpuOperationBatchCapacity; ++index) {
        const auto& step = steps[index];
        if (step.kind == GpuSceneExecutorStepKind::Solid) {
            if (!step.solidDataWindow || !step.solidDisplayWindow || step.checker)
                return std::nullopt;
            operations.emplace_back(
                render::GpuBatchSolid{{step.solidPixel, *step.solidDataWindow,
                                       *step.solidDisplayWindow, step.solidPixelAspect}});
        } else if (step.kind == GpuSceneExecutorStepKind::Translation) {
            const auto source = ref(step.input);
            if (!source || !step.outputWindow)
                return std::nullopt;
            operations.emplace_back(
                render::GpuBatchTranslation{*source, *step.outputWindow, step.translationX,
                                            step.translationY, step.translationOpacity});
        } else if (step.kind == GpuSceneExecutorStepKind::Affine) {
            const auto source = ref(step.input);
            if (!source || !step.outputWindow)
                return std::nullopt;
            operations.emplace_back(render::GpuBatchAffine{*source, *step.outputWindow,
                                                           step.affineMatrix, step.affineOpacity,
                                                           step.affineOutputDescriptor});
        } else if (step.kind == GpuSceneExecutorStepKind::SourceOver) {
            const auto source = ref(step.input);
            const auto destination =
                ref(step.destination == kInvalidGpuSceneCommand ? step.command : step.destination);
            if (!source || !destination)
                return std::nullopt;
            operations.emplace_back(render::GpuBatchSourceOver{*source, *destination});
        } else {
            break; // Ineligible operations retain their individual native GPU path.
        }
        if (step.command != kInvalidGpuSceneCommand)
            produced[step.command] = static_cast<std::uint32_t>(operations.size() - 1);
    }
    if (operations.size() < 2)
        return std::nullopt;
    if (!operationBatch)
        operationBatch = render::GpuOperationBatch::create(
            *device, {budgets.maxImageBytes, budgets.maxMetadataBytes});
    if (!operationBatch)
        return std::nullopt;
    if (budgets.collectQueueTimestamps && !queueTimingAttempted) {
        queueTimingAttempted = true;
        queueTimingSample.status =
            queueTiming ? queueTiming->begin() : render::GpuQueueTimingStatus::Failed;
        queueTimingStarted = queueTimingSample.status == render::GpuQueueTimingStatus::Active;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto native = operationBatch->begin(operations, requestBudget - liveBytes);
    const auto stats = operationBatch->statistics();
    if (stats.submissions != 0 && (native.code == render::GpuOperationBatchCode::None ||
                                   (native.code == render::GpuOperationBatchCode::Unproven &&
                                    operationBatch->hasUnretiredSubmission()))) {
        nativeKind = NativeKind::Batch;
        nativeInFlight = true;
        liveBytesAtDispatch = liveBytes;
        nativeStart = std::chrono::steady_clock::now();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(nativeStart - started).count();
        if (elapsed > 0)
            counters.processingSubmitNanoseconds += std::min(
                static_cast<std::uint64_t>(elapsed),
                std::numeric_limits<std::uint64_t>::max() - counters.processingSubmitNanoseconds);
        peakLiveBytes = std::max(peakLiveBytes, liveBytes + stats.peakAllocationBytes);
        counters.peakLiveImageBytes = peakLiveBytes;
        counters.peakStepImageBytes =
            std::max(counters.peakStepImageBytes, stats.peakAllocationBytes);
        batchStepCount = operations.size();
        ++counters.nativeBatches;
        counters.nativeSubmissions += stats.submissions;
        counters.dispatches += stats.dispatches;
        for (std::size_t index = cursor; index < cursor + batchStepCount; ++index) {
            const auto& step = steps[index];
            if (step.kind == GpuSceneExecutorStepKind::Solid)
                ++counters.solidDispatches;
            else if (step.kind == GpuSceneExecutorStepKind::Translation)
                ++counters.translationDispatches;
            else if (step.kind == GpuSceneExecutorStepKind::Affine)
                ++counters.affineDispatches;
            else if (step.kind == GpuSceneExecutorStepKind::SourceOver)
                ++counters.sourceOverDispatches;
        }
    }
    if (native.code != render::GpuOperationBatchCode::None) {
        if (native.code == render::GpuOperationBatchCode::DeviceLost) {
            deviceLost = true;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceLost, native.message);
        }
        if (operationBatch->hasUnretiredSubmission()) {
            nativeKind = NativeKind::Batch;
            nativeInFlight = true;
            return makeDiagnostic(GpuSceneExecutorDiagnosticCode::NativeUnproven, native.message);
        }
        ++counters.nativeBatchFallbacks;
        if (native.code == render::GpuOperationBatchCode::Busy)
            operationBatch.reset();
        return std::nullopt; // Only pre-submit construction/refusal falls back to serial native
                             // work.
    }
    return GpuSceneExecutorDiagnostic{};
}
GpuSceneExecutorPollResult GpuSceneExecutor::Impl::pollBatch() {
    const auto status = operationBatch->poll();
    if (status == render::GpuOperationBatchState::Pending) {
        if (std::chrono::steady_clock::now() - nativeStart >=
            std::chrono::milliseconds(budgets.jobDeadlineMilliseconds)) {
            operationBatch->cancel();
            ++counters.nativeJobTimeouts;
            fail(GpuSceneExecutorDiagnosticCode::NativeTimeout,
                 "native batch exceeded its deadline", true);
            return GpuSceneExecutorPollResult::Failure;
        }
        return GpuSceneExecutorPollResult::Pending;
    }
    if (status == render::GpuOperationBatchState::Ready && !cancelRequested) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - nativeStart)
                                 .count();
        if (elapsed > 0)
            counters.processingFenceObservationNanoseconds +=
                std::min(static_cast<std::uint64_t>(elapsed),
                         std::numeric_limits<std::uint64_t>::max() -
                             counters.processingFenceObservationNanoseconds);
        auto outputs = operationBatch->takeImages();
        if (outputs.size() != batchStepCount) {
            fail(GpuSceneExecutorDiagnosticCode::InternalInvariant,
                 "batch did not retain every output", false);
            return GpuSceneExecutorPollResult::Failure;
        }
        ++counters.nativeJobsCompleted;
        // Native poll validated every status word before returning Ready. Publication therefore
        // cannot expose an early output from a graph whose later kernel rejected its pixels.
        for (auto& output : outputs) {
            nativeKind = NativeKind::Batch;
            const auto completed = finishNative(std::move(output));
            if (completed.code != GpuSceneExecutorDiagnosticCode::None) {
                fail(completed.code, completed.message, false);
                return GpuSceneExecutorPollResult::Failure;
            }
        }
        if (cursor < steps.size())
            return GpuSceneExecutorPollResult::Pending;
        finishReady();
        return state == GpuSceneExecutorJobState::Ready ? GpuSceneExecutorPollResult::Ready
                                                        : GpuSceneExecutorPollResult::Failure;
    }
    const auto code = operationBatch->diagnostic().code;
    const bool unretired = operationBatch->hasUnretiredSubmission();
    if (!unretired) {
        nativeInFlight = false;
        nativeKind = NativeKind::None;
    }
    const bool cancelled = cancelRequested || code == render::GpuOperationBatchCode::Cancelled;
    if (cancelled)
        ++counters.nativeJobCancellations;
    else
        ++counters.nativeJobFailures;
    deviceLost = code == render::GpuOperationBatchCode::DeviceLost;
    fail(deviceLost  ? GpuSceneExecutorDiagnosticCode::DeviceLost
         : unretired ? GpuSceneExecutorDiagnosticCode::NativeUnproven
         : cancelled ? GpuSceneExecutorDiagnosticCode::Cancelled
         : code == render::GpuOperationBatchCode::StatusRejected
             ? GpuSceneExecutorDiagnosticCode::DescriptorMismatch
             : GpuSceneExecutorDiagnosticCode::DispatchRefused,
         operationBatch->diagnostic().message, unretired);
    return GpuSceneExecutorPollResult::Failure;
}
} // namespace bloom::runtime
