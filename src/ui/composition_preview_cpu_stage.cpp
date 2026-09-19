#include <bloom/ui/composition_preview_cpu_stage.hpp>

#include "composition_preview_stage_shared.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/qualified_display_preparation.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <bloom/document/project.hpp>

#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The CPU half of the composition preview pipeline. The private helpers and the two halves below
// are the single implementation; makeCompositionPreviewPipeline() in
// composition_preview_pipeline.cpp is a thin composition of the factories exported here.
//
// The compile/validate/request-build/processor-selection half both this stage and the GPU-scene
// stage share lives in composition_preview_stage_shared.cpp; the display-specific progress and
// diagnostic mapping stays here, next to the fallback that uses it.
namespace {

using bloom::runtime::TaskDiagnostic;

std::vector<TaskDiagnostic>
taskDiagnostics(const bloom::runtime::ReferenceDisplayPreparationResult& result) {
    std::vector<TaskDiagnostic> diagnostics;
    diagnostics.reserve(result.diagnostics().size());
    for (const auto& diagnostic : result.diagnostics()) {
        diagnostics.push_back(
            {.code = std::string(bloom::runtime::referenceDisplayDiagnosticCodeId(diagnostic.code)),
             .severity = diagnostic.severity,
             .summary = diagnostic.summary,
             .detail = diagnostic.detail,
             .suggestedAction = "Review the preview display intent and memory settings."});
    }
    return diagnostics;
}

std::vector<TaskDiagnostic>
taskDiagnostics(const bloom::runtime::QualifiedDisplayPreparationResult& result) {
    std::vector<TaskDiagnostic> diagnostics;
    diagnostics.reserve(result.diagnostics().size());
    for (const auto& diagnostic : result.diagnostics()) {
        diagnostics.push_back(
            {.code = std::string(bloom::runtime::qualifiedDisplayDiagnosticCodeId(diagnostic.code)),
             .severity = diagnostic.severity,
             .summary = diagnostic.summary,
             .detail = diagnostic.detail,
             .suggestedAction = "Review the preview display intent and memory settings."});
    }
    return diagnostics;
}

void reportEvaluationProgress(bloom::runtime::TaskContext& context,
                              const bloom::runtime::EvaluationProgress& progress) {
    std::string subphase;
    switch (progress.stage) {
    case bloom::runtime::EvaluationProgressStage::Preflight:
        subphase = "Preparing bounded image storage";
        break;
    case bloom::runtime::EvaluationProgressStage::Operation:
        subphase = progress.operation.has_value()
                       ? "Evaluating operation " + std::to_string(progress.operation->value())
                       : "Evaluating composition operations";
        break;
    }
    context.reportProgress({.phase = "Rendering composition preview",
                            .subphase = std::move(subphase),
                            .completed = progress.completed,
                            .total = progress.total});
}

void reportDisplayProgress(bloom::runtime::TaskContext& context,
                           const bloom::runtime::ReferenceDisplayProgress& progress) {
    const std::string subphase =
        progress.stage == bloom::runtime::ReferenceDisplayProgressStage::Preflight
            ? "Validating the bounded reference display handoff"
            : "Preparing reference display pixels";
    context.reportProgress({.phase = "Preparing composition preview display",
                            .subphase = subphase,
                            .completed = progress.completed,
                            .total = progress.total});
}

void reportQualifiedDisplayProgress(bloom::runtime::TaskContext& context,
                                    const bloom::runtime::QualifiedDisplayProgress& progress) {
    const std::string subphase =
        progress.stage == bloom::runtime::QualifiedDisplayProgressStage::Preflight
            ? "Validating the bounded qualified display handoff"
            : "Applying the qualified Bloom Neutral display transform";
    context.reportProgress({.phase = "Preparing composition preview display",
                            .subphase = subphase,
                            .completed = progress.completed,
                            .total = progress.total});
}

} // namespace

