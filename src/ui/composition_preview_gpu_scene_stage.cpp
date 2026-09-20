#include <bloom/ui/composition_preview_gpu_scene_stage.hpp>

#include "composition_preview_stage_shared.hpp"

#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using bloom::runtime::TaskDiagnostic;

std::string gpuSceneDiagnosticCodeId(const bloom::runtime::PreparedGpuSceneDiagnosticCode code) {
    using bloom::runtime::PreparedGpuSceneDiagnosticCode;
    switch (code) {
    case PreparedGpuSceneDiagnosticCode::None:
        return "bloom.preview.gpu-scene.none";
    case PreparedGpuSceneDiagnosticCode::InvalidRequest:
        return "bloom.preview.gpu-scene.invalid-request";
    case PreparedGpuSceneDiagnosticCode::InvalidPlan:
        return "bloom.preview.gpu-scene.invalid-plan";
    case PreparedGpuSceneDiagnosticCode::UnsupportedOperation:
        return "bloom.preview.gpu-scene.unsupported-operation";
    case PreparedGpuSceneDiagnosticCode::UnsupportedTransform:
        return "bloom.preview.gpu-scene.unsupported-transform";
    case PreparedGpuSceneDiagnosticCode::UnsupportedBlend:
        return "bloom.preview.gpu-scene.unsupported-blend";
    case PreparedGpuSceneDiagnosticCode::UnsupportedRequest:
        return "bloom.preview.gpu-scene.unsupported-request";
    case PreparedGpuSceneDiagnosticCode::MediaUnavailable:
        return "bloom.preview.gpu-scene.media-unavailable";
    case PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded:
        return "bloom.preview.gpu-scene.pixel-budget-exceeded";
    case PreparedGpuSceneDiagnosticCode::AllocationFailure:
        return "bloom.preview.gpu-scene.allocation-failure";
    case PreparedGpuSceneDiagnosticCode::Cancelled:
        return "bloom.preview.gpu-scene.cancelled";
    case PreparedGpuSceneDiagnosticCode::PreflightFailure:
        return "bloom.preview.gpu-scene.preflight-failure";
    case PreparedGpuSceneDiagnosticCode::InternalInvariant:
        return "bloom.preview.gpu-scene.internal-invariant";
    }
    // A builder diagnostic added by a later (media) slice, or any future code, is treated as a
    // GPU-subset refusal by gpuScenePreparationIsFailure() below rather than silently as a hard
    // failure, so the caller still takes the full CPU path.
    return "bloom.preview.gpu-scene.unrecognized";
}

// The hard-error codes are exactly the ones that mean "the scene could not be produced for a
// reason that is not a subset refusal". Every other code -- an unsupported operation/transform/
// blend/request, a pixel-storage budget refusal, and any future media-unavailable code -- is a
// GPU-subset refusal whose correct handling is the full original CPU fallback, never a fabricated
// empty scene. A budget refusal in particular is pressure, not corruption: the GPU scene's retained
// host set can exceed its request allowance while the CPU reference peak still fits, so the viewer
// must recover through the CPU path instead of failing closed.
bool gpuScenePreparationIsFailure(
    const bloom::runtime::PreparedGpuSceneDiagnosticCode code) noexcept {
    using bloom::runtime::PreparedGpuSceneDiagnosticCode;
    switch (code) {
    case PreparedGpuSceneDiagnosticCode::InvalidPlan:
    case PreparedGpuSceneDiagnosticCode::AllocationFailure:
    case PreparedGpuSceneDiagnosticCode::PreflightFailure:
    case PreparedGpuSceneDiagnosticCode::InternalInvariant:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] TaskDiagnostic
gpuSceneSubsetDiagnostic(const bloom::runtime::PreparedGpuSceneDiagnostic& diagnostic) {
    return {.code = gpuSceneDiagnosticCodeId(diagnostic.code),
            .severity = bloom::runtime::DiagnosticSeverity::Warning,
            .summary = diagnostic.message.empty()
                           ? std::string{"The composition is outside the prepared GPU subset"}
                           : diagnostic.message,
            .detail = {},
            .suggestedAction = "Take the full CPU composition preview path for this request."};
}

[[nodiscard]] TaskDiagnostic
gpuSceneFailureDiagnostic(const bloom::runtime::PreparedGpuSceneDiagnostic& diagnostic) {
    return {.code = gpuSceneDiagnosticCodeId(diagnostic.code),
            .severity = bloom::runtime::DiagnosticSeverity::Error,
            .summary = diagnostic.message.empty() ? std::string{"GPU scene preparation failed"}
                                                  : diagnostic.message,
            .detail = {},
            .suggestedAction = "Review the request and preview memory settings."};
}

} // namespace

