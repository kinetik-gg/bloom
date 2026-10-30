#pragma once

// The one Qt-side owner of the export queue's change notification (ADR 0033, section 11).
//
// The host's ExportQueueService advances in exactly one place, pump(). In the desktop application
// ONE adapter, owned by the composition root for the application's lifetime, calls pump() from
// TaskUiBridge::resultsAvailable (the coalesced completion drain every task consumer already uses)
// and re-emits a single queueChanged() when the snapshot revision moved. Export panels are
// replaceable and may be closed, duplicated or never opened, so no panel pumps; they read the
// snapshot through this adapter and issue commands through it.
//
// The adapter also reacts to the queue's typed terminal events: a succeeded or failed job is
// announced through UiFeedback (a sound, plus the operating-system notification while Bloom is not
// the active application). A drained queue makes no second sound.
//
// ExportQueuePort is the seam between this class and the host service. The desktop composition
// root passes a thin port over host::ExportQueueService; tests pass a scripted one. It carries only
// what the panel needs, in host types, and every member is non-blocking and does no I/O (the
// service's own threading guarantee).

#include <bloom/document/ids.hpp>
#include <bloom/host/export_queue.hpp>

#include <QDateTime>
#include <QObject>
#include <QString>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bloom::ui {

class TaskUiBridge;
class UiFeedback;

// What a project-truth command (an undoable edit of the queue) did. A refusal carries the
// reason the host gave, ready for a tooltip.
struct ExportEditResult final {
    bool accepted = false;
    QString reason;
    // The job an add created, read from the command result.
    std::optional<document::ExportJobId> addedJob;
    // The output an add-output created, read from the command result.
    std::optional<document::ExportOutputId> addedOutput;
    // Every job an add created, in the order they were asked for.
    std::vector<document::ExportJobId> addedJobs;
};

// A change to a job's render settings. Absent members are left alone.
struct ExportJobEdit final {
    std::optional<std::string> name;
    std::optional<document::ExportRange> range;
    std::optional<bool> enabled;
    // The outer optional says whether to edit; the inner one is the label (absent clears it).
    std::optional<std::optional<std::array<std::uint8_t, 3>>> labelColor;
};

// A change to one output. Absent members are left alone.
struct ExportOutputEdit final {
    // The catalogue preset the output now comes from. With `settings` the output takes those
    // settings under that preset (the Output Module dialog's format change and its edits, one undo
    // entry); without them it takes the preset's own settings.
    std::optional<std::string> presetId;
    std::optional<document::ExportOutputSettings> settings;
    std::optional<document::ExportDestination> destination;
    // A path the artist typed or chose (absolute), turned into a destination record by the edit
    // layer (host::exportDestinationForPath: project-relative inside the project folder, else an
    // external file URI) with this overwrite policy. Wins over `destination`.
    std::optional<std::pair<std::filesystem::path, document::ExportOverwritePolicy>> chosenPath;
    std::optional<bool> enabled;
    std::optional<bool> importIntoAssets;
};

// Builds an edit by assignment: `jobEdit([&](auto& edit) { edit.name = text; })`. Aggregate
// initialization with designated members would have to name every member.
template <typename Setup> [[nodiscard]] ExportJobEdit jobEdit(Setup&& setup) {
    ExportJobEdit edit;
    setup(edit);
    return edit;
}
template <typename Setup> [[nodiscard]] ExportOutputEdit outputEdit(Setup&& setup) {
    ExportOutputEdit edit;
    setup(edit);
    return edit;
}

class ExportQueuePort {
  public:
    virtual ~ExportQueuePort() = default;

    // ---- Reads.
    [[nodiscard]] virtual std::shared_ptr<const host::ExportQueueSnapshot> snapshot() const = 0;
    [[nodiscard]] virtual std::shared_ptr<const host::ExportPresetCatalogueSnapshot>
    catalogue() const = 0;
    [[nodiscard]] virtual host::ExportQueueEventBatch events(std::uint64_t afterSequence) const = 0;
    [[nodiscard]] virtual host::ExportLogBatch log(document::ExportJobId job,
                                                   std::uint64_t afterSequence) const = 0;
    [[nodiscard]] virtual std::optional<document::ExportJobId>
    jobForTaskOwner(runtime::TaskOwnerId owner) const = 0;