namespace bloom::ui {

using detail::compilePreviewPlan;
using detail::compileTaskDiagnostics;
using detail::evaluationRequestFor;
using detail::evaluationTaskDiagnostics;
using detail::isNeutralIntent;
using detail::missingResultDiagnostic;
using detail::selectDisplayProcessor;
using detail::validateStageRequest;

runtime::PreviewCpuStageFunction makeCompositionPreviewCpuStage(
    const runtime::SnapshotCompiler& compiler, const runtime::CpuCompositionEvaluator& evaluator,
    const runtime::QualifiedDisplayProcessorProvider& qualifiedProcessorProvider,
    CompiledPlanCacheHandle planCache) {
    if (planCache == nullptr) {
        planCache = std::make_shared<CompiledPlanCache>();
    }
    return [&compiler, &evaluator, &qualifiedProcessorProvider, planCache = std::move(planCache)](
               const document::Snapshot& snapshot,
               const runtime::PreviewRequestIdentity& desiredIdentity,
               const std::size_t pixelStorageByteLimit,
               const std::vector<runtime::SnapshotParameterOverride>& interactionOverride,
               runtime::TaskContext& context) {
        using StageResult = runtime::TaskResult<runtime::PreviewCpuStageOutcomeHandle>;

        if (context.isCancellationRequested()) {
            return StageResult::cancelled();
        }
        const auto qualifiedSnapshot = qualifiedProcessorProvider.snapshot();
        if (qualifiedSnapshot.readiness == runtime::QualifiedDisplayProcessorReadiness::Failed &&
            isNeutralIntent(desiredIdentity.colorIntent)) {
            return StageResult::failed(qualifiedSnapshot.failureDiagnostic);
        }
        if (const auto invalid =
                validateStageRequest(snapshot, desiredIdentity, pixelStorageByteLimit);
            invalid.has_value()) {
            return StageResult::failed(*invalid);
        }

        context.reportProgress({.phase = "Rendering composition preview",
                                .subphase = "Compiling the reachable composition graph",
                                .completed = 0,
                                .total = std::nullopt});
        auto compileResult = compilePreviewPlan(compiler, planCache, snapshot, desiredIdentity,
                                                interactionOverride, context.cancellation());
        auto diagnostics = compileTaskDiagnostics(compileResult);

        switch (compileResult.status) {
        case runtime::SnapshotCompileStatus::Unsupported:
            return StageResult::succeeded(std::make_shared<const runtime::PreviewCpuStageOutcome>(
                runtime::PreviewCpuStageOutcome{.status =
                                                    runtime::PreviewCpuStageStatus::Unsupported,
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

        const auto evaluationRequest =
            evaluationRequestFor(desiredIdentity, *compileResult.plan, pixelStorageByteLimit);
        auto evaluationResult = evaluator.evaluate(
            compileResult.plan, evaluationRequest, context.cancellation(),
            [&context](const runtime::EvaluationProgress& progress) {
                reportEvaluationProgress(context, progress);
            },
            context.rowBandExecutor());
        auto evaluationDiagnostics = evaluationTaskDiagnostics(evaluationResult);
        diagnostics.insert(diagnostics.end(),
                           std::make_move_iterator(evaluationDiagnostics.begin()),
                           std::make_move_iterator(evaluationDiagnostics.end()));

        switch (evaluationResult.status()) {
        case runtime::EvaluationStatus::Cancelled:
            return StageResult::cancelled(std::move(diagnostics));
        case runtime::EvaluationStatus::Failed:
            if (diagnostics.empty()) {
                diagnostics.push_back(missingResultDiagnostic("Composition evaluation failed"));
            }
            return StageResult::failed(std::move(diagnostics));
        case runtime::EvaluationStatus::Evaluated:
            break;
        }

        if (evaluationResult.frame() == nullptr) {
            diagnostics.push_back(
                missingResultDiagnostic("Composition evaluation returned no frame"));
            return StageResult::failed(std::move(diagnostics));
        }

        auto selection =
            selectDisplayProcessor(desiredIdentity.colorIntent, qualifiedSnapshot,
                                   desiredIdentity.displayName, desiredIdentity.viewName);
        if (selection.failed) {
            return StageResult::failed(missingResultDiagnostic(
                "The selected OCIO working space could not prepare a qualified display transform"));
        }

        auto stage = std::make_shared<const runtime::PreviewCpuStage>(
            desiredIdentity, evaluationResult.frame(), std::move(selection.handle),
            pixelStorageByteLimit, std::move(diagnostics));
        return StageResult::succeeded(std::make_shared<const runtime::PreviewCpuStageOutcome>(
            runtime::PreviewCpuStageOutcome{.status = runtime::PreviewCpuStageStatus::Evaluated,
                                            .stage = std::move(stage),
                                            .diagnostics = {}}));
    };
}

runtime::PreviewCpuDisplayFallback makeCompositionPreviewCpuDisplayFallback(
    const runtime::CpuReferenceDisplayPreparer& displayPreparer) {
    return [&displayPreparer](const runtime::PreviewCpuStage& stage,
                              runtime::TaskContext& context) {
        using Result = runtime::TaskResult<PreviewPreparationResultHandle>;

        const auto& desiredIdentity = stage.desiredIdentity();
        const std::size_t pixelStorageByteLimit = stage.pixelStorageByteLimit();
        auto diagnostics = stage.diagnostics();
        std::optional<runtime::PreparedPreviewFrame> prepared;
        if (stage.ocioQualified()) {
            const runtime::CpuQualifiedDisplayPreparer qualifiedPreparer(*stage.displayProcessor());
            const runtime::QualifiedDisplayPreparationRequest qualifiedRequest{
                .aggregatePixelStorageByteLimit = pixelStorageByteLimit,
                .viewAdjust = desiredIdentity.viewAdjust,
                .displayName = desiredIdentity.displayName,
                .viewName = desiredIdentity.viewName,
                .showLook = desiredIdentity.showLook,
            };
            auto qualifiedResult = qualifiedPreparer.prepare(
                stage.processFrame(), qualifiedRequest, context.cancellation(),
                [&context](const runtime::QualifiedDisplayProgress& progress) {
                    reportQualifiedDisplayProgress(context, progress);
                });
            auto qualifiedDiagnostics = taskDiagnostics(qualifiedResult);
            diagnostics.insert(diagnostics.end(),
                               std::make_move_iterator(qualifiedDiagnostics.begin()),
                               std::make_move_iterator(qualifiedDiagnostics.end()));

            switch (qualifiedResult.status()) {
            case runtime::QualifiedDisplayPreparationStatus::Cancelled:
                return Result::cancelled(std::move(diagnostics));
            case runtime::QualifiedDisplayPreparationStatus::Failed:
                if (diagnostics.empty()) {
                    diagnostics.push_back(
                        missingResultDiagnostic("Qualified display preparation failed"));
                }
                return Result::failed(std::move(diagnostics));
            case runtime::QualifiedDisplayPreparationStatus::Prepared:
                break;
            }
            if (qualifiedResult.frame() == nullptr) {
                diagnostics.push_back(
                    missingResultDiagnostic("Display preparation returned no immutable frame"));
                return Result::failed(std::move(diagnostics));
            }
            prepared = runtime::PreparedPreviewFrame::createQualified(
                desiredIdentity.requestGeneration, qualifiedResult.frame(),
                desiredIdentity.resolutionPolicy);
        } else {
            const runtime::ReferenceDisplayPreparationRequest displayRequest{
                .intent = runtime::ReferenceDisplayIntent::LinearRec709SceneToSrgb,
                .aggregatePixelStorageByteLimit = pixelStorageByteLimit,
                .viewAdjust = desiredIdentity.viewAdjust,
                .displayName = desiredIdentity.displayName,
                .viewName = desiredIdentity.viewName,
                .showLook = desiredIdentity.showLook,
            };
            auto displayResult = displayPreparer.prepare(
                stage.processFrame(), displayRequest, context.cancellation(),
                [&context](const runtime::ReferenceDisplayProgress& progress) {
                    reportDisplayProgress(context, progress);
                },
                context.rowBandExecutor());
            auto displayDiagnostics = taskDiagnostics(displayResult);
            diagnostics.insert(diagnostics.end(),
                               std::make_move_iterator(displayDiagnostics.begin()),
                               std::make_move_iterator(displayDiagnostics.end()));

            switch (displayResult.status()) {
            case runtime::ReferenceDisplayPreparationStatus::Cancelled:
                return Result::cancelled(std::move(diagnostics));
            case runtime::ReferenceDisplayPreparationStatus::Failed:
                if (diagnostics.empty()) {
                    diagnostics.push_back(
                        missingResultDiagnostic("Reference display preparation failed"));
                }
                return Result::failed(std::move(diagnostics));
            case runtime::ReferenceDisplayPreparationStatus::Prepared:
                break;
            }
            if (displayResult.frame() == nullptr) {
                diagnostics.push_back(
                    missingResultDiagnostic("Display preparation returned no immutable frame"));
                return Result::failed(std::move(diagnostics));
            }
            prepared = runtime::PreparedPreviewFrame::create(desiredIdentity.requestGeneration,
                                                             displayResult.frame(),
                                                             desiredIdentity.resolutionPolicy);
        }
        if (!prepared.has_value() || prepared->desiredIdentity() != desiredIdentity) {
            diagnostics.push_back(
                missingResultDiagnostic("The prepared frame identity did not match its request"));
            return Result::failed(std::move(diagnostics));
        }

        auto frame = std::make_shared<const runtime::PreparedPreviewFrame>(std::move(*prepared));
        auto preparedResult = runtime::PreviewPreparationResult::prepared(std::move(frame));
        if (!preparedResult.has_value()) {
            diagnostics.push_back(
                missingResultDiagnostic("The preview result rejected its prepared frame"));
            return Result::failed(std::move(diagnostics));
        }
        auto result =
            std::make_shared<const runtime::PreviewPreparationResult>(std::move(*preparedResult));
        return Result::succeeded(std::move(result), std::move(diagnostics));
    };
}

PreviewPreparationFunction
makeCompositionPreviewPipelineFromCpuStage(runtime::PreviewCpuStageFunction stage,
                                           runtime::PreviewCpuDisplayFallback fallback) {
    return
        [stage = std::move(stage), fallback = std::move(fallback)](
            const document::Snapshot& snapshot,
            const runtime::PreviewRequestIdentity& desiredIdentity,
            const std::size_t pixelStorageByteLimit,
            const std::vector<runtime::SnapshotParameterOverride>& interactionOverride,
            runtime::TaskContext& context) -> runtime::TaskResult<PreviewPreparationResultHandle> {
            using Result = runtime::TaskResult<PreviewPreparationResultHandle>;

            auto stageResult = stage(snapshot, desiredIdentity, pixelStorageByteLimit,
                                     interactionOverride, context);
            if (stageResult.state() == runtime::TaskState::Cancelled) {
                return Result::cancelled(stageResult.diagnostics());
            }
            if (stageResult.state() == runtime::TaskState::Failed) {
                return Result::failed(stageResult.diagnostics());
            }
            const auto& outcome = stageResult.value();
            if (!outcome.has_value() || *outcome == nullptr) {
                return Result::failed(missingResultDiagnostic("The CPU stage returned no outcome"));
            }
            if ((*outcome)->status == runtime::PreviewCpuStageStatus::Unsupported) {
                auto unsupported = std::make_shared<const runtime::PreviewPreparationResult>(
                    runtime::PreviewPreparationResult::unsupported());
                return Result::succeeded(std::move(unsupported), (*outcome)->diagnostics);
            }
            if ((*outcome)->stage == nullptr) {
                return Result::failed(missingResultDiagnostic(
                    "The CPU stage produced an evaluated outcome with no frame"));
            }
            return fallback(*(*outcome)->stage, context);
        };
}

} // namespace bloom::ui
