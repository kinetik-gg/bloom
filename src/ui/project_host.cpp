#include <bloom/ui/project_host.hpp>

#include <bloom/commands/command_stack.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/project.hpp>
#include <bloom/project/save_archive.hpp>

#include <QFileDialog>
#include <QMessageBox>

#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace bloom::ui {

namespace {

constexpr int kPollIntervalMs = 100;
constexpr std::uint64_t kBytesPerMebibyte = 1024ULL * 1024ULL;

} // namespace

ProjectHost::ProjectHost(runtime::TaskScheduler& scheduler, QObject* parent)
    : QObject(parent), scheduler_(scheduler) {
    auto publicationResult = host::PublicationCoordinator::create();
    if (!publicationResult.has_value()) {
        throw std::runtime_error("Bloom could not create the publication coordinator");
    }
    publicationCoordinator_.emplace(std::move(*publicationResult));

    auto artifactResult =
        platform::StagedArtifactCoordinator::create(platform::StagedArtifactConfig{});
    if (!artifactResult) {
        throw std::runtime_error("Bloom could not create the staged-artifact coordinator");
    }
    artifactCoordinator_.emplace(std::move(artifactResult).takeCoordinator());

    auto memoryResult = project::ProjectIoMemoryCoordinator::create();
    if (!memoryResult.has_value()) {
        throw std::runtime_error("Bloom could not create the project I/O memory coordinator");
    }
    memoryCoordinator_.emplace(std::move(*memoryResult));

    decisionProvider_ = [] {
        QMessageBox box;
        box.setWindowTitle(tr("Unsaved Changes"));
        box.setText(tr("This project has unsaved changes."));
        box.setInformativeText(tr("Do you want to save your changes before continuing?"));
        box.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Save);
        switch (box.exec()) {
        case QMessageBox::Save:
            return UnsavedChangeDecision::Save;
        case QMessageBox::Discard:
            return UnsavedChangeDecision::Discard;
        default:
            return UnsavedChangeDecision::Cancel;
        }
    };
    openPathProvider_ = []() -> std::optional<std::filesystem::path> {
        const auto chosen = QFileDialog::getOpenFileName(nullptr, tr("Open Project"), {},
                                                         tr("Bloom Projects (*.bloom)"));
        if (chosen.isEmpty()) {
            return std::nullopt;
        }
        return std::filesystem::path(chosen.toStdString());
    };
    saveAsPathProvider_ = []() -> std::optional<std::filesystem::path> {
        const auto chosen = QFileDialog::getSaveFileName(nullptr, tr("Save Project As"), {},
                                                         tr("Bloom Projects (*.bloom)"));
        if (chosen.isEmpty()) {
            return std::nullopt;
        }
        return std::filesystem::path(chosen.toStdString());
    };

    pollTimer_.setSingleShot(true);
    pollTimer_.setTimerType(Qt::PreciseTimer);
    connect(&pollTimer_, &QTimer::timeout, this, &ProjectHost::poll);

    // Bootstrap the initial session (decision 1/5): the application always starts on a fresh,
    // untouched new project. No one is connected to sessionReplaced()/dirtyStateChanged() yet, so
    // this emission is inert.
    replaceWithNewProject();
}

ProjectHost::~ProjectHost() {
    if (auto* save = std::get_if<host::AsyncSessionSave>(&inFlight_)) {
        save->requestCancellation();
    } else if (auto* open = std::get_if<host::AsyncSessionOpen>(&inFlight_)) {
        open->requestCancellation();
    }
}

host::ProjectSessionStateSnapshot ProjectHost::stateSnapshot() const {
    if (!session_.has_value()) {
        return {};
    }
    return session_->stateSnapshot();
}

bool ProjectHost::canSave() const {
    if (!session_.has_value() || isBusy()) {
        return false;
    }
    const auto snapshot = session_->stateSnapshot();
    return snapshot.valid &&
           snapshot.contentKind == host::ProjectSessionContentKind::DecodedDocument &&
           snapshot.editability == host::DecodedProjectEditability::Editable;
}

bool ProjectHost::isDirty() const {
    if (!session_.has_value()) {
        return false;
    }
    return session_->stateSnapshot().dirty.value_or(false);
}

bool ProjectHost::isBusy() const noexcept { return activity_ != ProjectHostActivity::Idle; }

ProjectHostActivity ProjectHost::activity() const noexcept { return activity_; }

