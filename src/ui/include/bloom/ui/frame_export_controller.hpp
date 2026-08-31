#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/host/frame_export_publication.hpp>
#include <bloom/host/output_analysis_attempt_runner.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/output/output_analysis_attempt.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <variant>

namespace bloom::ui {

class CompositionSession;
class TaskUiBridge;

// What the artist is offered to approve before FrameExportController calls
// bloom::host::approveFrameExportV1() (task F3, issue #103, design decision 3). Every field is read
// straight off the completed, retained bloom::output::OutputAnalysisAttemptV1 -- never recomputed,
// never user-editable. digestShortForm is a DISPLAY truncation of attempt->digest(); Export always
// approves with the attempt's own full digest (see FrameExportController::presentApproval() in
// frame_export_controller.cpp) -- the UI never invents, edits, or recomputes it.
struct FrameExportFacetSummary final {
    int exactFacetCount = 0;
    int nonExactFacetCount = 0;
    // Display-ready facet names (e.g. "Pixel Aspect") for every facet whose state is not Exact, in
    // fixed facet-ID order (docs/architecture/frame-output.md's eleven-facet table).
    QStringList nonExactFacetNames;
};

struct FrameExportApprovalPrompt final {
    std::filesystem::path destination;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // The exact serialized preset ID (docs/architecture/frame-output.md's typed-preset table) --
    // "FlatExrRgba32fLinRec709SceneV1" in version 1, never a paraphrase.
    QString presetName;
    FrameExportFacetSummary facets;
    // The first 16 lowercase hex characters of attempt->digest()->toLowercaseHex() -- "the digest's
    // short form" (design decision 3).
    QString digestShortForm;
};

enum class FrameExportApprovalDecision : std::uint8_t {
    Export,
    Cancel,
};

// Terminal, artist-facing outcomes. Mirrors ProjectHostOperationOutcome's honesty contract: never
// collapse a non-published, non-approvable, or failed outcome into success.
enum class FrameExportOutcome : std::uint8_t {
    Published,
    PublishedWithWarning,
    Cancelled,
    NotApprovable,
    Failed,
    Refused,
};

enum class FrameExportActivity : std::uint8_t {
    Idle,
    CompilingPlan,
    Analyzing,
    AwaitingApproval,
    Publishing,
};

using FrameExportDestinationProvider = std::function<std::optional<std::filesystem::path>()>;
using FrameExportApprovalDecisionProvider =
    std::function<FrameExportApprovalDecision(const FrameExportApprovalPrompt&)>;

// Composition root for "File -> Export Frame..." (task F3, issue #103). Drives the ACTIVE
// composition at CompositionSession::currentTime() -- the same one-truth "current frame" source
// CompositionPreviewController::requestPreview() already reads (composition_preview_controller.cpp)
// -- through: a plan-compile Cpu task (runtime::SnapshotCompiler, this controller's own thin
// UI-side task submission -- src/output/src/host add no new ABI for it), then
// bloom::host::beginOutputAnalysisAttemptV1() (Resolving/Evaluating/Identifying/Analyzing, polled
// off TaskUiBridge::snapshotsPolled per the qualified-display-processor bootstrap's poll-and-
// publish-once precedent), an artist approval decision, bloom::host::approveFrameExportV1(), and
// finally bloom::host::executeExportPublication() on its own BlockingIo task. Every stage is a real
// bloom::runtime::TaskScheduler task, so attempt/job progress reaches the ordinary
// Jobs/task-monitor surface automatically -- no separate progress plumbing is added here.
//
// One export at a time per window (design decision 5): the action stays disabled for the whole
// CompilingPlan/Analyzing/AwaitingApproval/Publishing span, exactly like ProjectHost's single
// in-flight Save/Open/Save-Copy. No queueing UI.
class FrameExportController final : public QObject {
    Q_OBJECT

  public:
    // `session`, `scheduler`, `taskUiBridge`, and `compiler` must outlive this object.
    // `publicationCoordinator`/`artifactCoordinator` must be the SAME application-wide instances
    // ProjectHost owns (ProjectHost::publicationCoordinator()/artifactCoordinator()) so save/export
    // target ordering and supersession share one truth (docs/architecture/frame-output.md,
    // "Capability Boundary": "Project saves and frame exports reuse one
    // src/platform::StagedArtifactCoordinator" and "The application-wide PublicationCoordinator
    // owns ... supersession across saves and exports"). `scratchDirectory`, when non-empty,
    // overrides the default app-private temp location (tests use this to stay inside their own temp
    // fixture); either way the resolved directory is created if missing.
    FrameExportController(CompositionSession& session, runtime::TaskScheduler& scheduler,
                          TaskUiBridge& taskUiBridge, const runtime::SnapshotCompiler& compiler,
                          host::PublicationCoordinator& publicationCoordinator,
                          platform::StagedArtifactCoordinator& artifactCoordinator,
                          std::filesystem::path scratchDirectory = {}, QObject* parent = nullptr);
    ~FrameExportController() override;

