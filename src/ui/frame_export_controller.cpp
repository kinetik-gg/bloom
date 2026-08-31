#include <bloom/ui/frame_export_controller.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <bloom/document/project.hpp>
#include <bloom/render/image.hpp>

#include <QDir>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>

#include <system_error>
#include <utility>

namespace bloom::ui {

namespace {

// Design decision 4: "an app-private temp location — find/establish the app's existing convention
// for transient files; justify the choice." No such convention existed anywhere in src/host,
// src/platform, or src/ui before this task (verified: grep found nothing), so this establishes one:
// QStandardPaths::TempLocation is Qt's own portable per-user temp root (XDG runtime/cache-adjacent
// /tmp on Linux, NSTemporaryDirectory() on macOS, %TEMP% on Windows -- exactly the kind of
// platform-appropriate transient location AGENTS.md's "treat Linux/macOS/Windows as first class"
// rule calls for); "Bloom/export-scratch" underneath it is a directory this application owns,
// distinct from any artist-chosen destination directory and never tracked by
// platform::StagedArtifactCoordinator, matching frame_export_publication.hpp's `scratchDirectory`
// contract ("a caller-owned directory ... never the real destination's parent, never tracked by
// StagedArtifactCoordinator").
[[nodiscard]] std::filesystem::path defaultExportScratchDirectory() {
    auto base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (base.isEmpty()) {
        base = QDir::tempPath();
    }
    return std::filesystem::path(base.toStdString()) / "Bloom" / "export-scratch";
}

[[nodiscard]] QString
firstDiagnosticSummary(const std::vector<runtime::TaskDiagnostic>& diagnostics, QString fallback) {
    if (diagnostics.empty() || diagnostics.front().summary.empty()) {
        return fallback;
    }
    return QString::fromStdString(diagnostics.front().summary);
}

[[nodiscard]] QString facetDisplayName(const output::OutputFacetIdV1 facet) {
    switch (facet) {
    case output::OutputFacetIdV1::Pixels:
        return FrameExportController::tr("Pixels");
    case output::OutputFacetIdV1::Precision:
        return FrameExportController::tr("Precision");
    case output::OutputFacetIdV1::Color:
        return FrameExportController::tr("Color");
    case output::OutputFacetIdV1::AlphaAssociation:
        return FrameExportController::tr("Alpha Association");
    case output::OutputFacetIdV1::Channels:
        return FrameExportController::tr("Channels");
    case output::OutputFacetIdV1::DataWindow:
        return FrameExportController::tr("Data Window");
    case output::OutputFacetIdV1::DisplayWindow:
        return FrameExportController::tr("Display Window");
    case output::OutputFacetIdV1::PixelAspect:
        return FrameExportController::tr("Pixel Aspect");
    case output::OutputFacetIdV1::Compression:
        return FrameExportController::tr("Compression");
    case output::OutputFacetIdV1::Metadata:
        return FrameExportController::tr("Metadata");
    case output::OutputFacetIdV1::ExternalDependencies:
        return FrameExportController::tr("External Dependencies");
    }
    return FrameExportController::tr("Unknown facet");
}

[[nodiscard]] FrameExportFacetSummary
summarizeFacets(const output::OutputAnalysisReportV1View& view) {
    FrameExportFacetSummary summary;
    for (const auto& facet : view.facets) {
        if (facet.state == output::OutputPreservationStateV1::Exact) {
            ++summary.exactFacetCount;
        } else {
            ++summary.nonExactFacetCount;
            summary.nonExactFacetNames << facetDisplayName(facet.facet);
        }
    }
    return summary;
}

[[nodiscard]] QString stageName(const host::OutputAnalysisAttemptStageV1 stage) {
    switch (stage) {
    case host::OutputAnalysisAttemptStageV1::Resolving:
        return FrameExportController::tr("resolving the destination");
    case host::OutputAnalysisAttemptStageV1::Evaluating:
        return FrameExportController::tr("evaluating the composition");
    case host::OutputAnalysisAttemptStageV1::Identifying:
        return FrameExportController::tr("computing the frame identity");
    case host::OutputAnalysisAttemptStageV1::Analyzing:
        return FrameExportController::tr("analyzing preservation");
    }
    return FrameExportController::tr("preparing the export");
}

[[nodiscard]] QString jobStageName(const host::FrameExportPublicationStageV1 stage) {
    switch (stage) {
    case host::FrameExportPublicationStageV1::Preflight:
        return FrameExportController::tr("preflight");
    case host::FrameExportPublicationStageV1::Staging:
        return FrameExportController::tr("staging");
    case host::FrameExportPublicationStageV1::Writing:
        return FrameExportController::tr("writing");
    case host::FrameExportPublicationStageV1::Verifying:
        return FrameExportController::tr("verifying");
    case host::FrameExportPublicationStageV1::ArtifactCopy:
        return FrameExportController::tr("copying the artifact");
    case host::FrameExportPublicationStageV1::Guard:
        return FrameExportController::tr("the final publish guard");
    case host::FrameExportPublicationStageV1::Cancelled:
        return FrameExportController::tr("cancellation");
    }
    return FrameExportController::tr("publication");
}

// docs/architecture/frame-output.md, "Non-Blocking Execution": the closed stage vocabulary
// Resolving/Evaluating/Identifying/ColorPreparing/Analyzing/PreparingOutput/Writing/Verifying/
// Publishing. This maps the approved job's own bloom::output::OutputExportProgressCallbackV1 into
// the existing bloom::runtime::TaskContext::reportProgress() plumbing (design decision 4: "smallest
// honest wiring, no new progress framework") -- the pre-approval attempt's Resolving/Evaluating/
// Identifying/Analyzing stages already report through the identical mechanism inside
// beginOutputAnalysisAttemptV1() itself (src/host/output_analysis_attempt_runner.cpp), so nothing
// extra is needed for that half.
[[nodiscard]] std::string outputExportStagePhase(const output::OutputExportStageV1 stage) {
    switch (stage) {
    case output::OutputExportStageV1::Resolving:
        return "Resolving";
    case output::OutputExportStageV1::Evaluating:
        return "Evaluating";
    case output::OutputExportStageV1::Identifying:
        return "Identifying";
    case output::OutputExportStageV1::ColorPreparing:
        return "ColorPreparing";
    case output::OutputExportStageV1::Analyzing:
        return "Analyzing";
    case output::OutputExportStageV1::PreparingOutput:
        return "PreparingOutput";
    case output::OutputExportStageV1::Writing:
        return "Writing";
    case output::OutputExportStageV1::Verifying:
        return "Verifying";
    case output::OutputExportStageV1::Publishing:
        return "Publishing";
    }
    return "Publishing";
}

[[nodiscard]] FrameExportOutcome
exportFailureOutcome(const host::FrameExportPublicationFailureV1* failure) {
    if (failure != nullptr && failure->stage() == host::FrameExportPublicationStageV1::Cancelled) {
        return FrameExportOutcome::Cancelled;
    }
    return FrameExportOutcome::Failed;
}

} // namespace

FrameExportController::FrameExportController(
    CompositionSession& session, runtime::TaskScheduler& scheduler, TaskUiBridge& taskUiBridge,
    const runtime::SnapshotCompiler& compiler, host::PublicationCoordinator& publicationCoordinator,
    platform::StagedArtifactCoordinator& artifactCoordinator,
    std::filesystem::path scratchDirectory, QObject* parent)
    : QObject(parent), session_(session), scheduler_(scheduler), taskUiBridge_(taskUiBridge),
      compiler_(compiler), publicationCoordinator_(publicationCoordinator),
      artifactCoordinator_(artifactCoordinator),
      scratchDirectory_(scratchDirectory.empty() ? defaultExportScratchDirectory()
                                                 : std::move(scratchDirectory)) {
    std::error_code error;
    std::filesystem::create_directories(scratchDirectory_, error);

    connect(&taskUiBridge_, &TaskUiBridge::snapshotsPolled, this, &FrameExportController::pollOnce);

    destinationProvider_ = []() -> std::optional<std::filesystem::path> {
        const auto chosen =
            QFileDialog::getSaveFileName(nullptr, tr("Export Frame"), {}, tr("OpenEXR (*.exr)"));
        if (chosen.isEmpty()) {
            return std::nullopt;
        }
        return std::filesystem::path(chosen.toStdString());
    };

    approvalDecisionProvider_ = [](const FrameExportApprovalPrompt& prompt) {
        QMessageBox box;
        box.setWindowTitle(tr("Export Frame"));
        box.setText(tr("Export this frame to %1?")
                        .arg(QString::fromStdString(prompt.destination.filename().string())));
        QStringList lines;
        lines << tr("Destination: %1").arg(QString::fromStdString(prompt.destination.string()));
        lines << tr("Resolution: %1 x %2").arg(prompt.width).arg(prompt.height);
        lines << tr("Preset: %1").arg(prompt.presetName);
        lines << tr("Preserved exactly: %1 of %2 facets")
                     .arg(prompt.facets.exactFacetCount)
                     .arg(prompt.facets.exactFacetCount + prompt.facets.nonExactFacetCount);
        if (!prompt.facets.nonExactFacetNames.isEmpty()) {
            lines << tr("Not exact: %1").arg(prompt.facets.nonExactFacetNames.join(", "));
        }
        lines << tr("Digest: %1…").arg(prompt.digestShortForm);
        box.setInformativeText(lines.join("\n"));
        box.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
        box.button(QMessageBox::Yes)->setText(tr("Export"));
        box.setDefaultButton(QMessageBox::Yes);
        return box.exec() == QMessageBox::Yes ? FrameExportApprovalDecision::Export
                                              : FrameExportApprovalDecision::Cancel;
    };
}

FrameExportController::~FrameExportController() {
    if (auto* compiling = std::get_if<CompileHandle>(&inFlight_)) {
        compiling->handle.cancel();
    } else if (auto* runner = std::get_if<host::OutputAnalysisAttemptRunnerV1>(&inFlight_)) {
        runner->requestCancellation();
    } else if (auto* job = std::get_if<ExportJobHandle>(&inFlight_)) {
        job->handle.cancel();
    }
}

bool FrameExportController::canExport() const noexcept {
    return activity_ == FrameExportActivity::Idle && session_.composition() != nullptr;
}

bool FrameExportController::isBusy() const noexcept {
    return activity_ != FrameExportActivity::Idle;
}

FrameExportActivity FrameExportController::activity() const noexcept { return activity_; }

const std::filesystem::path& FrameExportController::scratchDirectory() const noexcept {
    return scratchDirectory_;
}

std::uint64_t FrameExportController::chargedResourceBytes() const noexcept {
    return ledger_.chargedBytes();
}

void FrameExportController::setDestinationProvider(FrameExportDestinationProvider provider) {
    destinationProvider_ = std::move(provider);
}

void FrameExportController::setApprovalDecisionProvider(
    FrameExportApprovalDecisionProvider provider) {
    approvalDecisionProvider_ = std::move(provider);
}

void FrameExportController::requestExport() {
    if (!canExport()) {
        emit exportFinished(FrameExportOutcome::Refused,
                            tr("Another export is already in progress, or there is nothing to "
                               "export."));
        return;
    }
    if (!destinationProvider_) {
        emit exportFinished(FrameExportOutcome::Refused,
                            tr("No export destination dialog is configured."));
        return;
    }
    auto chosen = destinationProvider_();
    if (!chosen.has_value()) {
        return; // The artist cancelled the dialog; not an error.
    }
    beginExport(std::move(*chosen));
}

void FrameExportController::beginExport(std::filesystem::path destination) {
    if (!canExport()) {
        emit exportFinished(FrameExportOutcome::Refused,
                            tr("Another export is already in progress, or there is nothing to "
                               "export."));
        return;
    }
    if (destination.empty()) {
        emit exportFinished(FrameExportOutcome::Refused, tr("No destination was chosen."));
        return;
    }

    // The one-truth "current frame" source (cited in the implementor's report): CompositionSession
    // owns currentTime()/compositionId()/snapshot(), the SAME accessors
    // CompositionPreviewController::requestPreview() reads to build every preview request
    // (composition_preview_controller.cpp).
    const document::Snapshot snapshot = session_.snapshot();
    const document::CompositionId compositionId = session_.compositionId();
    if (snapshot.project().findComposition(compositionId) == nullptr) {
        emit exportFinished(FrameExportOutcome::Refused,
                            tr("No composition is available to export."));
        return;
    }

    pendingDestination_ = std::move(destination);

    runtime::TaskRequest request(
        "Compile composition for frame export",
        {.kind = runtime::TaskOwnerKind::Export, .id = runtime::TaskOwnerId::fromRaw(1)},
        runtime::TaskPriority::Foreground, runtime::TaskExecutor::Cpu);

    // Init-capture `&compiler = compiler_` (never a same-scope local by reference -- this task runs
    // on a Cpu worker thread strictly after beginExport() has already returned): binds directly to
    // the long-lived compiler this controller was constructed with.
    auto submission = scheduler_.submit<std::shared_ptr<const runtime::CompiledCompositionPlan>>(
        std::move(request),
        [snapshot, compositionId, &compiler = compiler_](runtime::TaskContext& context) {
            using TaskResult =
                runtime::TaskResult<std::shared_ptr<const runtime::CompiledCompositionPlan>>;
            if (context.isCancellationRequested()) {
                return TaskResult::cancelled();
            }
            context.reportProgress({.phase = "Compiling composition",
                                    .subphase = "",
                                    .completed = 0,
                                    .total = std::nullopt});
            auto compileResult = compiler.compile(
                {.snapshot = snapshot, .compositionId = compositionId}, context.cancellation());
            switch (compileResult.status) {
            case runtime::SnapshotCompileStatus::Cancelled:
                return TaskResult::cancelled();
            case runtime::SnapshotCompileStatus::Unsupported:
                return TaskResult::failed(
                    runtime::TaskDiagnostic{.code = "bloom.ui.frame-export.compile-unsupported",
                                            .severity = runtime::DiagnosticSeverity::Error,
                                            .summary = "The composition contains operations "
                                                       "frame export cannot compile",
                                            .detail = {},
                                            .suggestedAction = "Review the composition graph."});
            case runtime::SnapshotCompileStatus::Failed:
                return TaskResult::failed(
                    runtime::TaskDiagnostic{.code = "bloom.ui.frame-export.compile-failed",
                                            .severity = runtime::DiagnosticSeverity::Error,
                                            .summary = "The composition could not be compiled "
                                                       "for export",
                                            .detail = {},
                                            .suggestedAction = "Review the composition graph."});
            case runtime::SnapshotCompileStatus::Compiled:
                break;
            }
            if (compileResult.plan == nullptr) {
                return TaskResult::failed(runtime::TaskDiagnostic{
                    .code = "bloom.ui.frame-export.compile-no-plan",
                    .severity = runtime::DiagnosticSeverity::Error,
                    .summary = "Composition compilation returned no plan",
                    .detail = {},
                    .suggestedAction = "Report this internal error and retry."});
            }
            return TaskResult::succeeded(compileResult.plan);
        });

    if (!submission.accepted()) {
        emit exportFinished(FrameExportOutcome::Failed, tr("The export could not be started."));
        return;
    }
    inFlight_.emplace<CompileHandle>(CompileHandle{std::move(submission.handle)});
    setActivity(FrameExportActivity::CompilingPlan);
    taskUiBridge_.wake();
}

void FrameExportController::pollOnce() {
    if (auto* compiling = std::get_if<CompileHandle>(&inFlight_)) {
        handleCompileResult(*compiling);
        return;
    }
    if (auto* runner = std::get_if<host::OutputAnalysisAttemptRunnerV1>(&inFlight_)) {
        handleAttemptResult(*runner);
        return;
    }
    if (auto* job = std::get_if<ExportJobHandle>(&inFlight_)) {
        handleExportJobResult(*job);
        return;
    }
}

void FrameExportController::handleCompileResult(CompileHandle& compiling) {
    auto result = compiling.handle.tryTakeResult();
    if (!result.has_value()) {
        return;
    }
    if (result->state() != runtime::TaskState::Succeeded || !result->value().has_value() ||
        *result->value() == nullptr) {
        const auto outcome = result->state() == runtime::TaskState::Cancelled
                                 ? FrameExportOutcome::Cancelled
                                 : FrameExportOutcome::Failed;
        const QString message =
            outcome == FrameExportOutcome::Cancelled
                ? tr("The export was cancelled.")
                : firstDiagnosticSummary(result->diagnostics(),
                                         tr("The composition could not be compiled for export."));
        inFlight_.emplace<std::monostate>();
        setActivity(FrameExportActivity::Idle);
        finish(outcome, message);
        return;
    }

    const auto plan = *result->value();
    inFlight_.emplace<std::monostate>();

    // The evaluation memory budget reuses CompositionPreviewController's own default (composition_
    // preview_controller.hpp's kDefaultPreviewPixelStorageByteLimit): the same working-set bound
    // the viewer's own full-resolution preview already runs under for this composition, not a new
    // invented number. This is distinct from (and independent of) the export job's own resource
    // ledger admission below, which governs retained/staged export bytes, not evaluator scratch.
    host::OutputAnalysisAttemptRequestV1 request{
        .plan = plan,
        .evaluation = {.time = session_.currentTime(),
                       .output = plan->output(),
                       .resolution = runtime::CompositionFormatResolution{},
                       .quality = runtime::EvaluationQuality::Reference,
                       .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                       .pixelStorageByteLimit = kDefaultPreviewPixelStorageByteLimit},
        .targetPath = pendingDestination_,
        .overwritePolicy = platform::ArtifactOverwritePolicy::CreateOrReplace,
        .owner = {.kind = runtime::TaskOwnerKind::Export, .id = runtime::TaskOwnerId::fromRaw(1)}};

    auto begin = host::beginOutputAnalysisAttemptV1(scheduler_, artifactCoordinator_, ledger_,
                                                    std::move(request));
    if (!begin) {
        setActivity(FrameExportActivity::Idle);
        finish(FrameExportOutcome::Failed, tr("The export analysis could not be started."));
        return;
    }
    inFlight_.emplace<host::OutputAnalysisAttemptRunnerV1>(std::move(begin).takeHandle());
    setActivity(FrameExportActivity::Analyzing);
    taskUiBridge_.wake();
}

void FrameExportController::handleAttemptResult(host::OutputAnalysisAttemptRunnerV1& runner) {
    auto outcome = runner.tryComplete();
    if (!outcome.has_value()) {
        return;
    }
    inFlight_.emplace<std::monostate>();

    if (!*outcome) {
        setActivity(FrameExportActivity::Idle);
        const auto* failure = outcome->failure();
        const auto exportOutcome = (failure != nullptr && failure->cancelled())
                                       ? FrameExportOutcome::Cancelled
                                       : FrameExportOutcome::Failed;
        finish(exportOutcome, describeAttemptFailure(failure));
        return;
    }

    // OutputAnalysisAttemptOutcomeV1::operator bool() is exactly `attempt_ != nullptr`, so
    // `*outcome` true above already guarantees this; the null check is defensive only.
    auto attempt = outcome->attempt();
    if (attempt == nullptr) {
        setActivity(FrameExportActivity::Idle);
        finish(FrameExportOutcome::Failed, tr("The export analysis returned no attempt."));
        return;
    }
    if (!attempt->approvable() || !attempt->digest().has_value()) {
        setActivity(FrameExportActivity::Idle);
        const auto& report = attempt->report();
        finish(FrameExportOutcome::NotApprovable, report != nullptr
                                                      ? describeNonApprovable(*report)
                                                      : tr("This frame cannot be exported."));
        return;
    }

    presentApproval(attempt);
}

void FrameExportController::presentApproval(
    const std::shared_ptr<const output::OutputAnalysisAttemptV1>& attempt) {
    setActivity(FrameExportActivity::AwaitingApproval);

    // The one place attempt->digest() (a fresh std::optional temporary on every call) is
    // dereferenced: guarded right here, then reused for both the prompt's short form and the
    // approveFrameExportV1() call below, rather than repeating an unprovable-at-a-distance check
    // (handleAttemptResult() already confirmed has_value() before calling this function, but that
    // was a DIFFERENT temporary).
    const auto digest = attempt->digest();
    if (!digest.has_value()) {
        setActivity(FrameExportActivity::Idle);
        finish(FrameExportOutcome::Failed, tr("The export analysis lost its approval digest."));
        return;
    }

    FrameExportApprovalPrompt prompt;
    prompt.destination = pendingDestination_;
    if (attempt->frame() != nullptr) {
        if (const auto* descriptor = attempt->frame()->processImage().descriptor();
            descriptor != nullptr) {
            prompt.width = descriptor->displayWindow().extent().width();
            prompt.height = descriptor->displayWindow().extent().height();
        }
    }
    const auto presetIdentity =
        output::outputPresetIdentityV1(output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1);
    prompt.presetName =
        presetIdentity.has_value()
            ? QString::fromUtf8(presetIdentity->serializedId.data(),
                                static_cast<int>(presetIdentity->serializedId.size()))
            : QStringLiteral("FlatExrRgba32fLinRec709SceneV1");
    prompt.facets = summarizeFacets(attempt->report()->view());
    const auto digestHex = digest->toLowercaseHex();
    prompt.digestShortForm = QString::fromLatin1(digestHex.data(), 16);

    const auto decision = approvalDecisionProvider_ ? approvalDecisionProvider_(prompt)
                                                    : FrameExportApprovalDecision::Cancel;

    if (decision == FrameExportApprovalDecision::Cancel) {
        setActivity(FrameExportActivity::Idle);
        // Discards cleanly: `attempt` (this function's own const& parameter) never held its own
        // strong reference; the ONLY strong reference is handleAttemptResult()'s local `attempt`
        // variable, which unwinds the instant that function returns (right after this call), so its
        // retained ExportResourceReservationV1 drops to zero references and releases the charged
        // bytes then -- no explicit release call exists or is needed (docs/architecture/frame-
        // output.md: "Dismissal ... releases the completed attempt and its reservations"). Verified
        // by this task's cancel-at-approval test via chargedResourceBytes().
        finish(FrameExportOutcome::Cancelled, tr("The export was cancelled."));
        return;
    }

    // Export calls approveFrameExportV1() with the attempt's OWN digest -- never recomputed or
    // edited (docs/architecture/frame-output.md, "Non-Blocking Execution": "Approval itself
    // performs no evaluation, hashing, color work, or filesystem access"; the API's byte-equality
    // is the guard).
    auto approval = host::approveFrameExportV1(publicationCoordinator_, attempt, *digest);
    if (!approval) {
        setActivity(FrameExportActivity::Idle);
        finish(FrameExportOutcome::Failed, tr("The export could not be approved."));
        return;
    }
    beginExportJob(std::move(approval).takeRequest());
}

void FrameExportController::beginExportJob(std::unique_ptr<host::FrameExportRequestV1> request) {
    if (request == nullptr) {
        setActivity(FrameExportActivity::Idle);
        finish(FrameExportOutcome::Failed, tr("The export could not be started."));
        return;
    }
    auto shared = std::make_shared<std::optional<host::FrameExportPublicationResultV1>>();
    auto sharedRequest = std::shared_ptr<host::FrameExportRequestV1>(std::move(request));
    const auto scratchDirectory = scratchDirectory_;

    runtime::TaskRequest taskRequest(
        "Publish exported frame",
        {.kind = runtime::TaskOwnerKind::Export, .id = runtime::TaskOwnerId::fromRaw(1)},
        runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo);

    // Init-capture `&artifacts = artifactCoordinator_` (matching src/host/tests/
    // frame_export_publication_tests.cpp's own `beginExportRun()`): binds the closure's own
    // captured reference directly to the long-lived, application-wide coordinator this controller
    // was constructed with, never to a same-scope local -- this task runs on a BlockingIo worker
    // thread strictly after beginExportJob() has already returned, so capturing any local variable
    // by reference here would dangle.
    auto submission = scheduler_.submit<void>(
        std::move(taskRequest), [&artifacts = artifactCoordinator_, sharedRequest, shared,
                                 scratchDirectory](runtime::TaskContext& context) {
            output::OutputExportProgressCallbackV1 progress =
                [&context](const output::OutputExportProgressV1& update) {
                    context.reportProgress(
                        {.phase = outputExportStagePhase(update.stage),
                         .subphase = "",
                         .completed = update.completed,
                         .total = update.total == 0 ? std::nullopt
                                                    : std::optional<std::uint64_t>(update.total)});
                };
            auto result = host::executeExportPublication(
                context, artifacts, std::move(*sharedRequest), scratchDirectory,
                output::systemOutputExportClockV1(), std::move(progress));
            shared->emplace(std::move(result));
            return runtime::TaskResult<void>::succeeded();
        });

    if (!submission.accepted()) {
        setActivity(FrameExportActivity::Idle);
        finish(FrameExportOutcome::Failed, tr("The export job could not be started."));
        return;
    }
    inFlight_.emplace<ExportJobHandle>(ExportJobHandle{std::move(submission.handle), shared});
    setActivity(FrameExportActivity::Publishing);
    taskUiBridge_.wake();
}

void FrameExportController::handleExportJobResult(ExportJobHandle& job) {
    auto taskResult = job.handle.tryTakeResult();
    if (!taskResult.has_value()) {
        return;
    }
    auto resultOpt = std::move(*job.result);
    inFlight_.emplace<std::monostate>();
    setActivity(FrameExportActivity::Idle);

    if (!resultOpt.has_value()) {
        finish(FrameExportOutcome::Failed, tr("The export job ended without a result."));
        return;
    }
    const auto& result = *resultOpt;
    if (!result) {
        const auto* failure = result.failure();
        finish(exportFailureOutcome(failure), describeExportFailure(failure));
        return;
    }

    const auto* publication = result.publication();
    const auto publicationOutcome =
        publication != nullptr
            ? publication->outcome
            : platform::StagedArtifactPublicationOutcome::FailedBeforePublication;
    const auto destinationText = QString::fromStdString(pendingDestination_.string());
    switch (publicationOutcome) {
    case platform::StagedArtifactPublicationOutcome::Published:
        finish(FrameExportOutcome::Published, tr("Frame exported to %1.").arg(destinationText));
        break;
    case platform::StagedArtifactPublicationOutcome::PublishedWithDurabilityWarning:
        finish(FrameExportOutcome::PublishedWithWarning,
               tr("Frame exported to %1, but a later durability step failed. Consider exporting "
                  "again.")
                   .arg(destinationText));
        break;
    case platform::StagedArtifactPublicationOutcome::Superseded:
        finish(FrameExportOutcome::Cancelled,
               tr("This export was superseded by a newer export or save to the same file."));
        break;
    case platform::StagedArtifactPublicationOutcome::CancelledBeforePublication:
        finish(FrameExportOutcome::Cancelled, tr("The export was cancelled."));
        break;
    case platform::StagedArtifactPublicationOutcome::ExternalModificationConflict:
        finish(FrameExportOutcome::Failed,
               tr("The destination file changed outside Bloom. Choose a different location or "
                  "overwrite explicitly."));
        break;
    case platform::StagedArtifactPublicationOutcome::FailedBeforePublication:
        finish(FrameExportOutcome::Failed,
               tr("The export failed before the file could be replaced; the previous target, if "
                  "any, is unchanged."));
        break;
    }
}

void FrameExportController::setActivity(const FrameExportActivity activity) {
    if (activity_ == activity) {
        return;
    }
    activity_ = activity;
    emit busyChanged();
}

void FrameExportController::finish(const FrameExportOutcome outcome, QString message) {
    emit exportFinished(outcome, std::move(message));
}

QString FrameExportController::describeAttemptFailure(
    const host::OutputAnalysisAttemptFailureV1* failure) const {
    if (failure == nullptr) {
        return tr("The export analysis failed for an unknown reason.");
    }
    if (failure->cancelled()) {
        return tr("The export was cancelled while %1.").arg(stageName(failure->stage()));
    }
    if (const auto* resourceError = failure->payloadAs<output::OutputAnalysisAttemptErrorCodeV1>();
        resourceError != nullptr &&
        *resourceError == output::OutputAnalysisAttemptErrorCodeV1::ResourceReservationFailed) {
        return tr("The export could not reserve enough memory while %1.")
            .arg(stageName(failure->stage()));
    }
    return tr("The export analysis failed while %1.").arg(stageName(failure->stage()));
}

QString
FrameExportController::describeNonApprovable(const output::OutputAnalysisReportV1& report) const {
    QStringList reasons;
    for (const auto& facet : report.view().facets) {
        if (facet.state == output::OutputPreservationStateV1::Missing ||
            facet.state == output::OutputPreservationStateV1::Unsupported) {
            const auto code = output::outputFacetStableCodeTextV1(facet.stableCode);
            const QString codeText =
                code.has_value() ? QString::fromUtf8(code->data(), static_cast<int>(code->size()))
                                 : tr("unspecified");
            reasons << QStringLiteral("%1: %2").arg(facetDisplayName(facet.facet), codeText);
        }
    }
    if (reasons.isEmpty()) {
        return tr("This frame cannot be exported (analysis reported a non-approvable preservation "
                  "result).");
    }
    return tr("This frame cannot be exported: %1").arg(reasons.join("; "));
}

QString FrameExportController::describeExportFailure(
    const host::FrameExportPublicationFailureV1* failure) const {
    if (failure == nullptr) {
        return tr(
            "The export failed before the file could be written; the previous target, if any, "
            "is unchanged.");
    }
    if (failure->stage() == host::FrameExportPublicationStageV1::Cancelled) {
        return tr("The export was cancelled; the previous target, if any, is unchanged.");
    }
    if (const auto* staged = failure->payloadAs<platform::StagedArtifactError>();
        staged != nullptr &&
        *staged == platform::StagedArtifactError::ExternalModificationConflict) {
        return tr("The destination file changed outside Bloom. Choose a different location or "
                  "overwrite explicitly.");
    }
    if (failure->payloadAs<host::FrameExportDeadlineExceededV1>() != nullptr) {
        return tr("The export exceeded its total time limit during %1; the previous target, if "
                  "any, is unchanged.")
            .arg(jobStageName(failure->stage()));
    }
    if (failure->payloadAs<host::FrameExportNoProgressExceededV1>() != nullptr) {
        return tr("The export made no progress for too long during %1; the previous target, if "
                  "any, is unchanged.")
            .arg(jobStageName(failure->stage()));
    }
    if (failure->payloadAs<host::FrameExportResourceExhaustedV1>() != nullptr) {
        return tr("The export ran out of its resource budget during %1; the previous target, if "
                  "any, is unchanged.")
            .arg(jobStageName(failure->stage()));
    }
    return tr("The export failed during %1; the previous target, if any, is unchanged.")
        .arg(jobStageName(failure->stage()));
}

} // namespace bloom::ui