std::optional<std::filesystem::path> ProjectHost::displayPath() const {
    if (!session_.has_value()) {
        return std::nullopt;
    }
    const auto snapshot = session_->stateSnapshot();
    if (!snapshot.displayPath.has_value()) {
        return std::nullopt;
    }
    return snapshot.displayPath->value();
}

std::pair<document::Document*, commands::CommandStack*>
ProjectHost::liveDocumentAndStack() noexcept {
    if (!session_.has_value()) {
        return {nullptr, nullptr};
    }
    return session_->liveDocumentAndStack();
}

document::CompositionId ProjectHost::lowestCompositionId() const {
    if (!session_.has_value()) {
        return {};
    }
    const auto snapshot = session_->decodedSnapshot();
    if (!snapshot) {
        return {};
    }
    const auto compositions = snapshot.snapshot().project().compositions();
    if (compositions.empty()) {
        return {};
    }
    auto lowest = compositions.front().id();
    for (const auto& composition : compositions) {
        if (composition.id().value() < lowest.value()) {
            lowest = composition.id();
        }
    }
    return lowest;
}

void ProjectHost::setUnsavedChangeDecisionProvider(UnsavedChangeDecisionProvider provider) {
    decisionProvider_ = std::move(provider);
}

void ProjectHost::setOpenPathProvider(ProjectPathProvider provider) {
    openPathProvider_ = std::move(provider);
}

void ProjectHost::setSaveAsPathProvider(ProjectPathProvider provider) {
    saveAsPathProvider_ = std::move(provider);
}

bool ProjectHost::hasDirtyDecodedContent() const {
    if (!session_.has_value()) {
        return false;
    }
    const auto snapshot = session_->stateSnapshot();
    return snapshot.valid &&
           snapshot.contentKind == host::ProjectSessionContentKind::DecodedDocument &&
           snapshot.dirty.value_or(false);
}

void ProjectHost::confirmUnsavedChanges(std::function<void()> onProceed) {
    if (activity_ == ProjectHostActivity::ResolvingUnsavedChanges) {
        // "Only one destructive continuation is active. Duplicate close/quit requests are
        // idempotent." (docs/architecture/project-session.md, "Unsaved-Change State Machine").
        return;
    }
    if (!hasDirtyDecodedContent()) {
        onProceed();
        return;
    }
    if (!decisionProvider_) {
        // No provider configured: never silently discard an artist's edit.
        return;
    }

    setActivity(ProjectHostActivity::ResolvingUnsavedChanges);
    const auto decision = decisionProvider_();
    switch (decision) {
    case UnsavedChangeDecision::Cancel:
        setActivity(ProjectHostActivity::Idle);
        return;
    case UnsavedChangeDecision::Discard:
        setActivity(ProjectHostActivity::Idle);
        onProceed();
        return;
    case UnsavedChangeDecision::Save:
        pendingUnsavedContinuation_ = std::move(onProceed);
        // Let beginSave() own activity tracking from here; it refuses while busy, and this flow
        // is itself the only reason activity_ is currently non-Idle.
        setActivity(ProjectHostActivity::Idle);
        beginSave();
        if (activity_ != ProjectHostActivity::Saving) {
            // beginSave() could not even start an async attempt (pathless with no dialog answer,
            // no provider configured, or a typed begin refusal). Abandon this continuation rather
            // than looping automatically: the edit is never discarded, but the artist must retry
            // from the menu. The project stays dirty.
            pendingUnsavedContinuation_.reset();
            setActivity(ProjectHostActivity::Idle);
        }
        return;
    }
}

void ProjectHost::newProject() {
    if (isBusy()) {
        return;
    }
    confirmUnsavedChanges([this] { replaceWithNewProject(); });
}

void ProjectHost::requestOpen() {
    if (isBusy()) {
        return;
    }
    confirmUnsavedChanges([this] {
        if (!openPathProvider_) {
            return;
        }
        auto chosen = openPathProvider_();
        if (!chosen.has_value()) {
            return;
        }
        beginOpen(*chosen);
    });
}

void ProjectHost::requestSaveAs() {
    if (isBusy()) {
        emit saveFinished(ProjectHostOperationOutcome::Refused,
                          tr("Another project operation is already in progress."));
        return;
    }
    promptAndBeginSaveAs();
}

void ProjectHost::promptAndBeginSaveAs() {
    if (!saveAsPathProvider_) {
        emit saveFinished(ProjectHostOperationOutcome::Refused,
                          tr("No Save As dialog is configured."));
        return;
    }
    auto chosen = saveAsPathProvider_();
    if (!chosen.has_value()) {
        return; // The artist cancelled the dialog; not an error.
    }
    beginSaveAs(std::move(*chosen));
}

