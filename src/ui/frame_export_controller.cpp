#include <bloom/ui/frame_export_controller.hpp>

#include "composition_editor_support.hpp"
#include "composition_export_dialog.hpp"

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <bloom/host/frame_range_runner.hpp>

#include <bloom/document/project.hpp>
#include <bloom/render/image.hpp>

#include <bloom/ui/kit/dialog.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QPointer>
#include <QStandardPaths>

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <string>
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

// The export bridge's loader path. It reuses exactly the application-relative bundled loader the
// preview service already resolves in apps/bloom/main.cpp: the executable's own directory, never a
// build or system prefix. When this build packaged no loader, the path stays empty and the provider
// uses the existing platform-loader/CPU fallback -- the same explicit unavailable path the preview
// service takes.
[[nodiscard]] runtime::GpuProcessFrameEvaluatorOptions gpuExportOptions() {
    runtime::GpuProcessFrameEvaluatorOptions options;
#ifdef BLOOM_BUNDLED_VULKAN_LOADER
    options.loaderPath =
        std::filesystem::path(QCoreApplication::applicationDirPath().toStdString()) /
        BLOOM_BUNDLED_VULKAN_LOADER_SUBDIR / BLOOM_BUNDLED_VULKAN_LOADER_NAME;
#endif
    return options;
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
    case host::OutputAnalysisAttemptStageV1::ColorPreparing:
        return FrameExportController::tr("preparing the color transform");
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
    case host::FrameExportPublicationStageV1::ColorPreparing:
        return FrameExportController::tr("preparing the color transform");
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

// The exact serialized preset ID from the typed-preset table, never a paraphrase. The fallback
// spellings are unreachable (outputPresetIdentityV1() covers both closed presets) but keep this
// display path total.
[[nodiscard]] QString presetDisplayName(const output::OutputPresetV1 preset) {
    const auto identity = output::outputPresetIdentityV1(preset);
    if (identity.has_value()) {
        return QString::fromUtf8(identity->serializedId.data(),
                                 static_cast<int>(identity->serializedId.size()));
    }
    if (preset == output::OutputPresetV1::PngRgba8SrgbV1)
        return QStringLiteral("PngRgba8SrgbV1");
    if (preset == output::OutputPresetV1::TiffRgba16SrgbV1)
        return QStringLiteral("TiffRgba16SrgbV1");
    return QStringLiteral("FlatExrRgba32fLinRec709SceneV1");
}

} // namespace

QString outputPresetLabel(const output::OutputPresetV1 preset) {
    switch (preset) {
    case output::OutputPresetV1::PngRgba8SrgbV1:
        return QObject::tr("PNG (8-bit sRGB)");
    case output::OutputPresetV1::TiffRgba16SrgbV1:
        return QObject::tr("TIFF (16-bit sRGB)");
    case output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1:
        return QObject::tr("OpenEXR (32-bit float, linear Rec.709)");
    case output::OutputPresetV1::ProResMovV1:
        return QObject::tr("ProRes MOV");
    case output::OutputPresetV1::DnxhrMxfV1:
        return QObject::tr("DNxHR MXF");
    case output::OutputPresetV1::PcmWavV1:
        return QObject::tr("WAV / BWF audio");
    case output::OutputPresetV1::H264MovV1:
        return QObject::tr("H.264 MOV (review)");
    case output::OutputPresetV1::H264Mp4V1:
        return QObject::tr("H.264 MP4");
    case output::OutputPresetV1::HevcMovV1:
        return QObject::tr("HEVC MOV (hardware)");
    case output::OutputPresetV1::HevcMp4V1:
        return QObject::tr("HEVC MP4 (hardware)");
    }
    return presetDisplayName(preset);
}

output::OutputPresetV1
FrameExportController::presetForDestination(const std::filesystem::path& destination) {
    auto extension = destination.extension().string();
    for (auto& character : extension) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    if (extension == ".png")
        return output::OutputPresetV1::PngRgba8SrgbV1;
    if (extension == ".tif" || extension == ".tiff")
        return output::OutputPresetV1::TiffRgba16SrgbV1;
    return output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1;
}

