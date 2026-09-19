#pragma once

// Private owner-thread lifecycle helpers for GpuPreviewDisplayService.
//
// These internal-linkage helpers were the top of gpu_preview_display_service.cpp. They are cohesive
// pieces of the service's own lifecycle (diagnostic construction, generation/owner identity, the
// CPU pipeline body, and the root-admission submission helper), not a generic framework. Only
// gpu_preview_display_service.cpp includes this header, so the split adds no shared dependency and
// no public symbol. The public client API and the observable service behavior are unchanged.

#include "gpu_preview_display_service_private.hpp"

#include <bloom/runtime/gpu_preview_display_product.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bloom::runtime {
namespace {

[[nodiscard]] inline TaskDiagnostic previewDiagnostic(std::string code, std::string summary,
                                                      std::string detail = {}) {
    return {.code = std::move(code),
            .severity = DiagnosticSeverity::Error,
            .summary = std::move(summary),
            .detail = std::move(detail),
            .suggestedAction = "Review the GPU preview display service diagnostics."};
}

[[nodiscard]] inline GpuServiceGeneration allocateGpuServiceGeneration() noexcept {
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

[[nodiscard]] inline TaskOwner startupOwner(const GpuServiceGeneration generation) {
    return {.kind = TaskOwnerKind::Application, .id = TaskOwnerId::fromRaw(generation.value())};
}

// The CPU half of a submission: compile -> evaluate -> select, then apply the display product to
// the same evaluated frame. This is the exact composition
// makeCompositionPreviewPipelineFromCpuStage performs; the GPU path never replaces it, only draws
// it on a CPU worker when the GPU half cannot run.
[[nodiscard]] inline TaskResult<PreviewPreparationResultHandle>
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

[[nodiscard]] inline TaskSubmission<PreviewPreparationResultHandle> rootAdmissionRejected() {
    TaskSubmission<PreviewPreparationResultHandle> submission;
    submission.status = TaskSubmissionStatus::QueueFull;
    submission.diagnostic = previewDiagnostic(
        "bloom.runtime.gpu-preview-admission-closed",
        "The GPU preview display service is shutting down or its root admission bound is reached.");
    return submission;
}

// Submits on an already-held root admission reservation and consumes that reservation exactly once.
[[nodiscard]] inline TaskSubmission<PreviewPreparationResultHandle>
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

} // namespace
} // namespace bloom::runtime