    // A composition exists and no export is currently in flight (menu-enabled rule; MainWindow
    // additionally folds in its own read-only-placeholder check -- see main_window.cpp).
    [[nodiscard]] bool canExport() const noexcept;
    [[nodiscard]] bool isBusy() const noexcept;
    [[nodiscard]] FrameExportActivity activity() const noexcept;
    [[nodiscard]] const std::filesystem::path& scratchDirectory() const noexcept;
    // Test observability seam: bloom::output::ExportResourceLedgerV1::chargedBytes() for the ledger
    // this controller privately owns. Used to verify a dismissed/cancelled attempt charges nothing
    // (docs/architecture/frame-output.md: "Dismissal or supersession cancels unfinished work and
    // releases the completed attempt and its reservations").
    [[nodiscard]] std::uint64_t chargedResourceBytes() const noexcept;

    // Seam setters (ProjectHost's decision-4 precedent). Defaults are installed at construction (a
    // real QFileDialog::getSaveFileName ".exr" filter; a real QMessageBox Export/Cancel prompt), so
    // offscreen tests can drive the whole flow without a real dialog appearing.
    void setDestinationProvider(FrameExportDestinationProvider provider);
    void setApprovalDecisionProvider(FrameExportApprovalDecisionProvider provider);

  public slots:
    // "File -> Export Frame..." entry point: refuses while busy or without a composition, else
    // invokes the destination-dialog seam, then beginExport() with the chosen path.
    void requestExport();
    // Lower-level primitive (mirrors ProjectHost::beginSaveAs()): begins export against an
    // already-known destination, bypassing the destination dialog seam. Public so tests can drive
    // it directly.
    void beginExport(std::filesystem::path destination);

  signals:
    void busyChanged();
    // Typed outcome + a display-ready message, exactly like ProjectHost's saveFinished()/
    // openFinished() (never collapses a non-published or failed outcome into success).
    void exportFinished(bloom::ui::FrameExportOutcome outcome, QString message);

  private:
    struct CompileHandle final {
        runtime::TaskHandle<std::shared_ptr<const runtime::CompiledCompositionPlan>> handle;
    };
    struct ExportJobHandle final {
        runtime::TaskHandle<void> handle;
        std::shared_ptr<std::optional<host::FrameExportPublicationResultV1>> result;
    };

    void pollOnce();
    void handleCompileResult(CompileHandle& compiling);
    void handleAttemptResult(host::OutputAnalysisAttemptRunnerV1& runner);
    void handleExportJobResult(ExportJobHandle& job);
    void presentApproval(const std::shared_ptr<const output::OutputAnalysisAttemptV1>& attempt);
    void beginExportJob(std::unique_ptr<host::FrameExportRequestV1> request);
    void setActivity(FrameExportActivity activity);
    void finish(FrameExportOutcome outcome, QString message);
    [[nodiscard]] QString
    describeAttemptFailure(const host::OutputAnalysisAttemptFailureV1* failure) const;
    [[nodiscard]] QString describeNonApprovable(const output::OutputAnalysisReportV1& report) const;
    [[nodiscard]] QString
    describeExportFailure(const host::FrameExportPublicationFailureV1* failure) const;

    CompositionSession& session_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& taskUiBridge_;
    const runtime::SnapshotCompiler& compiler_;
    host::PublicationCoordinator& publicationCoordinator_;
    platform::StagedArtifactCoordinator& artifactCoordinator_;
    output::ExportResourceLedgerV1 ledger_;
    std::filesystem::path scratchDirectory_;

    FrameExportDestinationProvider destinationProvider_;
    FrameExportApprovalDecisionProvider approvalDecisionProvider_;

    FrameExportActivity activity_ = FrameExportActivity::Idle;
    std::filesystem::path pendingDestination_;

    std::variant<std::monostate, CompileHandle, host::OutputAnalysisAttemptRunnerV1,
                 ExportJobHandle>
        inFlight_;
};

} // namespace bloom::ui