void ProjectHost::beginSave() {
    if (isBusy() || !session_.has_value()) {
        emit saveFinished(ProjectHostOperationOutcome::Refused,
                          tr("Another project operation is already in progress."));
        return;
    }
    const auto snapshot = session_->stateSnapshot();
    if (!snapshot.valid ||
        snapshot.contentKind != host::ProjectSessionContentKind::DecodedDocument ||
        snapshot.editability != host::DecodedProjectEditability::Editable) {
        emit saveFinished(ProjectHostOperationOutcome::Refused,
                          tr("This project cannot be saved."));
        return;
    }
    if (!snapshot.displayPath.has_value()) {
        // Pathless Save routes to Save As (decision 3; captureSaveInput()'s own PathRequired
        // backs this -- asking the host here instead of duplicating the check in MainWindow).
        promptAndBeginSaveAs();
        return;
    }
    const auto intent = session_->capturePlainSavePathIntent();
    if (!intent.isValid()) {
        emit saveFinished(ProjectHostOperationOutcome::Refused, tr("Save could not start."));
        return;
    }
    startSave(snapshot.displayPath->value(), intent);
}

void ProjectHost::beginSaveAs(std::filesystem::path path) {
    if (isBusy() || !session_.has_value()) {
        emit saveFinished(ProjectHostOperationOutcome::Refused,
                          tr("Another project operation is already in progress."));
        return;
    }
    auto advance = session_->advancePathIntentForSaveAs();
    if (!advance) {
        emit saveFinished(ProjectHostOperationOutcome::Refused, tr("Save As could not start."));
        return;
    }
    startSave(std::move(path), advance.capture());
}

void ProjectHost::startSave(std::filesystem::path targetPath,
                            const host::SessionPathIntentCapture intent) {
    // Callers (beginSave()/beginSaveAs()) already checked isBusy()/session_.has_value(); these
    // guards are repeated defensively so this function is never the one place that assumes a
    // caller-side check without stating its own.
    if (!session_.has_value() || !publicationCoordinator_.has_value() ||
        !artifactCoordinator_.has_value()) {
        emit saveFinished(ProjectHostOperationOutcome::Refused,
                          tr("Another project operation is already in progress."));
        return;
    }
    auto operation = makeOperation();
    if (!operation.has_value()) {
        emit saveFinished(ProjectHostOperationOutcome::Refused,
                          tr("Not enough memory is available to start this save."));
        return;
    }
    const host::SessionSaveRequest request{
        .targetPath = std::move(targetPath),
        .overwritePolicy = platform::ArtifactOverwritePolicy::CreateOrReplace,
        .expectedTarget = std::nullopt,
        .limits = {},
        .intent = intent,
    };
    auto begun = host::beginSessionSave(*session_, scheduler_, *publicationCoordinator_,
                                        *artifactCoordinator_, request, std::move(*operation));
    if (!begun) {
        emit saveFinished(ProjectHostOperationOutcome::Refused,
                          tr("The save could not be started."));
        return;
    }
    inFlight_.emplace<host::AsyncSessionSave>(std::move(begun).takeHandle());
    setActivity(ProjectHostActivity::Saving);
    scheduleNextPoll();
}

void ProjectHost::beginOpen(const std::filesystem::path& path) {
    if (isBusy() || !session_.has_value()) {
        emit openFinished(ProjectHostOperationOutcome::Refused,
                          tr("Another project operation is already in progress."));
        return;
    }

    std::error_code sizeError;
    const auto fileBytes = std::filesystem::file_size(path, sizeError);
    if (sizeError) {
        emit openFinished(ProjectHostOperationOutcome::Failed,
                          tr("The project file could not be read: %1")
                              .arg(QString::fromStdString(sizeError.message())));
        return;
    }

    // Bounded by the archive size limit BEFORE reading fully (task U1 spec): checking the file
    // size against the same physical-archive limit project::openProjectArchive() itself enforces
    // avoids reading an oversize file into memory just to have it refused afterward.
    const project::SaveArchiveLimits limits{};
    if (fileBytes > limits.container.maxArchiveBytes) {
        emit openFinished(ProjectHostOperationOutcome::Oversize,
                          tr("This project file is too large to open (%1 MB; the limit is %2 MB).")
                              .arg(static_cast<qulonglong>(fileBytes / kBytesPerMebibyte))
                              .arg(static_cast<qulonglong>(limits.container.maxArchiveBytes /
                                                           kBytesPerMebibyte)));
        return;
    }

    auto displayPath = host::ProjectDisplayPath::create(path);
    if (!displayPath.has_value()) {
        emit openFinished(ProjectHostOperationOutcome::Refused,
                          tr("The chosen path is not valid."));
        return;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        emit openFinished(ProjectHostOperationOutcome::Failed,
                          tr("The project file could not be opened for reading."));
        return;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(fileBytes));
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream) {
        emit openFinished(ProjectHostOperationOutcome::Failed,
                          tr("The project file could not be fully read."));
        return;
    }

    auto operation = makeOperation();
    if (!operation.has_value()) {
        emit openFinished(ProjectHostOperationOutcome::Refused,
                          tr("Not enough memory is available to start this open."));
        return;
    }

    auto begun = host::beginSessionOpen(*session_, scheduler_, std::move(bytes),
                                        std::move(displayPath), limits, std::move(*operation));
    if (!begun) {
        emit openFinished(ProjectHostOperationOutcome::Refused,
                          tr("The open could not be started."));
        return;
    }
    inFlight_.emplace<host::AsyncSessionOpen>(std::move(begun).takeHandle());
    setActivity(ProjectHostActivity::Opening);
    scheduleNextPoll();
}

