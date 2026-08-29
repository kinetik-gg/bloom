#pragma once

#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/host/copy_async_io.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/session_async_io.hpp>
#include <bloom/host/session_open.hpp>
#include <bloom/host/session_save.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <QObject>
#include <QString>
#include <QTimer>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::commands {
class CommandStack;
}

namespace bloom::ui {

// The artist's Save/Discard/Cancel answer to an unsaved-change prompt (docs/architecture/
// project-session.md, "Unsaved-Change State Machine"). Returned by an injected
// UnsavedChangeDecisionProvider so offscreen tests can drive all three answers without a real
// QMessageBox.
enum class UnsavedChangeDecision : std::uint8_t {
    Save,
    Discard,
    Cancel,
};

using UnsavedChangeDecisionProvider = std::function<UnsavedChangeDecision()>;
// Returns a chosen filesystem path, or nullopt for "the artist cancelled the dialog". Used for
// both Open's and Save As's target-path chooser (decision 4's "same seam pattern for the file
// dialogs").
using ProjectPathProvider = std::function<std::optional<std::filesystem::path>()>;

// A typed outcome for saveFinished()/openFinished() -- composed from the host's own typed results
// (bloom::host::SessionSaveResult/SessionOpenResult) without ever collapsing a non-published or
// failed outcome into success. `Refused` covers every typed begin-time refusal (busy, read-only,
// capture/admission/submission failure) that never reached an async worker at all.
enum class ProjectHostOperationOutcome : std::uint8_t {
    Published,
    PublishedWithWarning,
    Superseded,
    Cancelled,
    ExternalConflict,
    Failed,
    Oversize,
    Refused,
};

// What ProjectHost is currently doing, for the menu's enabled-state and the status bar's
// "Saving…"/"Opening…" text (decision 3: "a modal-less 'Saving…'/'Opening…' statusBar message +
// disabled actions until finished").
enum class ProjectHostActivity : std::uint8_t {
    Idle,
    ResolvingUnsavedChanges,
    Saving,
    Opening,
};

// Owns the application's single live bloom::host::ProjectSession plus every service it needs to
// run New/Open/Save/Save-As over that session (task U1, issue #72). ProjectHost is the ONLY
// composition root for these services in the application; it owns:
//   - a bloom::host::ProjectSessionIdentitySource (runtime session identity)
//   - the live bloom::host::ProjectSession itself
//   - a bloom::host::PublicationCoordinator
//   - a bloom::platform::StagedArtifactCoordinator
//   - a reference to the application's bloom::runtime::TaskScheduler (shared with whatever else
//     already needs a BlockingIo worker -- see apps/bloom/main.cpp)
//   - a bloom::project::ProjectIoMemoryCoordinator
//   - at most ONE in-flight bloom::host::AsyncSessionSave OR bloom::host::AsyncSessionOpen; a
//     second request while one is in flight is refused with a typed busy outcome (Refused) rather
//     than queued -- the engine supports more concurrency than this, the UI does not need it yet.
//
// A QTimer (task_ui_bridge.cpp's interval precedent) polls the in-flight handle's isReady() and
// calls tryComplete() once ready, then applies the typed result to the session and emits a typed
// finished signal.
//
// Dialogs are behind small injectable seams (UnsavedChangeDecisionProvider/ProjectPathProvider)
// defaulting to real QMessageBox/QFileDialog implementations, so offscreen tests can drive New,
// Open, and Save entirely without a real dialog appearing.
class ProjectHost final : public QObject {
    Q_OBJECT

  public:
    explicit ProjectHost(runtime::TaskScheduler& scheduler, QObject* parent = nullptr);
    ~ProjectHost() override;

