#include <bloom/ui/composition_preview_pipeline.hpp>

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

std::vector<bloom::runtime::TaskDiagnostic>
taskDiagnostics(const bloom::runtime::ReferenceDisplayPreparationResult& result) {
    std::vector<bloom::runtime::TaskDiagnostic> diagnostics;
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

std::vector<bloom::runtime::TaskDiagnostic>
taskDiagnostics(const bloom::runtime::QualifiedDisplayPreparationResult& result) {
    std::vector<bloom::runtime::TaskDiagnostic> diagnostics;
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

PreviewPreparationFunction makeCompositionPreviewPipeline(
    const runtime::SnapshotCompiler& compiler, const runtime::CpuCompositionEvaluator& evaluator,
    const runtime::CpuReferenceDisplayPreparer& displayPreparer,
    const runtime::QualifiedDisplayProcessorProvider& qualifiedProcessorProvider,
    CompiledPlanCacheHandle planCache) {
    if (planCache == nullptr) {
        planCache = std::make_shared<CompiledPlanCache>();
    }
    return [&compiler, &evaluator, &displayPreparer, &qualifiedProcessorProvider,
            planCache = std::move(planCache)](
               const document::Snapshot& snapshot,
               const runtime::PreviewRequestIdentity& desiredIdentity,
               const std::size_t pixelStorageByteLimit,
               const std::vector<runtime::SnapshotParameterOverride>& interactionOverride,
               runtime::TaskContext& context) {
        using TaskResult = runtime::TaskResult<PreviewPreparationResultHandle>;

        if (context.isCancellationRequested()) {
            return TaskResult::cancelled();
        }
        // One single locked read of the provider up front (see QualifiedDisplayProcessorSnapshot's
        // documentation): readiness/handle/failureDiagnostic are used together below, and reading
        // them as separate calls could straddle the provider's one-shot publish() and observe an
        // inconsistent pair (Pending readiness alongside a Failed diagnostic, for example) --
        // reading once and reusing the same snapshot for both the early fail-closed check and the
        // later reference-vs-qualified branch makes this decision race-free.
        const auto qualifiedSnapshot = qualifiedProcessorProvider.snapshot();
        // Design decision 4 (fail-closed): once qualification is known to have failed, no preview
        // request is prepared at all -- CompositionPreviewController's existing Failed handling
        // retains its last-good frame and surfaces this diagnostic, and Bloom never auto-
        // substitutes the reference transform for a qualified request. This is checked before any
        // compilation/evaluation work so a permanently failed qualification does not keep spending
        // worker time on frames nothing will ever qualify to display.
        if (qualifiedSnapshot.readiness == runtime::QualifiedDisplayProcessorReadiness::Failed &&
            isNeutralIntent(desiredIdentity.colorIntent)) {
            return TaskResult::failed(qualifiedSnapshot.failureDiagnostic);
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
        // docs/architecture/animation-and-time.md, "Direct Manipulation And Preview Overrides":
        // SnapshotCompileRequest carries zero or one typed parameter override; admission
        // (revision/target/reachability/schema/kind/domain, then source kind) is entirely
        // SnapshotCompiler's -- this only threads the override the controller already sourced from
        // the session's active interaction.
        auto compileResult = planCache->compile(compiler,
                                                {.snapshot = snapshot,
                                                 .compositionId = desiredIdentity.compositionId,
                                                 .parameterOverrides = interactionOverride},
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
            .output = compileResult.plan->output(),
            .resolution = desiredIdentity.resolution,
            .quality = desiredIdentity.quality,
            .colorIntent = desiredIdentity.colorIntent,
            .pixelStorageByteLimit = pixelStorageByteLimit,
            .bypassOperationCache = !interactionOverride.empty(),
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

        // Design decision 5 (default flow after readiness): every request routes through the
        // qualified preparer once qualifiedProcessorProvider reports Ready; the honest startup
        // window (design decision 3) routes through the unchanged reference path otherwise -- the
        // permanently-Failed case already returned above, before evaluation even ran.
        std::optional<runtime::PreparedPreviewFrame> prepared;
        std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> selectedHandle;
        if (isNeutralIntent(desiredIdentity.colorIntent) && desiredIdentity.displayName.empty()) {
            selectedHandle = qualifiedSnapshot.handle;
        } else if (isNeutralIntent(desiredIdentity.colorIntent) &&
                   qualifiedSnapshot.readiness ==
                       runtime::QualifiedDisplayProcessorReadiness::Pending) {
            // Preserve the documented startup reference window even when the Viewer has already
            // chosen the config's default display/view. A non-default choice is built once the
            // qualified provider is ready; it never changes export evaluation.
            selectedHandle = nullptr;
        } else {
            selectedHandle = buildSelectedDisplayProcessor(
                desiredIdentity.colorIntent, desiredIdentity.displayName, desiredIdentity.viewName);
        }
        if (!isNeutralIntent(desiredIdentity.colorIntent) && selectedHandle == nullptr) {
            return TaskResult::failed(missingResultDiagnostic(
                "The selected OCIO working space could not prepare a qualified display transform"));
        }
        if (selectedHandle != nullptr) {
            const runtime::CpuQualifiedDisplayPreparer qualifiedPreparer(*selectedHandle);
            const runtime::QualifiedDisplayPreparationRequest qualifiedRequest{
                .aggregatePixelStorageByteLimit = pixelStorageByteLimit,
                .viewAdjust = desiredIdentity.viewAdjust,
                .displayName = desiredIdentity.displayName,
                .viewName = desiredIdentity.viewName,
                .showLook = desiredIdentity.showLook,
            };
            auto qualifiedResult = qualifiedPreparer.prepare(
                evaluationResult.frame(), qualifiedRequest, context.cancellation(),
                [&context](const runtime::QualifiedDisplayProgress& progress) {
                    reportQualifiedDisplayProgress(context, progress);
                });
            auto qualifiedDiagnostics = taskDiagnostics(qualifiedResult);
            diagnostics.insert(diagnostics.end(),
                               std::make_move_iterator(qualifiedDiagnostics.begin()),
                               std::make_move_iterator(qualifiedDiagnostics.end()));

            switch (qualifiedResult.status()) {
            case runtime::QualifiedDisplayPreparationStatus::Cancelled:
                return TaskResult::cancelled(std::move(diagnostics));
            case runtime::QualifiedDisplayPreparationStatus::Failed:
                if (diagnostics.empty()) {
                    diagnostics.push_back(
                        missingResultDiagnostic("Qualified display preparation failed"));
                }
                return TaskResult::failed(std::move(diagnostics));
            case runtime::QualifiedDisplayPreparationStatus::Prepared:
                break;
            }
            if (qualifiedResult.frame() == nullptr) {
                diagnostics.push_back(
                    missingResultDiagnostic("Display preparation returned no immutable frame"));
                return TaskResult::failed(std::move(diagnostics));
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
                evaluationResult.frame(), displayRequest, context.cancellation(),
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
                return TaskResult::cancelled(std::move(diagnostics));
            case runtime::ReferenceDisplayPreparationStatus::Failed:
                if (diagnostics.empty()) {
                    diagnostics.push_back(
                        missingResultDiagnostic("Reference display preparation failed"));
                }
                return TaskResult::failed(std::move(diagnostics));
            case runtime::ReferenceDisplayPreparationStatus::Prepared:
                break;
            }
            if (displayResult.frame() == nullptr) {
                diagnostics.push_back(
                    missingResultDiagnostic("Display preparation returned no immutable frame"));
                return TaskResult::failed(std::move(diagnostics));
            }
            prepared = runtime::PreparedPreviewFrame::create(desiredIdentity.requestGeneration,
                                                             displayResult.frame(),
                                                             desiredIdentity.resolutionPolicy);
        }
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