void ProjectHost::replaceWithNewProject() {
    host::NewProjectSessionRequest request{
        .projectName = "Untitled",
        .compositionName = "Composition 1",
        .duration = core::RationalTime::fromInteger(10),
        .format = {},
    };
    auto created = host::ProjectSession::createNew(identitySource_, std::move(request));
    if (!created) {
        // RuntimeIdentityExhausted/ResourceUnavailable/InvalidNewProject: unreachable in practice
        // with a fresh identity source and fixed request fields. The previous session (if any) is
        // left completely untouched rather than crashing the application.
        return;
    }
    session_.emplace(std::move(created).takeSession());
    emit sessionReplaced();
    emit dirtyStateChanged();
}

void ProjectHost::poll() {
    if (!session_.has_value()) {
        // Unreachable in practice: an in-flight handle is never created without a valid session,
        // and session_ is only ever replaced, never emptied. Guarded explicitly so tryComplete()
        // below is never called against an absent session; reschedule rather than silently
        // abandoning any in-flight handle.
        if (!std::holds_alternative<std::monostate>(inFlight_)) {
            scheduleNextPoll();
        }
        return;
    }
    if (auto* save = std::get_if<host::AsyncSessionSave>(&inFlight_)) {
        if (!save->isReady()) {
            scheduleNextPoll();
            return;
        }
        auto result = save->tryComplete(*session_);
        inFlight_.emplace<std::monostate>();
        setActivity(ProjectHostActivity::Idle);
        if (result.has_value()) {
            handleSaveResult(std::move(*result));
        }
        return;
    }
    if (auto* open = std::get_if<host::AsyncSessionOpen>(&inFlight_)) {
        if (!open->isReady()) {
            scheduleNextPoll();
            return;
        }
        auto result = open->tryComplete(*session_);
        inFlight_.emplace<std::monostate>();
        setActivity(ProjectHostActivity::Idle);
        if (result.has_value()) {
            handleOpenResult(std::move(*result));
        }
        return;
    }
}

void ProjectHost::scheduleNextPoll() { pollTimer_.start(kPollIntervalMs); }

void ProjectHost::setActivity(const ProjectHostActivity activity) {
    if (activity_ == activity) {
        return;
    }
    activity_ = activity;
    emit activityChanged();
}