    [[nodiscard]] host::ProjectSessionStateSnapshot stateSnapshot() const;
    // Editable decoded content, not busy -- matches decision 3 exactly ("Save disabled for
    // read-only/busy"). Reused for Save As's own enabled-state; this slice does not distinguish
    // the two.
    // Not noexcept: each reads bloom::host::ProjectSession::stateSnapshot(), which copies
    // std::string undo/redo labels and is not itself noexcept.
    [[nodiscard]] bool canSave() const;
    // Preserved-read-only content with retained original archive bytes, not busy (task SC1, issue
    // #77). Save Copy is the only operation available on preserved-read-only content
    // (docs/architecture/project-session.md: "A preserved-read-only project cannot run native Save
    // or Save As; Save Copy stages, validates, and atomically publishes an asynchronous
    // byte-for-byte copy").
    [[nodiscard]] bool canSaveCopy() const;
    [[nodiscard]] bool isDirty() const;
    [[nodiscard]] bool isBusy() const noexcept;
    [[nodiscard]] ProjectHostActivity activity() const noexcept;
    [[nodiscard]] std::optional<std::filesystem::path> displayPath() const;

    // Design decision 2's UI seam, forwarded from the live ProjectSession: raw, non-owning
    // pointers to the currently installed document/command-stack pair (null for non-decoded
    // content). The caller hands these to a projection's own rebind() (e.g.
    // CompositionSession::rebind()) after sessionReplaced() fires.
    [[nodiscard]] std::pair<document::Document*, commands::CommandStack*>
    liveDocumentAndStack() noexcept;
    // The contract's "lowest valid CompositionId" (docs/architecture/project-session.md, "Session
    // Publication", item 5) for the currently installed decoded content; a default-constructed
    // (invalid) CompositionId when there is no decoded content or no composition at all.
    [[nodiscard]] document::CompositionId lowestCompositionId() const;

    // Seam setters (decision 4). Defaults are installed at construction (QMessageBox/QFileDialog);
    // tests replace them before driving New/Open/Save.
    void setUnsavedChangeDecisionProvider(UnsavedChangeDecisionProvider provider);
    void setOpenPathProvider(ProjectPathProvider provider);
    void setSaveAsPathProvider(ProjectPathProvider provider);

    // Runs the unsaved-change flow (Save/Discard/Cancel) if, and only if, the current content is a
    // dirty decoded document; otherwise `onProceed` runs immediately. Exposed publicly (beyond
    // decision 1's newProject()/beginOpen()/beginSave()/beginSaveAs() list) so MainWindow's window
    // close can share the exact same composed logic per decision 4 ("before New, Open, and window
    // close ... prompt") instead of duplicating it.
    void confirmUnsavedChanges(std::function<void()> onProceed);

  public slots:
    // Composes the unsaved-change flow, then REPLACES the session in-place with a fresh
    // createNew() session (decision 1). Emits sessionReplaced() on success.
    void newProject();
    // The "Open…" menu action's entry point: composes the unsaved-change flow, then the open-path
    // dialog seam, then beginOpen() with the chosen path.
    void requestOpen();
    // The "Save As…" menu action's entry point: the save-path dialog seam, then beginSaveAs() with
    // the chosen path.
    void requestSaveAs();

    // The "Save" menu action's entry point (decision 3): a pathless project routes to the Save As
    // dialog seam internally (session.capturePlainSavePathIntent()'s own PathRequired backs this --
    // asking the host here instead of duplicating the check in MainWindow).
    void beginSave();
    // The "Save a Copy…" menu action's entry point (task SC1, issue #77): enabled only for
    // preserved-read-only content with retained bytes (see canSaveCopy()); reuses the SAME Save As
    // dialog provider seam (setSaveAsPathProvider()) rather than adding a third one. Drives
    // beginCopyPublication() through the single-in-flight variant and poll loop, exactly as
    // beginSave()/beginSaveAs() drive Save/Open. Never touches session state: Save Copy stages,
    // validates, and atomically publishes an asynchronous byte-for-byte copy and never claims to
    // rewrite or migrate the document (docs/architecture/project-session.md).
    void requestSaveCopy();
    // Lower-level primitives (decision 1's literal list): begin the async op against an
    // already-known path. Public so tests can drive them directly, bypassing the dialog seams.
    void beginOpen(const std::filesystem::path& path);
    void beginSaveAs(std::filesystem::path path);
    // Lower-level primitive mirroring beginOpen()/beginSaveAs(): begins Save Copy against an
    // already-known target path, bypassing the Save As dialog seam. Public so tests can drive it
    // directly.
    void beginSaveCopy(std::filesystem::path path);

