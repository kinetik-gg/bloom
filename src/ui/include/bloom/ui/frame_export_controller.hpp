#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/host/frame_export_publication.hpp>
#include <bloom/host/output_analysis_attempt_runner.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/output/output_analysis_attempt.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
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
    // The typed preset the chosen destination extension selected, read straight off the completed
    // attempt (never re-derived from the path at prompt time).
    output::OutputPresetV1 preset = output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1;
    // The exact serialized preset ID (docs/architecture/frame-output.md's typed-preset table) --
    // "PngRgba8SrgbV1" or "FlatExrRgba32fLinRec709SceneV1", never a paraphrase.
    QString presetName;
    FrameExportFacetSummary facets;
    // The first 16 lowercase hex characters of attempt->digest()->toLowercaseHex() -- "the digest's
    // short form" (design decision 3).
    QString digestShortForm;
    QString implementationNote{};
    QString profile{};
};

enum class FrameExportApprovalDecision : std::uint8_t {
    Export,
    Cancel,
};

// One "Export Frame Range..." request (task S5, item 3a). `firstFrame`/`lastFrame` are inclusive
// composition frame INDICES, and every frame between them is evaluated at its own exact rational
// time (ui::frameTimeForIndex(), the same exact mapping the timeline ruler and the transport use)
// -- never at an accumulated or approximated one.
struct FrameExportRangeRequest final {
    std::filesystem::path destination;
    std::uint64_t firstFrame = 0;
    std::uint64_t lastFrame = 0;
    host::SequenceNamingV1 naming = {};
};

struct CompositionExportRequest final {
    FrameExportRangeRequest range;
    output::OutputPresetV1 preset = output::OutputPresetV1::ProResMovV1;
    std::string profile = "hq";
    bool audio = true;
    bool hardware = false;
    bool openh264Consent = false;
    std::uint32_t sampleRate = 48000;
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
using FrameExportRangeProvider = std::function<std::optional<FrameExportRangeRequest>()>;
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
    // `displayProcessorProvider`, when non-null, must be the SAME application-wide
    // bloom::runtime::QualifiedDisplayProcessorProvider the viewer's preview pipeline reads, and
    // must outlive this object. A PNG export then REUSES that already-built qualified processor
    // instead of resolving and building a second one on its own blocking stage. When it is null
    // (or still Pending), the PNG attempt builds its own -- a real cost, never a silent fallback
    // to an unqualified transform. It is a trailing defaulted parameter so the composition root
    // can adopt it without every existing caller changing.
    FrameExportController(
        CompositionSession& session, runtime::TaskScheduler& scheduler, TaskUiBridge& taskUiBridge,
        const runtime::SnapshotCompiler& compiler,
        host::PublicationCoordinator& publicationCoordinator,
        platform::StagedArtifactCoordinator& artifactCoordinator,
        std::filesystem::path scratchDirectory = {},
        runtime::QualifiedDisplayProcessorProvider* displayProcessorProvider = nullptr,
        QObject* parent = nullptr);
    ~FrameExportController() override;

    // A composition exists and no export is currently in flight (menu-enabled rule; MainWindow
    // additionally folds in its own read-only-placeholder check -- see main_window.cpp).
    [[nodiscard]] bool canExport() const noexcept;
    [[nodiscard]] bool isBusy() const noexcept;
    // True while a FRAME RANGE export is in flight, which is the only export long enough for a
    // cancel affordance to mean anything (a single frame is one attempt plus one publish).
    [[nodiscard]] bool isExportingRange() const noexcept;
    // How many frames of the in-flight range have published so far, and how many it will publish in
    // total. Both zero when no range export is running. Exposed for a progress readout and for
    // tests.
    [[nodiscard]] std::uint64_t publishedFrameCount() const noexcept;
    [[nodiscard]] std::uint64_t totalFrameCount() const noexcept;
    [[nodiscard]] FrameExportActivity activity() const noexcept;
    [[nodiscard]] const std::filesystem::path& scratchDirectory() const noexcept;
    // Test observability seam: bloom::output::ExportResourceLedgerV1::chargedBytes() for the ledger
    // this controller privately owns. Used to verify a dismissed/cancelled attempt charges nothing
    // (docs/architecture/frame-output.md: "Dismissal or supersession cancels unfinished work and
    // releases the completed attempt and its reservations").
    [[nodiscard]] std::uint64_t chargedResourceBytes() const noexcept;

    // Seam setters (ProjectHost's decision-4 precedent). Defaults are installed at construction (a
    // real QFileDialog::getSaveFileName offering the closed ".exr"/".png"/".tiff" filters; TIFF is
    // backed by the supervised worker when available; a real
    // QMessageBox Export/Cancel prompt naming the selected preset), so
    // offscreen tests can drive the whole flow without a real dialog appearing.
    void setDestinationProvider(FrameExportDestinationProvider provider);
    void setRangeProvider(FrameExportRangeProvider provider);
    void setApprovalDecisionProvider(FrameExportApprovalDecisionProvider provider);

