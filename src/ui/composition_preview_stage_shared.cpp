#include "composition_preview_stage_shared.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>

#include <bloom/document/project.hpp>

#include <string>
#include <utility>

namespace bloom::ui::detail {
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

std::vector<TaskDiagnostic>
compileTaskDiagnostics(const bloom::runtime::SnapshotCompileResult& result) {
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

std::vector<TaskDiagnostic>
evaluationTaskDiagnostics(const bloom::runtime::EvaluationResult& result) {
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

TaskDiagnostic missingResultDiagnostic(std::string summary) {
    return {.code = "bloom.preview.pipeline.invalid-result",
            .severity = bloom::runtime::DiagnosticSeverity::Error,
            .summary = std::move(summary),
            .detail = {},
            .suggestedAction = "Report this internal error and retry the preview."};
}

bool isNeutralIntent(const bloom::runtime::EvaluationColorIntent& intent) noexcept {
    return intent.workingColorSpaceId == bloom::runtime::kLinearRec709SceneColorSpaceId &&
           (intent.ocioConfigRevision == bloom::core::Sha256Digest{} ||
            intent.ocioConfigRevision == bloom::color::kBloomNeutralV1ConfigDigest);
}

std::optional<TaskDiagnostic>
validateStageRequest(const document::Snapshot& snapshot,
                     const runtime::PreviewRequestIdentity& desiredIdentity,
                     const std::size_t pixelStorageByteLimit) {
    if (desiredIdentity.projectId != snapshot.project().id() ||
        snapshot.project().findComposition(desiredIdentity.compositionId) == nullptr ||
        desiredIdentity.sourceRevision != snapshot.revision() ||
        desiredIdentity.requestGeneration == 0 || pixelStorageByteLimit == 0) {
        return missingResultDiagnostic("The preview pipeline received mismatched request data");
    }
    return std::nullopt;
}

runtime::SnapshotCompileResult
compilePreviewPlan(const runtime::SnapshotCompiler& compiler,
                   const std::shared_ptr<runtime::CompiledPlanCache>& planCache,
                   const document::Snapshot& snapshot,
                   const runtime::PreviewRequestIdentity& desiredIdentity,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides,
                   const runtime::CancellationToken& cancellation) {
    return planCache->compile(compiler,
                              {.snapshot = snapshot,
                               .compositionId = desiredIdentity.compositionId,
                               .parameterOverrides = overrides},
                              cancellation);
}

runtime::EvaluationRequest
evaluationRequestFor(const runtime::PreviewRequestIdentity& desiredIdentity,
                     const runtime::CompiledCompositionPlan& plan,
                     const std::size_t pixelStorageByteLimit) noexcept {
    return runtime::EvaluationRequest{
        .time = desiredIdentity.time,
        .output = plan.output(),
        .resolution = desiredIdentity.resolution,
        .quality = desiredIdentity.quality,
        .colorIntent = desiredIdentity.colorIntent,
        .pixelStorageByteLimit = pixelStorageByteLimit,
        .roi = desiredIdentity.roi,
        .bypassLookNodes = !desiredIdentity.showLook,
    };
}

SelectedDisplayProcessor
selectDisplayProcessor(const runtime::EvaluationColorIntent& intent,
                       const runtime::QualifiedDisplayProcessorSnapshot& qualifiedSnapshot,
                       const std::string_view displayName,
                       const std::string_view viewName) noexcept {
    SelectedDisplayProcessor selected;
    if (isNeutralIntent(intent) && displayName.empty()) {
        selected.handle = qualifiedSnapshot.handle;
        return selected;
    }
    if (isNeutralIntent(intent) &&
        qualifiedSnapshot.readiness == runtime::QualifiedDisplayProcessorReadiness::Pending) {
        selected.handle = nullptr;
        return selected;
    }
    selected.handle = buildSelectedDisplayProcessor(intent, displayName, viewName);
    if (!isNeutralIntent(intent) && selected.handle == nullptr) {
        selected.failed = true;
    }
    return selected;
}

} // namespace bloom::ui::detail
