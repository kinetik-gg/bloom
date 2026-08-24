#include <bloom/ui/composition_preview_pipeline.hpp>

#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <bloom/document/project.hpp>

#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

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

std::vector<bloom::runtime::TaskDiagnostic>
taskDiagnostics(const bloom::runtime::SnapshotCompileResult& result) {
    std::vector<bloom::runtime::TaskDiagnostic> diagnostics;
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

std::vector<bloom::runtime::TaskDiagnostic>
taskDiagnostics(const bloom::runtime::EvaluationResult& result) {
    std::vector<bloom::runtime::TaskDiagnostic> diagnostics;
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

bloom::runtime::TaskDiagnostic missingResultDiagnostic(std::string summary) {
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
    case bloom::runtime::EvaluationProgressStage::DisplayMapping:
        subphase = "Preparing reference display pixels";
        break;
    }
    context.reportProgress({.phase = "Rendering composition preview",
                            .subphase = std::move(subphase),
                            .completed = progress.completed,
                            .total = progress.total});
}

} // namespace

namespace bloom::ui {

PreviewPreparationFunction
makeCompositionPreviewPipeline(const runtime::SnapshotCompiler& compiler,
                               const runtime::CpuCompositionEvaluator& evaluator) {
    return [&compiler, &evaluator](const document::Snapshot& snapshot,
                                   const runtime::PreviewRequestIdentity& desiredIdentity,
                                   const std::size_t pixelStorageByteLimit,
                                   runtime::TaskContext& context) {
        using TaskResult = runtime::TaskResult<PreviewPreparationResultHandle>;

        if (context.isCancellationRequested()) {
            return TaskResult::cancelled();
        }
        if (desiredIdentity.projectId != snapshot.project().id() ||
            snapshot.project().findComposition(desiredIdentity.compositionId) == nullptr ||
            desiredIdentity.sourceRevision != snapshot.revision() ||
            desiredIdentity.requestGeneration == 0 || pixelStorageByteLimit == 0) {
            return TaskResult::failed(
                missingResultDiagnostic("The preview pipeline received mismatched request data"));
        }

        context.reportProgress({.phase = "Rendering composition preview",
                                .subphase = "Compiling the reachable composition graph",
                                .completed = 0,
                                .total = std::nullopt});
        auto compileResult =
            compiler.compile({.snapshot = snapshot, .compositionId = desiredIdentity.compositionId},
                             context.cancellation());
        auto diagnostics = taskDiagnostics(compileResult);

        switch (compileResult.status) {
        case runtime::SnapshotCompileStatus::Unsupported:
            return TaskResult::succeeded(std::make_shared<const runtime::PreviewPreparationResult>(
                                             runtime::PreviewPreparationResult::unsupported()),
                                         std::move(diagnostics));
        case runtime::SnapshotCompileStatus::Cancelled:
            return TaskResult::cancelled(std::move(diagnostics));
        case runtime::SnapshotCompileStatus::Failed:
            if (diagnostics.empty()) {
                diagnostics.push_back(
                    missingResultDiagnostic("Composition plan compilation failed"));
            }
            return TaskResult::failed(std::move(diagnostics));
        case runtime::SnapshotCompileStatus::Compiled:
            break;
        }

        if (compileResult.plan == nullptr) {
            diagnostics.push_back(
                missingResultDiagnostic("Composition compilation returned no immutable plan"));
            return TaskResult::failed(std::move(diagnostics));
        }

        const runtime::EvaluationRequest evaluationRequest{
            .time = desiredIdentity.time,
            .output = compileResult.plan->output,
            .resolution = desiredIdentity.resolution,
            .quality = desiredIdentity.quality,
            .colorIntent = desiredIdentity.colorIntent,
            .pixelStorageByteLimit = pixelStorageByteLimit,
        };
        auto evaluationResult =
            evaluator.evaluate(compileResult.plan, evaluationRequest, context.cancellation(),
                               [&context](const runtime::EvaluationProgress& progress) {
                                   reportEvaluationProgress(context, progress);
                               });
        auto evaluationDiagnostics = taskDiagnostics(evaluationResult);
        diagnostics.insert(diagnostics.end(),
                           std::make_move_iterator(evaluationDiagnostics.begin()),
                           std::make_move_iterator(evaluationDiagnostics.end()));

        switch (evaluationResult.status()) {
        case runtime::EvaluationStatus::Cancelled:
            return TaskResult::cancelled(std::move(diagnostics));
        case runtime::EvaluationStatus::Failed:
            if (diagnostics.empty()) {
                diagnostics.push_back(missingResultDiagnostic("Composition evaluation failed"));
            }
            return TaskResult::failed(std::move(diagnostics));
        case runtime::EvaluationStatus::Evaluated:
            break;
        }

        if (evaluationResult.frame() == nullptr) {
            diagnostics.push_back(
                missingResultDiagnostic("Composition evaluation returned no frame"));
            return TaskResult::failed(std::move(diagnostics));
        }
        auto prepared = runtime::PreparedPreviewFrame::create(desiredIdentity.requestGeneration,
                                                              evaluationResult.frame());
        if (!prepared.has_value() || prepared->desiredIdentity() != desiredIdentity) {
            diagnostics.push_back(
                missingResultDiagnostic("The prepared frame identity did not match its request"));
            return TaskResult::failed(std::move(diagnostics));
        }

        auto frame = std::make_shared<const runtime::PreparedPreviewFrame>(std::move(*prepared));
        auto preparedResult = runtime::PreviewPreparationResult::prepared(std::move(frame));
        if (!preparedResult.has_value()) {
            diagnostics.push_back(
                missingResultDiagnostic("The preview result rejected its prepared frame"));
            return TaskResult::failed(std::move(diagnostics));
        }
        auto result =
            std::make_shared<const runtime::PreviewPreparationResult>(std::move(*preparedResult));
        return TaskResult::succeeded(std::move(result), std::move(diagnostics));
    };
}

} // namespace bloom::ui