    // The artist-facing name of frame `index` under `destination`: the destination's stem, a dot,
    // the index zero-padded to at least four digits (widened when the range needs more), then the
    // destination's own extension -- "title.0007.png". Static and pure so a test can state the
    // naming contract without running an export.
    [[nodiscard]] static std::filesystem::path
    sequenceFramePath(const std::filesystem::path& destination, std::uint64_t index,
                      std::uint64_t lastIndex);

    // The typed preset a destination path selects: the closed extension->preset mapping the export
    // command uses (design decision 3: "the chosen extension selects the preset"). ".png", ".tif",
    // and ".tiff" (ASCII case-insensitive) select their typed presets; every other extension --
    // including ".exr", no extension at all, and an unrecognized one -- keeps the flat OpenEXR
    // preset this command has always used, so no pre-existing destination changes meaning.
    [[nodiscard]] static output::OutputPresetV1
    presetForDestination(const std::filesystem::path& destination);

  public slots:
    // "File -> Export Frame..." entry point: refuses while busy or without a composition, else
    // invokes the destination-dialog seam, then beginExport() with the chosen path.
    void requestExport();
    // Lower-level primitive (mirrors ProjectHost::beginSaveAs()): begins export against an
    // already-known destination, bypassing the destination dialog seam. Public so tests can drive
    // it directly.
    void beginExport(std::filesystem::path destination);
    // "File -> Export Frame Range..." entry point: refuses while busy or without a composition,
    // else invokes the range-dialog seam, then beginRangeExport().
    void requestRangeExport();
    void requestCompositionExport();
    void beginCompositionExport(CompositionExportRequest request);
    // The dialog-free primitive, mirroring beginExport(). A range whose frames are not all inside
    // the composition's own valid index range is refused outright rather than silently clamped.
    void beginRangeExport(FrameExportRangeRequest request);
    // Cancels whatever stage is in flight and abandons the rest of a range. Frames already
    // published stay published -- a sequence export is a sequence of complete publications, not one
    // transaction -- and the terminal outcome says how many landed. A no-op when nothing is
    // running.
    void requestCancellation();

  signals:
    void busyChanged();
    // Emitted whenever publishedFrameCount() changes during a range export, so a progress readout
    // never has to poll.
    void rangeProgressChanged();
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

    // The in-flight frame range (task S5, item 3a). Absent for a single-frame export, which is what
    // keeps every existing path byte-for-byte unchanged: the sequence is a driver that REUSES the
    // single-frame stages rather than a second copy of them.
    struct SequenceState final {
        std::filesystem::path destination;
        std::uint64_t firstFrame = 0;
        std::uint64_t lastFrame = 0;
        std::uint64_t nextFrame = 0;
        std::uint64_t publishedFrames = 0;
        document::FrameRate frameRate;
        core::RationalTime duration;
        // The plan is compiled ONCE for the whole range: a compiled plan is time-independent (every
        // animated parameter is a curve index the evaluator samples at the request time), so
        // recompiling per frame would be pure waste and could not change a pixel.
        std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
        // The artist approves ONCE, from the first frame's own completed attempt. Every subsequent
        // frame still goes through approveFrameExportV1() with ITS OWN attempt and ITS OWN digest
        // -- the byte-equality guard and the per-frame publication intent are never bypassed -- but
        // the artist is not asked again. Asking per frame would make a hundred-frame range a
        // hundred modal dialogs, which is not an approval, it is an obstacle.
        host::SequenceNamingV1 naming = {};
        std::string compositionName = {};
        bool approved = false;
        bool cancelled = false;
    };

    void pollOnce();
    void pollCompositionExport();
    void handleCompileResult(CompileHandle& compiling);
    void handleAttemptResult(host::OutputAnalysisAttemptRunnerV1& runner);
    void handleExportJobResult(ExportJobHandle& job);
    void presentApproval(const std::shared_ptr<const output::OutputAnalysisAttemptV1>& attempt);
    void beginExportJob(std::unique_ptr<host::FrameExportRequestV1> request);
    // Starts the attempt for `sequence_`'s next frame, or finishes the range when none is left.
    void advanceSequence();
    // Builds and submits one frame's attempt against an already-compiled plan at an exact time.
    [[nodiscard]] bool
    beginAttempt(const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
                 core::RationalTime time);
    void finishSequence(FrameExportOutcome outcome, QString message);
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
    runtime::QualifiedDisplayProcessorProvider* displayProcessorProvider_ = nullptr;
    output::ExportResourceLedgerV1 ledger_;
    std::filesystem::path scratchDirectory_;

    FrameExportDestinationProvider destinationProvider_;
    FrameExportRangeProvider rangeProvider_;
    FrameExportApprovalDecisionProvider approvalDecisionProvider_;
    std::optional<SequenceState> sequence_;
    std::unique_ptr<host::SequenceExportRunnerV1> mediaExport_;

    FrameExportActivity activity_ = FrameExportActivity::Idle;
    std::filesystem::path pendingDestination_;
    output::OutputPresetV1 pendingPreset_ = output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1;

    std::variant<std::monostate, CompileHandle, host::OutputAnalysisAttemptRunnerV1,
                 ExportJobHandle>
        inFlight_;
};

} // namespace bloom::ui