namespace bloom::ui {

using detail::compilePreviewPlan;
using detail::compileTaskDiagnostics;
using detail::evaluationRequestFor;
using detail::missingResultDiagnostic;
using detail::selectDisplayProcessor;
using detail::validateStageRequest;

runtime::PreviewGpuSceneStageFunction makeCompositionPreviewGpuSceneStage(
    const runtime::SnapshotCompiler& compiler, const runtime::CpuGpuSceneBuilder& builder,
    const runtime::QualifiedDisplayProcessorProvider& qualifiedProcessorProvider,
    CompiledPlanCacheHandle planCache) {
    if (planCache == nullptr) {
        planCache = std::make_shared<CompiledPlanCache>();
    }
    return [&compiler, &builder, &qualifiedProcessorProvider, planCache = std::move(planCache)](
               const document::Snapshot& snapshot,
               const runtime::PreviewRequestIdentity& desiredIdentity,
               const std::size_t pixelStorageByteLimit,
               const std::vector<runtime::SnapshotParameterOverride>& interactionOverride,
               runtime::TaskContext& context) {
        using StageResult = runtime::TaskResult<runtime::PreviewGpuSceneStageOutcomeHandle>;

        if (context.isCancellationRequested()) {
            return StageResult::cancelled();
        }
        const auto qualifiedSnapshot = qualifiedProcessorProvider.snapshot();
        if (qualifiedSnapshot.readiness == runtime::QualifiedDisplayProcessorReadiness::Failed &&
            detail::isNeutralIntent(desiredIdentity.colorIntent)) {
            return StageResult::failed(qualifiedSnapshot.failureDiagnostic);
        }
        if (const auto invalid =
                validateStageRequest(snapshot, desiredIdentity, pixelStorageByteLimit);
            invalid.has_value()) {
            return StageResult::failed(*invalid);
        }

        context.reportProgress({.phase = "Preparing GPU preview scene",
                                .subphase = "Compiling the reachable composition graph",
                                .completed = 0,
                                .total = std::nullopt});
        auto compileResult = compilePreviewPlan(compiler, planCache, snapshot, desiredIdentity,
                                                interactionOverride, context.cancellation());
        auto diagnostics = compileTaskDiagnostics(compileResult);

        switch (compileResult.status) {
        case runtime::SnapshotCompileStatus::Unsupported:
            return StageResult::succeeded(
                std::make_shared<const runtime::PreviewGpuSceneStageOutcome>(
                    runtime::PreviewGpuSceneStageOutcome{
                        .status = runtime::PreviewGpuSceneStageStatus::Unsupported,
                        .stage = {},
                        .diagnostics = std::move(diagnostics)}));
        case runtime::SnapshotCompileStatus::Cancelled:
            return StageResult::cancelled(std::move(diagnostics));
        case runtime::SnapshotCompileStatus::Failed:
            if (diagnostics.empty()) {
                diagnostics.push_back(
                    missingResultDiagnostic("Composition plan compilation failed"));
            }
            return StageResult::failed(std::move(diagnostics));
        case runtime::SnapshotCompileStatus::Compiled:
            break;
        }

        if (compileResult.plan == nullptr) {
            diagnostics.push_back(
                missingResultDiagnostic("Composition compilation returned no immutable plan"));
            return StageResult::failed(std::move(diagnostics));
        }

        // The unsupported-request screen runs inside the builder before any pixel or media work, so
        // a request the builder will not prepare falls back before anything heavy is touched.
        const auto evaluationRequest =
            evaluationRequestFor(desiredIdentity, *compileResult.plan, pixelStorageByteLimit);
        context.reportProgress({.phase = "Preparing GPU preview scene",
                                .subphase = "Preparing the immutable GPU scene",
                                .completed = 0,
                                .total = std::nullopt});
        auto buildResult =
            builder.build(compileResult.plan, evaluationRequest, context.cancellation());

        if (buildResult.scene == nullptr) {
            if (buildResult.diagnostic.code == runtime::PreparedGpuSceneDiagnosticCode::Cancelled) {
                return StageResult::cancelled(std::move(diagnostics));
            }
            if (gpuScenePreparationIsFailure(buildResult.diagnostic.code)) {
                diagnostics.push_back(gpuSceneFailureDiagnostic(buildResult.diagnostic));
                return StageResult::failed(std::move(diagnostics));
            }
            // A GPU-subset refusal: the caller takes the full original CPU path. Never an empty
            // frame and never a partial scene.
            diagnostics.push_back(gpuSceneSubsetDiagnostic(buildResult.diagnostic));
            return StageResult::succeeded(
                std::make_shared<const runtime::PreviewGpuSceneStageOutcome>(
                    runtime::PreviewGpuSceneStageOutcome{
                        .status = runtime::PreviewGpuSceneStageStatus::UnsupportedGpuSubset,
                        .stage = {},
                        .diagnostics = std::move(diagnostics)}));
        }

        auto selection =
            selectDisplayProcessor(desiredIdentity.colorIntent, qualifiedSnapshot,
                                   desiredIdentity.displayName, desiredIdentity.viewName);
        if (selection.failed) {
            return StageResult::failed(missingResultDiagnostic(
                "The selected OCIO working space could not prepare a qualified display transform"));
        }

        auto stage = std::make_shared<const runtime::PreviewGpuSceneStage>(
            desiredIdentity, std::move(buildResult.scene), std::move(selection.handle),
            pixelStorageByteLimit, std::move(diagnostics));
        return StageResult::succeeded(std::make_shared<const runtime::PreviewGpuSceneStageOutcome>(
            runtime::PreviewGpuSceneStageOutcome{.status =
                                                     runtime::PreviewGpuSceneStageStatus::Prepared,
                                                 .stage = std::move(stage),
                                                 .diagnostics = {}}));
    };
}

} // namespace bloom::ui