FrameExportController::FrameExportController(
    CompositionSession& session, runtime::TaskScheduler& scheduler, TaskUiBridge& taskUiBridge,
    const runtime::SnapshotCompiler& compiler, host::PublicationCoordinator& publicationCoordinator,
    platform::StagedArtifactCoordinator& artifactCoordinator,
    std::filesystem::path scratchDirectory,
    runtime::QualifiedDisplayProcessorProvider* const displayProcessorProvider,
    std::shared_ptr<host::GpuExportProvider> gpuExportProvider, QObject* parent)
    : QObject(parent), session_(session), scheduler_(scheduler), taskUiBridge_(taskUiBridge),
      compiler_(compiler), publicationCoordinator_(publicationCoordinator),
      artifactCoordinator_(artifactCoordinator),
      displayProcessorProvider_(displayProcessorProvider),
      gpuExportProvider_(gpuExportProvider != nullptr
                             ? std::move(gpuExportProvider)
                             : host::GpuExportProvider::create(gpuExportOptions())),
      scratchDirectory_(scratchDirectory.empty() ? defaultExportScratchDirectory()
                                                 : std::move(scratchDirectory)) {
    std::error_code error;
    std::filesystem::create_directories(scratchDirectory_, error);

    // Non-blocking: schedules the one-time device bootstrap on the shared scheduler's worker and
    // returns immediately, so the UI thread never waits on device creation. The attempt runner
    // defers its first evaluation until this bootstrap is terminal, so the first export is GPU when
    // a device exists rather than a CPU fallback that raced the bootstrap.
    gpuExportProvider_->prepare(scheduler_);

    connect(&taskUiBridge_, &TaskUiBridge::snapshotsPolled, this, &FrameExportController::pollOnce);
    connect(&taskUiBridge_, &TaskUiBridge::resultsAvailable, this,
            &FrameExportController::pollOnce);

    installDefaultDialogs();
}

FrameExportController::~FrameExportController() {
    if (auto* compiling = std::get_if<CompileHandle>(&inFlight_)) {
        compiling->handle.cancel();
    } else if (auto* runner = std::get_if<host::OutputAnalysisAttemptRunnerV1>(&inFlight_)) {
        runner->requestCancellation();
    } else if (auto* preparing = std::get_if<DeliverableHandle>(&inFlight_)) {
        preparing->cancel();
    } else if (auto* job = std::get_if<ExportJobHandle>(&inFlight_)) {
        job->handle.cancel();
    }
}

bool FrameExportController::canExport() const noexcept {
    return !shuttingDown_ && !cachePurgeGate_ && activity_ == FrameExportActivity::Idle &&
           session_.composition() != nullptr;
}

void FrameExportController::setCachePurgeGate(const bool gated) {
    if (cachePurgeGate_ == gated)
        return;
    cachePurgeGate_ = gated;
    if (!gated)
        pollOnce();
}

bool FrameExportController::isBusy() const noexcept {
    return activity_ != FrameExportActivity::Idle;
}

FrameExportActivity FrameExportController::activity() const noexcept { return activity_; }

const std::filesystem::path& FrameExportController::scratchDirectory() const noexcept {
    return scratchDirectory_;
}

std::uint64_t FrameExportController::chargedResourceBytes() const noexcept {
    return ledger_->chargedBytes();
}

void FrameExportController::setDestinationProvider(FrameExportDestinationProvider provider) {
    destinationProvider_ = std::move(provider);
}

void FrameExportController::setRangeProvider(FrameExportRangeProvider provider) {
    rangeProvider_ = std::move(provider);
}

void FrameExportController::setAttemptRequestAdjusterForTesting(
    std::function<void(host::OutputAnalysisAttemptRequestV1&)> adjuster) {
    attemptRequestAdjuster_ = std::move(adjuster);
}
void FrameExportController::setApprovalDecisionProvider(
    FrameExportApprovalDecisionProvider provider) {
    approvalRequester_ = [provider = std::move(provider)](
                             const FrameExportApprovalPrompt& prompt,
                             const std::function<void(FrameExportApprovalDecision)>& reply) {
        reply(provider ? provider(prompt) : FrameExportApprovalDecision::Cancel);
    };
}

void FrameExportController::setApprovalRequester(FrameExportApprovalRequester requester) {
    approvalRequester_ = std::move(requester);
}

void FrameExportController::setDialogOwner(QWidget* owner) { dialogOwner_ = owner; }

void FrameExportController::setH264EncoderService(H264EncoderService* service) {
    h264Encoder_ = service;
}