  signals:
    // Fires exactly once per successful New replace or successful Open install.
    void sessionReplaced();
    void dirtyStateChanged();
    void activityChanged();
    // Typed outcome + a display-ready message (never collapses Superseded/failed into success).
    void saveFinished(bloom::ui::ProjectHostOperationOutcome outcome, QString message);
    void openFinished(bloom::ui::ProjectHostOperationOutcome outcome, QString message);
    // Honest copy outcome (task SC1, issue #77): published / superseded / conflict / failed; the
    // success message names the target file. Never fired for a state that also changes
    // dirtyStateChanged()/sessionReplaced() -- Save Copy never touches session state.
    void copyFinished(bloom::ui::ProjectHostOperationOutcome outcome, QString message);

  private:
    void poll();
    void scheduleNextPoll();
    void setActivity(ProjectHostActivity activity);
    void replaceWithNewProject();
    void promptAndBeginSaveAs();
    void promptAndBeginSaveCopy();
    void startSave(std::filesystem::path targetPath, host::SessionPathIntentCapture intent);
    void startSaveCopy(std::filesystem::path targetPath);
    void handleSaveResult(host::SessionSaveResult result);
    void handleOpenResult(host::SessionOpenResult result);
    void handleCopyResult(host::CopyPublicationResult result);
    [[nodiscard]] std::optional<project::ProjectIoOperationMemory> makeOperation() const;
    [[nodiscard]] bool hasDirtyDecodedContent() const;

    runtime::TaskScheduler& scheduler_;
    host::ProjectSessionIdentitySource identitySource_;
    std::optional<host::PublicationCoordinator> publicationCoordinator_;
    std::optional<platform::StagedArtifactCoordinator> artifactCoordinator_;
    std::optional<project::ProjectIoMemoryCoordinator> memoryCoordinator_;
    std::optional<host::ProjectSession> session_;

    // The application-owned bounded original archive for the currently installed preserved-read-
    // only content (task SC1, issue #77): beginOpen() already owns the bytes it read for the
    // Installed+PreservedReadOnly path, so this retains them instead of dropping them, rather than
    // re-reading the source file on every Save a Copy. Plain std::vector, deliberately NOT
    // PMR-charged through ProjectIoOperationMemory: the resident-budget contract covers the memory
    // an in-flight Project I/O *operation* charges against its own bounded budget, not this
    // longer-lived, application-owned retention -- which is itself bounded (at most one archive, no
    // larger than the archive size limit) independently of any operation's budget. Cleared whenever
    // installed content is replaced by anything other than a matching preserved-read-only open
    // (New, a decoded/editable Open, or a failed-to-retain preserved-read-only open).
    std::vector<std::byte> retainedArchiveBytes_;
    // Holds the bytes read for an in-flight Open until handleOpenResult() knows whether to promote
    // them into retainedArchiveBytes_ (Installed + PreservedReadOnly) or drop them (any other
    // outcome); see beginOpen()'s own comment.
    std::vector<std::byte> pendingOpenArchiveBytes_;
    // The target path an in-flight Save Copy is publishing to, retained only to name the file in
    // copyFinished()'s success message once the async operation completes.
    std::filesystem::path pendingCopyTargetPath_;

    std::variant<std::monostate, host::AsyncSessionSave, host::AsyncSessionOpen,
                 host::AsyncCopyPublication>
        inFlight_;
    QTimer pollTimer_;
    ProjectHostActivity activity_ = ProjectHostActivity::Idle;

    UnsavedChangeDecisionProvider decisionProvider_;
    ProjectPathProvider openPathProvider_;
    ProjectPathProvider saveAsPathProvider_;

    // Set only while the unsaved-change flow's Save choice is running an async save; consumed
    // exactly once when that save completes (docs/architecture/project-session.md's "If the
    // document changes while saving, the flow returns to a fresh decision instead of discarding
    // the newer edit" -- implemented by re-running confirmUnsavedChanges(), which re-reads dirty
    // state fresh).
    std::optional<std::function<void()>> pendingUnsavedContinuation_;
};

} // namespace bloom::ui
