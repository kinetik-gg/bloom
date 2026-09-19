#include <bloom/ui/composition_preview_cpu_stage.hpp>

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
namespace {

using bloom::runtime::TaskDiagnostic;

void appendSubjectId(std::string& detail, const char* label, const auto& id) {
    if (!id.has_value()) {
        return;
    }
    if (!detail.empty()) {
        detail += ' ';
    }
    detail += label;
    detail += '=';
    detail += std::to_string(id->value());
}

std::string compileSubjectDetail(const bloom::runtime::CompileDiagnostic& diagnostic) {
    std::string detail = diagnostic.detail;
    appendSubjectId(detail, "node", diagnostic.subject.nodeId);
    appendSubjectId(detail, "edge", diagnostic.subject.edgeId);
    appendSubjectId(detail, "parameter", diagnostic.subject.parameterId);
    appendSubjectId(detail, "layer", diagnostic.subject.layerId);
    appendSubjectId(detail, "slot", diagnostic.subject.layerSlotId);
    if (!diagnostic.subject.field.empty()) {
        if (!detail.empty()) {
            detail += ' ';
        }
        detail += "field=";
        detail += diagnostic.subject.field;
    }
    return detail;
}

std::string evaluationSubjectDetail(const bloom::runtime::EvaluationDiagnostic& diagnostic) {
    std::string detail = diagnostic.detail;
    appendSubjectId(detail, "operation", diagnostic.subject.operation);
    appendSubjectId(detail, "node", diagnostic.subject.nodeId);
    appendSubjectId(detail, "layer", diagnostic.subject.layerId);
    if (!diagnostic.subject.field.empty()) {
        if (!detail.empty()) {
            detail += ' ';
        }
        detail += "field=";
        detail += diagnostic.subject.field;
    }
    return detail;
}

std::vector<TaskDiagnostic> taskDiagnostics(const bloom::runtime::SnapshotCompileResult& result) {
    std::vector<TaskDiagnostic> diagnostics;
    diagnostics.reserve(result.diagnostics.size());
    for (const auto& diagnostic : result.diagnostics) {
        diagnostics.push_back(
            {.code = std::string(bloom::runtime::compileDiagnosticCodeId(diagnostic.code)),
             .severity = diagnostic.severity,
             .summary = diagnostic.summary,
             .detail = compileSubjectDetail(diagnostic),
             .suggestedAction = "Inspect the referenced composition objects and node schemas."});
    }
    return diagnostics;
}

std::vector<TaskDiagnostic> taskDiagnostics(const bloom::runtime::EvaluationResult& result) {
    std::vector<TaskDiagnostic> diagnostics;
    diagnostics.reserve(result.diagnostics().size());
    for (const auto& diagnostic : result.diagnostics()) {
        diagnostics.push_back(
            {.code = std::string(bloom::runtime::evaluationDiagnosticCodeId(diagnostic.code)),
             .severity = diagnostic.severity,
             .summary = diagnostic.summary,
             .detail = evaluationSubjectDetail(diagnostic),
             .suggestedAction = "Review the affected operation and preview memory settings."});
    }
    return diagnostics;
}

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

TaskDiagnostic missingResultDiagnostic(std::string summary) {
    return {.code = "bloom.preview.pipeline.invalid-result",
            .severity = bloom::runtime::DiagnosticSeverity::Error,
            .summary = std::move(summary),
            .detail = {},
            .suggestedAction = "Report this internal error and retry the preview."};
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

[[nodiscard]] bool isNeutralIntent(const bloom::runtime::EvaluationColorIntent& intent) noexcept {
    return intent.workingColorSpaceId == bloom::runtime::kLinearRec709SceneColorSpaceId &&
           (intent.ocioConfigRevision == bloom::core::Sha256Digest{} ||
            intent.ocioConfigRevision == bloom::color::kBloomNeutralV1ConfigDigest);
}

[[nodiscard]] std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle>
buildSelectedDisplayProcessor(const bloom::runtime::EvaluationColorIntent& intent) noexcept {
    try {
        const auto expectedRevision = intent.ocioConfigRevision == bloom::core::Sha256Digest{}
                                          ? bloom::color::kBloomNeutralV1ConfigDigest
                                          : intent.ocioConfigRevision;
        const auto locator = isNeutralIntent(intent) ? bloom::color::kBloomNeutralV1ConfigUri
                                                     : bloom::color::kAcesCgV1ConfigUri;
        auto resolution =
            bloom::color::resolveOcioBuiltIn(bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                                             locator, expectedRevision, intent.workingColorSpaceId);
        if (!resolution.ready()) {
            return {};
        }
        auto resolved = std::move(resolution).takeResolved();
        if (!resolved.has_value()) {
            return {};
        }
        auto built = bloom::color::buildBloomNeutralCpuDisplayProcessor(*resolved);
        auto handle = std::move(built).takeHandle();
        if (!handle.has_value()) {
            return {};
        }
        return std::make_shared<const bloom::color::PreparedCpuDisplayProcessorHandle>(
            std::move(*handle));
    } catch (...) {
        return {};
    }
}

[[nodiscard]] std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle>
buildSelectedDisplayProcessor(const bloom::runtime::EvaluationColorIntent& intent,
                              const std::string_view displayName,
                              const std::string_view viewName) noexcept {
    if (displayName.empty() || viewName.empty()) {
        return buildSelectedDisplayProcessor(intent);
    }
    try {
        const auto expectedRevision = intent.ocioConfigRevision == bloom::core::Sha256Digest{}
                                          ? bloom::color::kBloomNeutralV1ConfigDigest
                                          : intent.ocioConfigRevision;
        const auto locator = isNeutralIntent(intent) ? bloom::color::kBloomNeutralV1ConfigUri
                                                     : bloom::color::kAcesCgV1ConfigUri;
        auto resolution =
            bloom::color::resolveOcioBuiltIn(bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                                             locator, expectedRevision, intent.workingColorSpaceId);
        if (!resolution.ready()) {
            return {};
        }
        auto resolved = std::move(resolution).takeResolved();
        if (!resolved.has_value()) {
            return {};
        }
        auto built =
            bloom::color::buildBloomNeutralCpuDisplayProcessor(*resolved, displayName, viewName);
        auto handle = std::move(built).takeHandle();
        if (!handle.has_value()) {
            return {};
        }
        return std::make_shared<const bloom::color::PreparedCpuDisplayProcessorHandle>(
            std::move(*handle));
    } catch (...) {
        return {};
    }
}

} // namespace

namespace bloom::ui {

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
        if (desiredIdentity.projectId != snapshot.project().id() ||
            snapshot.project().findComposition(desiredIdentity.compositionId) == nullptr ||
            desiredIdentity.sourceRevision != snapshot.revision() ||
            desiredIdentity.requestGeneration == 0 || pixelStorageByteLimit == 0) {
            return StageResult::failed(
                missingResultDiagnostic("The preview pipeline received mismatched request data"));
        }