void ProjectHost::handleSaveResult(host::SessionSaveResult result) {
    emit dirtyStateChanged();

    auto outcome = ProjectHostOperationOutcome::Failed;
    QString message;

    switch (result.stage()) {
    case host::SessionSaveStage::None:
        outcome = ProjectHostOperationOutcome::Refused;
        message = tr("The save did not run.");
        break;
    case host::SessionSaveStage::Capture:
        outcome = ProjectHostOperationOutcome::Refused;
        message = tr("The save could not capture the current project state.");
        break;
    case host::SessionSaveStage::Publication:
        outcome = ProjectHostOperationOutcome::Failed;
        message = tr("The save failed before the file could be written.");
        break;
    case host::SessionSaveStage::Savepoint: {
        const auto* publication = result.publication();
        const auto publicationOutcome =
            publication != nullptr
                ? publication->outcome
                : platform::StagedArtifactPublicationOutcome::FailedBeforePublication;
        switch (publicationOutcome) {
        case platform::StagedArtifactPublicationOutcome::Published:
            outcome = ProjectHostOperationOutcome::Published;
            message = tr("Project saved.");
            break;
        case platform::StagedArtifactPublicationOutcome::PublishedWithDurabilityWarning:
            outcome = ProjectHostOperationOutcome::PublishedWithWarning;
            message =
                tr("Project saved, but a later durability step failed. Consider saving again.");
            break;
        case platform::StagedArtifactPublicationOutcome::Superseded:
            outcome = ProjectHostOperationOutcome::Superseded;
            message = tr("This save was superseded by a newer save.");
            break;
        case platform::StagedArtifactPublicationOutcome::CancelledBeforePublication:
            outcome = ProjectHostOperationOutcome::Cancelled;
            message = tr("The save was cancelled.");
            break;
        case platform::StagedArtifactPublicationOutcome::ExternalModificationConflict:
            outcome = ProjectHostOperationOutcome::ExternalConflict;
            message =
                tr("The file changed outside Bloom. Reload, Save As, or overwrite explicitly.");
            break;
        case platform::StagedArtifactPublicationOutcome::FailedBeforePublication:
            outcome = ProjectHostOperationOutcome::Failed;
            message = tr("The save failed before the file could be replaced.");
            break;
        }
        if (publication != nullptr && publication->targetWasPublished() &&
            result.savepointStatus().has_value() &&
            *result.savepointStatus() != host::ProjectSessionSavepointStatus::Accepted) {
            // The file is durably published, but the local session bookkeeping refused the
            // savepoint (e.g. a stale intent after a later replacement). Never claim success here
            // even though a real file now exists on disk -- see
            // docs/architecture/project-session.md, "Save Result Acceptance".
            outcome = ProjectHostOperationOutcome::Failed;
            message = tr("The project file was written, but this open project is now out of date "
                         "with it; reopen the file to continue editing it.");
        }
        break;
    }
    }

    emit saveFinished(outcome, message);

    if (pendingUnsavedContinuation_.has_value()) {
        auto continuation = std::move(*pendingUnsavedContinuation_);
        pendingUnsavedContinuation_.reset();
        // Re-run the flow fresh (docs/architecture/project-session.md's "If the document changes
        // while saving, the flow returns to a fresh decision instead of discarding the newer
        // edit"): confirmUnsavedChanges() re-reads dirty state now, so a clean project proceeds
        // immediately, a still-dirty one (failure/supersession/an intervening edit) re-prompts.
        confirmUnsavedChanges(std::move(continuation));
    }
}

void ProjectHost::handleOpenResult(host::SessionOpenResult result) {
    auto outcome = ProjectHostOperationOutcome::Failed;
    QString message;
    bool installed = false;

    switch (result.stage()) {
    case host::SessionOpenStage::Admission:
        outcome = ProjectHostOperationOutcome::Refused;
        message = tr("The open could not start.");
        break;
    case host::SessionOpenStage::Opening:
        outcome = ProjectHostOperationOutcome::Failed;
        message = tr("The project file could not be opened.");
        break;
    case host::SessionOpenStage::NotOpened:
        outcome = ProjectHostOperationOutcome::Cancelled;
        message = tr("The open was cancelled.");
        break;
    case host::SessionOpenStage::Installation: {
        const auto* outcomeVariant = result.installOutcome();
        const auto* status = outcomeVariant != nullptr
                                 ? std::get_if<host::SessionInstallStatus>(outcomeVariant)
                                 : nullptr;
        if (status != nullptr && *status == host::SessionInstallStatus::Installed) {
            installed = true;
            outcome = ProjectHostOperationOutcome::Published;
            // A preserved-read-only install has no live document/command-stack at all (see
            // liveDocumentAndStack()); this build has no workspace surface for that content kind
            // (KNOWN LIMITATION -- see the implementor's report), so callers must not assume a
            // Published open outcome always means an editable composition became available.
            message =
                result.attemptedContentKind() == host::ProjectSessionContentKind::PreservedReadOnly
                    ? tr("Opened as preserved read-only content; this build has no editor for it "
                         "yet.")
                    : tr("Project opened.");
        } else {
            outcome = ProjectHostOperationOutcome::Failed;
            message = tr("The project could not be installed.");
        }
        break;
    }
    }

    emit dirtyStateChanged();
    emit openFinished(outcome, message);

    if (installed) {
        emit sessionReplaced();
    }
}

std::optional<project::ProjectIoOperationMemory> ProjectHost::makeOperation() const {
    if (!memoryCoordinator_.has_value()) {
        return std::nullopt;
    }
    return memoryCoordinator_->createOperation();
}

} // namespace bloom::ui