    // Pure: the bloom-cli / Python script that recreates `job` (host::exportJobScript). It is a port
    // member so the panel never links the host implementation directly.
    [[nodiscard]] virtual std::string jobScript(const document::ExportJob& job,
                                                host::ExportScriptFormat format) const = 0;

    // ---- The single publisher.
    virtual void pump() = 0;

    // ---- Session commands (not undoable, never saved).
    virtual host::ExportCommandResult startAll() = 0;
    virtual host::ExportCommandResult start(std::span<const document::ExportJobId> jobs) = 0;
    virtual host::ExportCommandResult pause(document::ExportJobId job) = 0;
    virtual host::ExportCommandResult resume(document::ExportJobId job) = 0;
    virtual host::ExportCommandResult stop(document::ExportJobId job) = 0;
    virtual host::ExportCommandResult stopAll() = 0;
    virtual host::ExportCommandResult retry(document::ExportJobId job) = 0;
    virtual host::ExportCommandResult confirmReplace(document::ExportJobId job,
                                                     document::ExportOutputId output) = 0;
    virtual void refreshPreflight() = 0;

    // ---- Project-truth commands: one undo entry each, on the ordinary command path.
    // One job per composition, in order, as one undoable edit. `index` is where the first one goes
    // among the jobs (the end when absent).
    virtual ExportEditResult addJobs(const std::vector<document::CompositionId>& compositions,
                                     std::optional<std::size_t> index) = 0;
    virtual ExportEditResult duplicateJob(document::ExportJobId job) = 0;
    virtual ExportEditResult removeJob(document::ExportJobId job) = 0;
    virtual ExportEditResult removeFinished() = 0;
    // Moves the job so it sits at `index` among the jobs after the move.
    virtual ExportEditResult moveJob(document::ExportJobId job, std::size_t index) = 0;
    virtual ExportEditResult editJob(document::ExportJobId job, const ExportJobEdit& edit) = 0;
    // Adds an output in the format of `presetId` (the catalogue's default when empty), as one
    // undoable edit.
    virtual ExportEditResult addOutput(document::ExportJobId job, const std::string& presetId) = 0;
    virtual ExportEditResult applyPreset(document::ExportJobId job,
                                         document::ExportOutputId output,
                                         const std::string& presetId) = 0;
    virtual ExportEditResult editOutput(document::ExportJobId job, document::ExportOutputId output,
                                        const ExportOutputEdit& edit) = 0;
    virtual ExportEditResult removeOutput(document::ExportJobId job,
                                          document::ExportOutputId output) = 0;
};

class ExportQueueAdapter final : public QObject {
    Q_OBJECT

  public:
    // `feedback` may be null (no sound or notification). Both are borrowed and outlive the adapter.
    ExportQueueAdapter(ExportQueuePort& port, UiFeedback* feedback = nullptr,
                       QObject* parent = nullptr);

    // The application's feedback service; set once it exists (it is built after the editors).
    void setFeedback(UiFeedback* feedback) noexcept { feedback_ = feedback; }
    // The queue service behind the port was replaced (File > New): forget what was seen, and tell
    // every panel to read the new queue.
    void rebind();

    // Connects the completion drain: every TaskUiBridge::resultsAvailable runs drain().
    void attach(TaskUiBridge& bridge);

    // The only caller of ExportQueuePort::pump(). Public for the bridge connection and for tests.
    void drain();

    [[nodiscard]] std::shared_ptr<const host::ExportQueueSnapshot> snapshot() const;
    [[nodiscard]] std::shared_ptr<const host::ExportPresetCatalogueSnapshot> catalogue() const;
    [[nodiscard]] host::ExportLogBatch log(document::ExportJobId job,
                                           std::uint64_t afterSequence) const;
    [[nodiscard]] std::optional<document::ExportJobId>
    jobForTaskOwner(runtime::TaskOwnerId owner) const;
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    // When the job's newest run started, as this adapter first saw it (its elapsed time taken off
    // the moment it was seen). Session state for the Export panel's Started column: it lives as
    // long as the application, so a panel opened later still shows it, and is never saved.
    [[nodiscard]] std::optional<QDateTime> startedAt(document::ExportJobId job) const;
    [[nodiscard]] std::string jobScript(const document::ExportJob& job,
                                        host::ExportScriptFormat format) const {
        return port_.jobScript(job, format);
    }