        context.reportProgress({.phase = "Rendering composition preview",
                                .subphase = "Compiling the reachable composition graph",
                                .completed = 0,
                                .total = std::nullopt});
        auto compileResult = planCache->compile(compiler,
                                                {.snapshot = snapshot,
                                                 .compositionId = desiredIdentity.compositionId,
                                                 .parameterOverrides = interactionOverride},
                                                context.cancellation());
        auto diagnostics = taskDiagnostics(compileResult);

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

        const runtime::EvaluationRequest evaluationRequest{
            .time = desiredIdentity.time,
            .output = compileResult.plan->output(),
            .resolution = desiredIdentity.resolution,
            .quality = desiredIdentity.quality,
            .colorIntent = desiredIdentity.colorIntent,
            .pixelStorageByteLimit = pixelStorageByteLimit,
            .roi = desiredIdentity.roi,
            .bypassLookNodes = !desiredIdentity.showLook,
        };
        auto evaluationResult = evaluator.evaluate(
            compileResult.plan, evaluationRequest, context.cancellation(),
            [&context](const runtime::EvaluationProgress& progress) {
                reportEvaluationProgress(context, progress);
            },
            context.rowBandExecutor());
        auto evaluationDiagnostics = taskDiagnostics(evaluationResult);
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

        std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> selectedHandle;
        if (isNeutralIntent(desiredIdentity.colorIntent) && desiredIdentity.displayName.empty()) {
            selectedHandle = qualifiedSnapshot.handle;
        } else if (isNeutralIntent(desiredIdentity.colorIntent) &&
                   qualifiedSnapshot.readiness ==
                       runtime::QualifiedDisplayProcessorReadiness::Pending) {
            selectedHandle = nullptr;
        } else {
            selectedHandle = buildSelectedDisplayProcessor(
                desiredIdentity.colorIntent, desiredIdentity.displayName, desiredIdentity.viewName);
        }
        if (!isNeutralIntent(desiredIdentity.colorIntent) && selectedHandle == nullptr) {
            return StageResult::failed(missingResultDiagnostic(
                "The selected OCIO working space could not prepare a qualified display transform"));
        }

        auto stage = std::make_shared<const runtime::PreviewCpuStage>(
            desiredIdentity, evaluationResult.frame(), std::move(selectedHandle),
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
