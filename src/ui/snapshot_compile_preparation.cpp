#include <bloom/ui/snapshot_compile_preparation.hpp>

#include <bloom/runtime/snapshot_compiler.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string compileSubjectDetail(const bloom::runtime::CompileDiagnostic& diagnostic) {
    std::string detail = diagnostic.detail;
    const auto appendId = [&detail](const char* label, const auto& id) {
        if (!id.has_value()) {
            return;
        }
        if (!detail.empty()) {
            detail += ' ';
        }
        detail += label;
        detail += '=';
        detail += std::to_string(id->value());
    };
    appendId("node", diagnostic.subject.nodeId);
    appendId("edge", diagnostic.subject.edgeId);
    appendId("parameter", diagnostic.subject.parameterId);
    appendId("layer", diagnostic.subject.layerId);
    appendId("slot", diagnostic.subject.layerSlotId);
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

} // namespace

namespace bloom::ui {

PreviewPreparationFunction
makeSnapshotCompilePreparation(const runtime::SnapshotCompiler& compiler) {
    return [&compiler](const document::Snapshot& snapshot,
                       const document::CompositionId compositionId, runtime::TaskContext& context) {
        using Result = runtime::TaskResult<SnapshotCompileResultHandle>;

        context.reportProgress({.phase = "Compiling composition",
                                .subphase = "Validating the reachable node graph",
                                .completed = 0,
                                .total = 2});
        auto result = compiler.compile({snapshot, compositionId}, context.cancellation());
        auto diagnostics = taskDiagnostics(result);

        switch (result.status) {
        case runtime::SnapshotCompileStatus::Compiled:
            context.reportProgress({.phase = "Compiling composition",
                                    .subphase = "Immutable plan ready",
                                    .completed = 2,
                                    .total = 2});
            return Result::succeeded(
                std::make_shared<const runtime::SnapshotCompileResult>(std::move(result)),
                std::move(diagnostics));
        case runtime::SnapshotCompileStatus::Unsupported:
            context.reportProgress({.phase = "Compiling composition",
                                    .subphase = "Unsupported graph diagnosed",
                                    .completed = 2,
                                    .total = 2});
            return Result::succeeded(
                std::make_shared<const runtime::SnapshotCompileResult>(std::move(result)),
                std::move(diagnostics));
        case runtime::SnapshotCompileStatus::Cancelled:
            return Result::cancelled(std::move(diagnostics));
        case runtime::SnapshotCompileStatus::Failed:
            if (diagnostics.empty()) {
                diagnostics.push_back(
                    {.code = "bloom.runtime.compile.failed",
                     .severity = runtime::DiagnosticSeverity::Error,
                     .summary = "Composition plan compilation failed",
                     .detail = "The compiler did not produce a more specific diagnostic.",
                     .suggestedAction = "Inspect the composition graph and retry the operation."});
            }
            return Result::failed(std::move(diagnostics));
        }

        return Result::failed(
            {.code = "bloom.runtime.compile.invalid-status",
             .severity = runtime::DiagnosticSeverity::Error,
             .summary = "Composition plan compilation returned an invalid status",
             .detail = {},
             .suggestedAction = "Report this internal error and retry the operation."});
    };
}

} // namespace bloom::ui