    // Session commands. Each forwards to the port (whose service pumps before returning) and then
    // lets the adapter see the new revision, so a panel updates at once.
    host::ExportCommandResult startAll();
    host::ExportCommandResult start(document::ExportJobId job);
    host::ExportCommandResult pause(document::ExportJobId job);
    host::ExportCommandResult resume(document::ExportJobId job);
    host::ExportCommandResult stop(document::ExportJobId job);
    host::ExportCommandResult stopAll();
    host::ExportCommandResult retry(document::ExportJobId job);
    host::ExportCommandResult confirmReplace(document::ExportJobId job,
                                             document::ExportOutputId output);
    void refreshPreflight();

    // Project-truth commands.
    ExportEditResult duplicateJob(document::ExportJobId job);
    ExportEditResult removeJob(document::ExportJobId job);
    ExportEditResult removeFinished();
    ExportEditResult moveJob(document::ExportJobId job, std::size_t index);
    ExportEditResult editJob(document::ExportJobId job, const ExportJobEdit& edit);
    ExportEditResult addOutput(document::ExportJobId job, const std::string& presetId = {});
    ExportEditResult applyPreset(document::ExportJobId job, document::ExportOutputId output,
                                 const std::string& presetId);
    ExportEditResult editOutput(document::ExportJobId job, document::ExportOutputId output,
                                const ExportOutputEdit& edit);
    ExportEditResult removeOutput(document::ExportJobId job, document::ExportOutputId output);

    // Export > Add to Export Queue, the Assets context menu and the panel's own button. On
    // success it asks for the Export panel to be revealed and announces the new job so a panel
    // selects it.
    ExportEditResult addCompositionToQueue(document::CompositionId composition);
    // The same for several compositions at once (an Assets drag or its context menu): one job each,
    // in order, one undo entry, revealed and selected. `index` places the first among the jobs.
    ExportEditResult addCompositionsToQueue(const std::vector<document::CompositionId>& compositions,
                                            std::optional<std::size_t> index = std::nullopt);
    // Jobs > Show in Export: reveals the panel and selects `job`.
    void showJob(document::ExportJobId job);
    // Why the queue cannot take a job right now (empty when it can).
    [[nodiscard]] QString addUnavailableReason() const;

    // A job added by addCompositionToQueue() that no panel has selected yet (the panel was not
    // open when it was added). Reading it clears it.
    [[nodiscard]] std::optional<document::ExportJobId> takePendingSelection();

    // Panels ask the composition root to carry out a remedy (host::ExportRemedy).
    void requestRemedy(host::ExportRemedy remedy);

  Q_SIGNALS:
    // rebind() ran: the next snapshot is a different queue even if its revision number repeats.
    void rebound();
    // The snapshot revision moved: exactly once per revision, however many panels listen.
    void queueChanged();
    // The Export panel should be shown (opened if it is not).
    void revealRequested();
    // A new job exists and should be selected by a panel showing the queue.
    void jobAdded(qulonglong job);
    // The Remedy for an unavailable encoder: the existing H.264 consent / locate flow, or the
    // project colour settings, opened by the composition root.
    void remedyRequested(int remedy);

  private:
    // Sees the port's newest snapshot: reads new terminal events and, when the revision moved,
    // emits queueChanged() once. Never pumps.
    void observe();
    void handleEvents();
    [[nodiscard]] QString jobName(document::ExportJobId job) const;

    ExportQueuePort& port_;
    UiFeedback* feedback_ = nullptr;
    std::uint64_t revision_ = 0;
    std::uint64_t lastEventSequence_ = 0;
    std::optional<document::ExportJobId> pendingSelection_;
    struct Started final {
        std::uint64_t run = 0;
        QDateTime at;
    };
    std::map<std::uint64_t, Started> started_;
};

} // namespace bloom::ui