void FrameExportController::setSequenceExportRetirementOwner(
    std::shared_ptr<host::SequenceExportRetirementOwnerV1> owner) {
    Q_ASSERT(!mediaExport_);
    sequenceExportRetirement_ = std::move(owner);
}

bool FrameExportController::isExportingRange() const noexcept {
    return sequence_.has_value() || mediaExport_ != nullptr;
}

std::uint64_t FrameExportController::publishedFrameCount() const noexcept {
    if (mediaExport_)
        return mediaExport_->encodedFrames();
    return sequence_.has_value() ? sequence_->publishedFrames : 0;
}

std::uint64_t FrameExportController::totalFrameCount() const noexcept {
    if (mediaExport_)
        return mediaExport_->totalFrames();
    return sequence_.has_value() ? sequence_->lastFrame - sequence_->firstFrame + 1 : 0;
}

std::filesystem::path
FrameExportController::sequenceFramePath(const std::filesystem::path& destination,
                                         const std::uint64_t index, const std::uint64_t lastIndex) {
    return host::FrameRangeRunnerV1::sequenceFramePath(destination, index, lastIndex);
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

void FrameExportController::requestRangeExport() {
    if (!canExport()) {
        emit exportFinished(FrameExportOutcome::Refused,
                            tr("Another export is already in progress, or there is nothing to "
                               "export."));
        return;
    }
    if (!rangeProvider_) {
        emit exportFinished(FrameExportOutcome::Refused,
                            tr("No frame-range dialog is configured."));
        return;
    }
    auto chosen = rangeProvider_();
    if (!chosen.has_value()) {
        return; // The artist cancelled the dialog; not an error.
    }
    beginRangeExport(std::move(*chosen));
}

void FrameExportController::beginRangeExport(FrameExportRangeRequest request) {
    if (!canExport()) {
        emit exportFinished(FrameExportOutcome::Refused,
                            tr("Another export is already in progress, or there is nothing to "
                               "export."));
        return;
    }
    if (request.destination.empty()) {
        emit exportFinished(FrameExportOutcome::Refused, tr("No destination was chosen."));
        return;
    }
    const auto context = frameContextFor(session_);
    if (!context.has_value()) {
        emit exportFinished(FrameExportOutcome::Refused,
                            tr("This composition has no valid frame range to export."));
        return;
    }
    // Refused, never clamped: an artist who asked for frames 0-200 of a 100-frame composition has a
    // different intent from one who asked for 0-99, and silently exporting the shorter range would
    // hide that.
    if (request.lastFrame < request.firstFrame || request.lastFrame > context->maxFrameIndexValue) {
        emit exportFinished(FrameExportOutcome::Refused,
                            tr("Frames %1 to %2 are outside this composition's range of 0 to %3.")
                                .arg(request.firstFrame)
                                .arg(request.lastFrame)
                                .arg(context->maxFrameIndexValue));
        return;
    }

    const auto compositionName = session_.composition()->name();
    const auto firstPath =
        host::sequencePublicationPathV1(request.destination, compositionName, request.firstFrame,
                                        request.firstFrame, request.lastFrame, request.naming);
    if (!firstPath) {
        emit exportFinished(FrameExportOutcome::Refused,
                            tr("Sequence names must be unique, ascending filenames with one frame "
                               "token and the preset extension."));
        return;
    }
    pendingDestination_ = *firstPath;
    pendingPreset_ = presetForDestination(request.destination);
    sequence_.emplace(SequenceState{.destination = std::move(request.destination),
                                    .firstFrame = request.firstFrame,
                                    .lastFrame = request.lastFrame,
                                    .nextFrame = request.firstFrame,
                                    .publishedFrames = 0,
                                    .frameRate = context->frameRate,
                                    .duration = context->duration,
                                    .plan = nullptr,
                                    .exr = std::move(request.exr),
                                    .bypassLookNodes = request.bypassLookNodes,
                                    .naming = std::move(request.naming),
                                    .compositionName = compositionName,
                                    .approved = false,
                                    .cancelled = false});
    emit rangeProgressChanged();
    // The plan compile is the SAME stage a single-frame export runs; the sequence only differs in
    // what happens once it lands (see handleCompileResult()).
    beginExport(pendingDestination_);
}

void FrameExportController::requestCancellation() {
    // A question still open is withdrawn (its dialog closes) and answered Cancel here.
    withdrawApproval();
    if (auto held = std::exchange(pendingApprovalAttempt_, nullptr)) {
        if (sequence_.has_value())
            sequence_->cancelled = true;
        resolveFrameApproval(held, FrameExportApprovalDecision::Cancel);
    }
    if (mediaExport_)
        mediaExport_->cancel();
    if (sequence_.has_value()) {
        sequence_->cancelled = true;
    }
    if (auto* compiling = std::get_if<CompileHandle>(&inFlight_)) {
        compiling->handle.cancel();
    } else if (auto* runner = std::get_if<host::OutputAnalysisAttemptRunnerV1>(&inFlight_)) {
        runner->requestCancellation();
    } else if (auto* preparing = std::get_if<DeliverableHandle>(&inFlight_)) {
        preparing->cancel();
    } else if (auto* job = std::get_if<ExportJobHandle>(&inFlight_)) {
        job->handle.cancel();
    }
    taskUiBridge_.wake();
}

void FrameExportController::retireForShutdown() {
    if (shuttingDown_)
        return;
    shuttingDown_ = true;
    requestCancellation();
    if (!pollingComposition_)
        mediaExport_.reset();
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
    const auto colorIntent = runtime::captureEvaluationColorIntent(session_.colorIntent());
    if (!colorIntent) {
        const auto message = tr("The export color intent could not be captured.");
        if (sequence_)
            finishSequence(FrameExportOutcome::Failed, message);
        else
            emit exportFinished(FrameExportOutcome::Failed, message);
        return;
    }
    const auto projectPath = projectPathProvider_ ? projectPathProvider_() : std::nullopt;
    pendingAssetBaseDirectory_ = projectPath ? projectPath->parent_path() : std::filesystem::path{};
    pendingColorIntent_ = *colorIntent;
    pendingTime_ = session_.currentTime();
    const document::CompositionId compositionId = session_.compositionId();
    if (snapshot.project().findComposition(compositionId) == nullptr) {
        emit exportFinished(FrameExportOutcome::Refused,
                            tr("No composition is available to export."));
        return;
    }

    pendingDestination_ = std::move(destination);
    pendingPreset_ = presetForDestination(pendingDestination_);
    const auto availability = output::outputPresetAvailabilityV1(pendingPreset_);
    if (!availability.available) {
        emit exportFinished(FrameExportOutcome::Refused,
                            QString::fromUtf8(availability.reason.data(),
                                              static_cast<int>(availability.reason.size())));
        return;
    }

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
    if (cachePurgeGate_)
        return;
    if (mediaExport_) {
        pollCompositionExport();
        return;
    }
    if (auto* compiling = std::get_if<CompileHandle>(&inFlight_)) {
        handleCompileResult(*compiling);
        return;
    }
    if (auto* runner = std::get_if<host::OutputAnalysisAttemptRunnerV1>(&inFlight_)) {
        handleAttemptResult(*runner);
        return;
    }
    if (auto* preparing = std::get_if<DeliverableHandle>(&inFlight_)) {
        pollDeliverablePreparation(*preparing);
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
        if (sequence_.has_value()) {
            finishSequence(outcome, message);
            return;
        }
        setActivity(FrameExportActivity::Idle);
        finish(outcome, message);
        return;
    }

    const auto plan = *result->value();
    inFlight_.emplace<std::monostate>();

    // A range export compiles ONCE and then drives every frame off this same plan (a compiled plan
    // is time-independent), so the sequence retains it here and advanceSequence() owns the
    // per-frame stages from now on.
    if (sequence_.has_value()) {
        sequence_->plan = plan;
        advanceSequence();
        return;
    }

    if (!beginAttempt(plan, pendingTime_)) {
        setActivity(FrameExportActivity::Idle);
        finish(FrameExportOutcome::Failed, tr("The export analysis could not be started."));
    }
}

bool FrameExportController::beginAttempt(
    const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
    const core::RationalTime time) {
    // The evaluation memory budget is the host-availability-derived GPU/CPU export budget: a
    // conservative share of currently-available RAM, floored and capped (never unbounded), so the
    // gate is open for a large composition while one export cannot plan to own the machine. This is
    // distinct from (and independent of) the export job's own resource ledger admission below,
    // which governs retained/staged export bytes, not evaluator scratch.
    //
    // `time` is the ONLY thing a sequence varies per frame: every frame is evaluated at its own
    // exact rational time, never at an accumulated one, which is exactly what makes an exported
    // sequence a per-frame proof of the animation rather than of the transport.
    host::OutputAnalysisAttemptRequestV1 request{
        .plan = plan,
        .evaluation = {.time = time,
                       .output = plan->output(),
                       .resolution = runtime::CompositionFormatResolution{},
                       .quality = runtime::EvaluationQuality::Reference,
                       .colorIntent = pendingColorIntent_,
                       .pixelStorageByteLimit = 0,
                       .bypassLookNodes = sequence_ && sequence_->bypassLookNodes},
        .assetBaseDirectory = pendingAssetBaseDirectory_,
        .targetPath = pendingDestination_,
        .overwritePolicy = platform::ArtifactOverwritePolicy::CreateOrReplace,
        .owner = {.kind = runtime::TaskOwnerKind::Export, .id = runtime::TaskOwnerId::fromRaw(1)},
        .preset = pendingPreset_,
        .displayProcessorProvider = displayProcessorProvider_,
        .gpuProvider = gpuExportProvider_,
        .useRootPixelCapacity = true};

    if (attemptRequestAdjuster_)
        attemptRequestAdjuster_(request);
    auto begin = host::beginOutputAnalysisAttemptV1(scheduler_, artifactCoordinator_, *ledger_,
                                                    std::move(request));
    if (!begin) {
        return false;
    }
    inFlight_.emplace<host::OutputAnalysisAttemptRunnerV1>(std::move(begin).takeHandle());
    setActivity(FrameExportActivity::Analyzing);
    taskUiBridge_.wake();
    return true;
}

void FrameExportController::advanceSequence() {
    if (!sequence_.has_value()) {
        return;
    }
    if (sequence_->cancelled) {
        finishSequence(FrameExportOutcome::Cancelled,
                       tr("The frame-range export was cancelled after %1 of %2 frames.")
                           .arg(sequence_->publishedFrames)
                           .arg(totalFrameCount()));
        return;
    }
    if (sequence_->nextFrame > sequence_->lastFrame) {
        finishSequence(
            FrameExportOutcome::Published,
            tr("Exported %1 frames to %2.")
                .arg(sequence_->publishedFrames)
                .arg(QString::fromStdString(sequence_->destination.parent_path().string())));
        return;
    }
    const host::FrameRangeRequestV1 frameRequest{.destination = sequence_->destination,
                                                 .firstFrame = sequence_->firstFrame,
                                                 .lastFrame = sequence_->lastFrame,
                                                 .frameRate = sequence_->frameRate,
                                                 .duration = sequence_->duration};
    const auto time = host::FrameRangeRunnerV1::timeForFrame(frameRequest, sequence_->nextFrame);
    if (!time.has_value()) {
        finishSequence(
            FrameExportOutcome::Failed,
            tr("Frame %1 has no exact time in this composition.").arg(sequence_->nextFrame));
        return;
    }
    // Everything the call below needs is read into locals first: beginAttempt() is a non-const
    // call, so nothing may assume `sequence_` is still engaged across it.
    const auto frameIndex = sequence_->nextFrame;
    const auto plan = sequence_->plan;
    const auto path = host::sequencePublicationPathV1(
        sequence_->destination, sequence_->compositionName, frameIndex, sequence_->firstFrame,
        sequence_->lastFrame, sequence_->naming);
    if (!path) {
        finishSequence(FrameExportOutcome::Failed, tr("Invalid sequence filename."));
        return;
    }
    pendingDestination_ = *path;
    if (!beginAttempt(plan, *time)) {
        finishSequence(
            FrameExportOutcome::Failed,
            tr("The export analysis could not be started for frame %1.").arg(frameIndex));
    }
}

void FrameExportController::finishSequence(const FrameExportOutcome outcome, QString message) {
    sequence_.reset();
    inFlight_.emplace<std::monostate>();
    setActivity(FrameExportActivity::Idle);
    emit rangeProgressChanged();
    finish(outcome, std::move(message));
}

void FrameExportController::handleAttemptResult(host::OutputAnalysisAttemptRunnerV1& runner) {
    auto outcome = runner.tryComplete();
    if (!outcome.has_value()) {
        return;
    }
    inFlight_.emplace<std::monostate>();

    if (!*outcome) {
        const auto* failure = outcome->failure();
        const auto exportOutcome = (failure != nullptr && failure->cancelled())
                                       ? FrameExportOutcome::Cancelled
                                       : FrameExportOutcome::Failed;
        if (sequence_.has_value()) {
            finishSequence(exportOutcome, describeAttemptFailure(failure));
            return;
        }
        setActivity(FrameExportActivity::Idle);
        finish(exportOutcome, describeAttemptFailure(failure));
        return;
    }

    // OutputAnalysisAttemptOutcomeV1::operator bool() is exactly `attempt_ != nullptr`, so
    // `*outcome` true above already guarantees this; the null check is defensive only.
    auto attempt = outcome->attempt();
    if (attempt == nullptr) {
        if (sequence_.has_value()) {
            finishSequence(FrameExportOutcome::Failed,
                           tr("The export analysis returned no attempt."));
            return;
        }
        setActivity(FrameExportActivity::Idle);
        finish(FrameExportOutcome::Failed, tr("The export analysis returned no attempt."));
        return;
    }
    if (!attempt->approvable() || !attempt->digest().has_value()) {
        const auto& report = attempt->report();
        const QString message = report != nullptr ? describeNonApprovable(*report)
                                                  : tr("This frame cannot be exported.");
        if (sequence_.has_value()) {
            finishSequence(FrameExportOutcome::NotApprovable, message);
            return;
        }
        setActivity(FrameExportActivity::Idle);
        finish(FrameExportOutcome::NotApprovable, message);
        return;
    }

    if (sequence_ && pendingPreset_ == output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1 &&
        (!sequence_->exr.outputColorSpaceId.empty() ||
         sequence_->exr.compression != output::FlatExrCompressionV1::Zip)) {
        beginDeliverablePreparation(std::move(attempt));
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
    // Read off the completed attempt, which took its preset from the chosen destination extension
    // -- never re-derived from the path here, so the prompt can never name a different preset from
    // the one that was actually analyzed.
    prompt.preset = attempt->preset();
    prompt.presetName = presetDisplayName(prompt.preset);
    prompt.presetLabel = outputPresetLabel(prompt.preset);
    prompt.facets = summarizeFacets(attempt->report()->view());
    prompt.implementationNote =
        QString::fromStdString(output::outputLookDescriptionV1(attempt->frame()->identity()));
    if (const auto& exr = attempt->report()->exr(); exr) {
        prompt.profile = tr("%1; %2").arg(
            QString::fromStdString(exr->options().outputColorSpaceId),
            QString::fromUtf8(output::flatExrCompressionNameV1(exr->options().compression).data())
                .toUpper());
    }
    if (attempt->report()->display())
        prompt.implementationNote +=
            "\n" + QString::fromStdString(attempt->report()->display()->description());
    const auto digestHex = digest->toLowercaseHex();
    prompt.digestShortForm = QString::fromLatin1(digestHex.data(), 16);

    // The artist is asked once per export: for a single frame that is this frame, and for a range
    // it is the FIRST frame only (sequence_->approved). Every frame still runs
    // approveFrameExportV1() with its own attempt and its own digest below -- the byte-equality
    // guard and the per-frame publication intent are never skipped; only the question is.
    if (sequence_.has_value() && sequence_->approved) {
        resolveFrameApproval(attempt, FrameExportApprovalDecision::Export);
        return;
    }
    // The question is asynchronous; the attempt (and its reservation) is held until the answer.
    pendingApprovalAttempt_ = attempt;
    askApproval(prompt, [this](const FrameExportApprovalDecision decision) {
        auto held = std::exchange(pendingApprovalAttempt_, nullptr);
        if (held)
            resolveFrameApproval(held, decision);
    });
}

void FrameExportController::resolveFrameApproval(
    const std::shared_ptr<const output::OutputAnalysisAttemptV1>& attempt,
    FrameExportApprovalDecision decision) {
    const auto digest = attempt->digest();
    if (!digest.has_value()) {
        setActivity(FrameExportActivity::Idle);
        finish(FrameExportOutcome::Failed, tr("The export analysis lost its approval digest."));
        return;
    }
    if (shuttingDown_ || (sequence_.has_value() && sequence_->cancelled))
        decision = FrameExportApprovalDecision::Cancel;
    if (sequence_.has_value() && decision == FrameExportApprovalDecision::Export) {
        sequence_->approved = true;
    }

    if (decision == FrameExportApprovalDecision::Cancel) {
        if (sequence_.has_value()) {
            finishSequence(FrameExportOutcome::Cancelled, tr("The export was cancelled."));
            return;
        }
        setActivity(FrameExportActivity::Idle);
        // Discards cleanly: the attempt's only strong references are this call's caller's (the
        // held pending attempt, or handleAttemptResult()'s local), which unwind right after this
        // returns, so its retained ExportResourceReservationV1 drops to zero references and
        // releases the charged bytes then -- no explicit release call exists or is needed
        // (docs/architecture/frame-output.md: "Dismissal ... releases the completed attempt and
        // its reservations"). Verified by the cancel-at-approval test via chargedResourceBytes().
        finish(FrameExportOutcome::Cancelled, tr("The export was cancelled."));
        return;
    }

    // Export calls approveFrameExportV1() with the attempt's OWN digest -- never recomputed or
    // edited (docs/architecture/frame-output.md, "Non-Blocking Execution": "Approval itself
    // performs no evaluation, hashing, color work, or filesystem access"; the API's byte-equality
    // is the guard).
    auto approval = host::approveFrameExportV1(publicationCoordinator_, attempt, *digest);
    if (!approval) {
        if (sequence_.has_value()) {
            finishSequence(FrameExportOutcome::Failed, tr("The export could not be approved."));
            return;
        }
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
        if (sequence_.has_value()) {
            finishSequence(FrameExportOutcome::Failed,
                           tr("The export job ended without a result."));
            return;
        }
        finish(FrameExportOutcome::Failed, tr("The export job ended without a result."));
        return;
    }
    const auto& result = *resultOpt;
    if (!result) {
        const auto* failure = result.failure();
        if (sequence_.has_value()) {
            finishSequence(exportFailureOutcome(failure), describeExportFailure(failure));
            return;
        }
        finish(exportFailureOutcome(failure), describeExportFailure(failure));
        return;
    }

    const auto* publication = result.publication();
    const auto publicationOutcome =
        publication != nullptr
            ? publication->outcome
            : platform::StagedArtifactPublicationOutcome::FailedBeforePublication;
    const auto destinationText = QString::fromStdString(pendingDestination_.string());

    // A range export treats each frame's publication as one step of the sequence. A published (or
    // durability-warned) frame advances to the next; anything else ends the whole range with that
    // frame's own outcome, leaving every frame already published exactly as it is -- a sequence is
    // a sequence of complete publications, not one transaction that could roll back.
    if (sequence_.has_value()) {
        const bool landed =
            publicationOutcome == platform::StagedArtifactPublicationOutcome::Published ||
            publicationOutcome ==
                platform::StagedArtifactPublicationOutcome::PublishedWithDurabilityWarning;
        if (!landed) {
            finishSequence(FrameExportOutcome::Failed,
                           tr("Frame %1 could not be published to %2; %3 earlier frames are "
                              "unaffected.")
                               .arg(sequence_->nextFrame)
                               .arg(destinationText)
                               .arg(sequence_->publishedFrames));
            return;
        }
        ++sequence_->publishedFrames;
        ++sequence_->nextFrame;
        emit rangeProgressChanged();
        advanceSequence();
        return;
    }

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
    if (const auto* phase = failure->payloadAs<runtime::GpuProcessFrameDiagnosticCode>()) {
        if (*phase == runtime::GpuProcessFrameDiagnosticCode::AwaitingCapacity) {
            return tr("The export is waiting for a current processing-capacity sample. Retry when "
                      "capacity is ready.");
        }
        if (*phase == runtime::GpuProcessFrameDiagnosticCode::BudgetRefused) {
            return tr("The export could not reserve its processing memory. Release other work or "
                      "reduce the request.");
        }
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
    if (failure->payloadAs<host::FrameExportPreparedBytesExceededV1>() != nullptr) {
        return tr("This frame's prepared PNG pixels exceed the export size limit; nothing was "
                  "written and the previous target, if any, is unchanged.");
    }
    return tr("The export failed during %1; the previous target, if any, is unchanged.")
        .arg(jobStageName(failure->stage()));
}

} // namespace bloom::ui
