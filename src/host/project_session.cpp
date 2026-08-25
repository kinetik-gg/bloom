#include <bloom/host/project_session.hpp>

#include <bloom/document/new_project.hpp>

#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace {

[[nodiscard]] constexpr bool
isKnownEditability(const bloom::host::DecodedProjectEditability value) noexcept {
    switch (value) {
    case bloom::host::DecodedProjectEditability::Editable:
    case bloom::host::DecodedProjectEditability::DegradedEditable:
        return true;
    }
    return false;
}

static_assert(std::is_nothrow_move_constructible_v<bloom::host::ProjectDisplayPath>);
static_assert(std::is_nothrow_move_constructible_v<std::optional<bloom::host::ProjectDisplayPath>>);
static_assert(std::is_nothrow_move_constructible_v<bloom::host::ProjectSession>);

} // namespace

namespace bloom::host {

ProjectSessionIdentitySourceSnapshot ProjectSessionIdentitySource::snapshot() const noexcept {
    std::lock_guard lock(mutex_);
    return {
        .lastIssuedSessionId = ProjectSessionId::fromRaw(lastIssuedSessionId_),
        .identityExhausted = lastIssuedSessionId_ == std::numeric_limits<std::uint64_t>::max(),
    };
}

std::optional<ProjectSessionId> ProjectSessionIdentitySource::issue() noexcept {
    std::lock_guard lock(mutex_);
    if (lastIssuedSessionId_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    ++lastIssuedSessionId_;
    return ProjectSessionId::fromRaw(lastIssuedSessionId_);
}

bool ProjectSessionIdentitySource::setLastIssuedSessionIdForTesting(
    const std::uint64_t value) noexcept {
    std::lock_guard lock(mutex_);
    if (value < lastIssuedSessionId_) {
        return false;
    }
    lastIssuedSessionId_ = value;
    return true;
}

std::optional<ProjectDisplayPath> ProjectDisplayPath::create(std::filesystem::path value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    return ProjectDisplayPath(std::move(value));
}

DecodedProjectSnapshotResult::DecodedProjectSnapshotResult(
    const DecodedProjectSnapshotStatus status) noexcept
    : status_(status) {}

DecodedProjectSnapshotResult::DecodedProjectSnapshotResult(document::Snapshot snapshot) noexcept
    : status_(DecodedProjectSnapshotStatus::Available), snapshot_(std::move(snapshot)) {}

const document::Snapshot& DecodedProjectSnapshotResult::snapshot() const& {
    if (!snapshot_.has_value()) {
        throw std::logic_error("Project session does not own a decoded document snapshot");
    }
    return *snapshot_;
}

ProjectSession::ProjectSession(const ProjectSessionId projectSessionId,
                               std::unique_ptr<document::Document> document,
                               std::unique_ptr<commands::CommandStack> commandStack,
                               const DecodedProjectEditability editability,
                               const document::Revision cleanRevision,
                               std::optional<ProjectDisplayPath> displayPath) noexcept
    : projectSessionId_(projectSessionId), contentKind_(ProjectSessionContentKind::DecodedDocument),
      editability_(editability), displayPath_(std::move(displayPath)),
      cleanRevision_(cleanRevision), document_(std::move(document)),
      commandStack_(std::move(commandStack)), valid_(true) {}

ProjectSession::ProjectSession(const ProjectSessionId projectSessionId,
                               ProjectDisplayPath preservedDisplayPath) noexcept
    : projectSessionId_(projectSessionId),
      contentKind_(ProjectSessionContentKind::PreservedReadOnly),
      displayPath_(std::move(preservedDisplayPath)), valid_(true) {}

ProjectSession::ProjectSession(ProjectSession&& other) noexcept
    : projectSessionId_(std::exchange(other.projectSessionId_, ProjectSessionId{})),
      resultAcceptanceGeneration_(
          std::exchange(other.resultAcceptanceGeneration_, SessionResultAcceptanceGeneration{})),
      openIntentGeneration_(std::exchange(other.openIntentGeneration_, OpenIntentGeneration{})),
      pathIntentGeneration_(
          std::exchange(other.pathIntentGeneration_, SessionPathIntentGeneration{})),
      pathIntentKind_(other.pathIntentKind_),
      newestAcceptedPublicationIntent_(
          std::exchange(other.newestAcceptedPublicationIntent_, PublicationIntentId{})),
      contentKind_(other.contentKind_), editability_(other.editability_),
      displayPath_(std::move(other.displayPath_)), cleanRevision_(other.cleanRevision_),
      document_(std::move(other.document_)), commandStack_(std::move(other.commandStack_)),
      valid_(std::exchange(other.valid_, false)) {}

ProjectSessionCreateResult ProjectSession::createNew(ProjectSessionIdentitySource& identitySource,
                                                     NewProjectSessionRequest request) {
    try {
        auto created = document::makeNewProject(std::move(request.projectName),
                                                std::move(request.compositionName),
                                                request.duration, request.format);
        auto document = std::make_unique<document::Document>(std::move(created.project));
        auto commandStack = std::make_unique<commands::CommandStack>(*document);
        const auto cleanRevision = document->snapshot().revision();
        const auto projectSessionId = identitySource.issue();
        if (!projectSessionId.has_value()) {
            return ProjectSessionCreateResult(ProjectSessionCreateStatus::RuntimeIdentityExhausted);
        }
        return ProjectSessionCreateResult(
            ProjectSession(*projectSessionId, std::move(document), std::move(commandStack),
                           DecodedProjectEditability::Editable, cleanRevision, std::nullopt));
    } catch (const std::bad_alloc&) {
        return ProjectSessionCreateResult(ProjectSessionCreateStatus::ResourceUnavailable);
    } catch (const std::logic_error&) {
        return ProjectSessionCreateResult(ProjectSessionCreateStatus::InvalidNewProject);
    }
}

ProjectSessionCreateResult
ProjectSession::createDecoded(ProjectSessionIdentitySource& identitySource,
                              DecodedProjectSessionRequest request) {
    if (!isKnownEditability(request.editability)) {
        return ProjectSessionCreateResult(ProjectSessionCreateStatus::InvalidDecodedProject);
    }
    try {
        std::unique_ptr<document::Document> document;
        if (request.persistedAllocatorHighWater.has_value()) {
            document = std::make_unique<document::Document>(std::move(request.project),
                                                            *request.persistedAllocatorHighWater);
        } else {
            document = std::make_unique<document::Document>(std::move(request.project));
        }
        auto commandStack = std::make_unique<commands::CommandStack>(*document);
        const auto cleanRevision = document->snapshot().revision();
        const auto projectSessionId = identitySource.issue();
        if (!projectSessionId.has_value()) {
            return ProjectSessionCreateResult(ProjectSessionCreateStatus::RuntimeIdentityExhausted);
        }
        return ProjectSessionCreateResult(
            ProjectSession(*projectSessionId, std::move(document), std::move(commandStack),
                           request.editability, cleanRevision, std::move(request.displayPath)));
    } catch (const std::bad_alloc&) {
        return ProjectSessionCreateResult(ProjectSessionCreateStatus::ResourceUnavailable);
    } catch (const std::invalid_argument&) {
        return ProjectSessionCreateResult(ProjectSessionCreateStatus::InvalidDecodedProject);
    }
}

ProjectSessionCreateResult
ProjectSession::createPreservedReadOnly(ProjectSessionIdentitySource& identitySource,
                                        std::filesystem::path displayPath) {
    auto validatedPath = ProjectDisplayPath::create(std::move(displayPath));
    if (!validatedPath.has_value()) {
        return ProjectSessionCreateResult(ProjectSessionCreateStatus::InvalidDisplayPath);
    }
    const auto projectSessionId = identitySource.issue();
    if (!projectSessionId.has_value()) {
        return ProjectSessionCreateResult(ProjectSessionCreateStatus::RuntimeIdentityExhausted);
    }
    return ProjectSessionCreateResult(ProjectSession(*projectSessionId, std::move(*validatedPath)));
}

bool ProjectSession::isValid() const noexcept {
    if (!valid_ || !projectSessionId_.isValid() || !resultAcceptanceGeneration_.isValid() ||
        !openIntentGeneration_.isValid() || !pathIntentGeneration_.isValid() ||
        !SessionPathIntentCapture::isKnownKind(pathIntentKind_) ||
        (!displayPath_.has_value() &&
         contentKind_ == ProjectSessionContentKind::PreservedReadOnly)) {
        return false;
    }
    if (contentKind_ == ProjectSessionContentKind::PreservedReadOnly) {
        return document_ == nullptr && commandStack_ == nullptr && !editability_.has_value() &&
               !cleanRevision_.has_value();
    }
    return document_ != nullptr && commandStack_ != nullptr && editability_.has_value() &&
           cleanRevision_.has_value();
}

ProjectSessionStateSnapshot ProjectSession::stateSnapshot() const {
    ProjectSessionStateSnapshot result{
        .projectSessionId = projectSessionId_,
        .resultAcceptanceGeneration = resultAcceptanceGeneration_,
        .openIntentGeneration = openIntentGeneration_,
        .pathIntentGeneration = pathIntentGeneration_,
        .pathIntentKind = pathIntentKind_,
        .newestAcceptedPublicationIntent = newestAcceptedPublicationIntent_,
        .contentKind = contentKind_,
        .editability = editability_,
        .displayPath = displayPath_,
        .currentRevision = std::nullopt,
        .cleanRevision = cleanRevision_,
        .dirty = std::nullopt,
        .canUndo = false,
        .canRedo = false,
        .historySize = 0,
        .undoLabel = std::nullopt,
        .redoLabel = std::nullopt,
        .valid = isValid(),
    };
    if (!result.valid || contentKind_ != ProjectSessionContentKind::DecodedDocument) {
        return result;
    }

    const auto current = document_->snapshot().revision();
    result.currentRevision = current;
    result.dirty = current != cleanRevision_.value_or(document::Revision{});
    result.canUndo = commandStack_->canUndo();
    result.canRedo = commandStack_->canRedo();
    result.historySize = commandStack_->size();
    if (const auto label = commandStack_->undoLabel()) {
        result.undoLabel = std::string(*label);
    }
    if (const auto label = commandStack_->redoLabel()) {
        result.redoLabel = std::string(*label);
    }
    return result;
}

SessionResultAcceptanceCapture ProjectSession::captureResultAcceptance() const noexcept {
    if (!isValid()) {
        return {};
    }
    return SessionResultAcceptanceCapture(projectSessionId_, resultAcceptanceGeneration_);
}

SessionPathIntentCapture ProjectSession::capturePlainSavePathIntent() const noexcept {
    const auto resultAcceptance = captureResultAcceptance();
    if (!resultAcceptance.isValid() || contentKind_ != ProjectSessionContentKind::DecodedDocument ||
        !displayPath_.has_value() || pathIntentKind_ != SessionPathIntentKind::ExistingPath) {
        return {};
    }
    return SessionPathIntentCapture(resultAcceptance, pathIntentGeneration_,
                                    SessionPathIntentKind::ExistingPath);
}

OpenIntentAdmissionResult ProjectSession::admitOpenIntent() noexcept {
    if (!isValid()) {
        return OpenIntentAdmissionResult(OpenIntentAdmissionStatus::InvalidSession);
    }
    if (resultAcceptanceGeneration_.value() == std::numeric_limits<std::uint64_t>::max() ||
        openIntentGeneration_.value() == std::numeric_limits<std::uint64_t>::max()) {
        return OpenIntentAdmissionResult(OpenIntentAdmissionStatus::RuntimeIdentityExhausted);
    }
    openIntentGeneration_ = OpenIntentGeneration::fromRaw(openIntentGeneration_.value() + 1);
    const auto decodedRevision =
        contentKind_ == ProjectSessionContentKind::DecodedDocument
            ? std::optional<document::Revision>{commandStack_->trackedRevision()}
            : std::nullopt;
    return OpenIntentAdmissionResult(OpenIntentCapture(
        captureResultAcceptance(), openIntentGeneration_, contentKind_, decodedRevision));
}

SessionPathIntentAdvanceResult ProjectSession::advancePathIntentForSaveAs() noexcept {
    if (!isValid()) {
        return SessionPathIntentAdvanceResult(SessionPathIntentAdvanceStatus::InvalidSession);
    }
    if (contentKind_ != ProjectSessionContentKind::DecodedDocument) {
        return SessionPathIntentAdvanceResult(SessionPathIntentAdvanceStatus::ReadOnly);
    }
    if (pathIntentGeneration_.value() == std::numeric_limits<std::uint64_t>::max()) {
        return SessionPathIntentAdvanceResult(
            SessionPathIntentAdvanceStatus::RuntimeIdentityExhausted);
    }
    pathIntentGeneration_ = SessionPathIntentGeneration::fromRaw(pathIntentGeneration_.value() + 1);
    pathIntentKind_ = SessionPathIntentKind::ReplacementPath;
    newestAcceptedPublicationIntent_ = {};
    return SessionPathIntentAdvanceResult(SessionPathIntentCapture(
        captureResultAcceptance(), pathIntentGeneration_, SessionPathIntentKind::ReplacementPath));
}

SessionPathIntentAbandonStatus
ProjectSession::abandonSaveAsIntent(const SessionPathIntentCapture intent) noexcept {
    if (!isValid()) {
        return SessionPathIntentAbandonStatus::InvalidSession;
    }
    if (contentKind_ != ProjectSessionContentKind::DecodedDocument) {
        return SessionPathIntentAbandonStatus::ReadOnly;
    }
    if (intent.kind() != SessionPathIntentKind::ReplacementPath || !matchesPathIntent(intent)) {
        return SessionPathIntentAbandonStatus::StaleIntent;
    }
    pathIntentKind_ = SessionPathIntentKind::ExistingPath;
    return SessionPathIntentAbandonStatus::Abandoned;
}

bool ProjectSession::matchesResultAcceptance(
    const SessionResultAcceptanceCapture capture) const noexcept {
    return isValid() && capture.isValid() && capture == captureResultAcceptance();
}

bool ProjectSession::isDesiredOpenIntent(const OpenIntentCapture capture) const noexcept {
    if (!isValid() || !capture.isValid() ||
        capture.resultAcceptance() != captureResultAcceptance() ||
        capture.generation() != openIntentGeneration_ || capture.contentKind() != contentKind_) {
        return false;
    }
    if (contentKind_ == ProjectSessionContentKind::PreservedReadOnly) {
        return true;
    }
    return capture.decodedRevision() == commandStack_->trackedRevision();
}

bool ProjectSession::matchesPathIntent(const SessionPathIntentCapture capture) const noexcept {
    return isValid() && capture.isValid() &&
           capture.resultAcceptance() == captureResultAcceptance() &&
           capture.generation() == pathIntentGeneration_ && capture.kind() == pathIntentKind_;
}

SessionResultAcceptanceAdvanceStatus
ProjectSession::advanceResultAcceptanceForInstalledReplacement() noexcept {
    if (!isValid()) {
        return SessionResultAcceptanceAdvanceStatus::InvalidSession;
    }
    if (resultAcceptanceGeneration_.value() == std::numeric_limits<std::uint64_t>::max()) {
        return SessionResultAcceptanceAdvanceStatus::RuntimeIdentityExhausted;
    }
    resultAcceptanceGeneration_ =
        SessionResultAcceptanceGeneration::fromRaw(resultAcceptanceGeneration_.value() + 1);
    pathIntentKind_ = SessionPathIntentKind::ExistingPath;
    newestAcceptedPublicationIntent_ = {};
    return SessionResultAcceptanceAdvanceStatus::Advanced;
}

bool ProjectSession::setGenerationsForTesting(
    const SessionResultAcceptanceGeneration resultAcceptanceGeneration,
    const OpenIntentGeneration openIntentGeneration,
    const SessionPathIntentGeneration pathIntentGeneration) noexcept {
    if (!isValid() || !resultAcceptanceGeneration.isValid() || !openIntentGeneration.isValid() ||
        !pathIntentGeneration.isValid() ||
        resultAcceptanceGeneration < resultAcceptanceGeneration_ ||
        openIntentGeneration < openIntentGeneration_ ||
        pathIntentGeneration < pathIntentGeneration_) {
        return false;
    }
    resultAcceptanceGeneration_ = resultAcceptanceGeneration;
    openIntentGeneration_ = openIntentGeneration;
    pathIntentGeneration_ = pathIntentGeneration;
    return true;
}

DecodedProjectSnapshotResult ProjectSession::decodedSnapshot() const {
    if (!isValid()) {
        return DecodedProjectSnapshotResult(DecodedProjectSnapshotStatus::InvalidSession);
    }
    if (contentKind_ != ProjectSessionContentKind::DecodedDocument) {
        return DecodedProjectSnapshotResult(DecodedProjectSnapshotStatus::NoDecodedDocument);
    }
    return DecodedProjectSnapshotResult(document_->snapshot());
}

ProjectSessionCommandResult ProjectSession::execute(commands::Transaction transaction) {
    if (!isValid() || contentKind_ != ProjectSessionContentKind::DecodedDocument) {
        return unavailableCommandResult();
    }
    return {.status = ProjectSessionCommandStatus::Completed,
            .command = commandStack_->execute(std::move(transaction))};
}

ProjectSessionCommandResult ProjectSession::undo() {
    if (!isValid() || contentKind_ != ProjectSessionContentKind::DecodedDocument) {
        return unavailableCommandResult();
    }
    return {.status = ProjectSessionCommandStatus::Completed, .command = commandStack_->undo()};
}

ProjectSessionCommandResult ProjectSession::redo() {
    if (!isValid() || contentKind_ != ProjectSessionContentKind::DecodedDocument) {
        return unavailableCommandResult();
    }
    return {.status = ProjectSessionCommandStatus::Completed, .command = commandStack_->redo()};
}

ProjectSessionSavepointStatus ProjectSession::acceptSavepoint(
    const SessionPathIntentCapture intent, const PublicationIntentId publicationIntent,
    const document::Revision publishedRevision, std::optional<ProjectDisplayPath> publishedPath) {
    if (!isValid()) {
        return ProjectSessionSavepointStatus::InvalidSession;
    }
    if (contentKind_ != ProjectSessionContentKind::DecodedDocument) {
        return ProjectSessionSavepointStatus::ReadOnly;
    }
    if (!publicationIntent.isValid()) {
        return ProjectSessionSavepointStatus::InvalidPublicationIntent;
    }
    if (newestAcceptedPublicationIntent_.isValid() &&
        publicationIntent <= newestAcceptedPublicationIntent_) {
        return ProjectSessionSavepointStatus::StaleIntent;
    }
    if (!matchesPathIntent(intent)) {
        return ProjectSessionSavepointStatus::StaleIntent;
    }
    const auto current = document_->snapshot().revision();
    if (publishedRevision > current) {
        return ProjectSessionSavepointStatus::UnknownRevision;
    }
    if (intent.kind() == SessionPathIntentKind::ExistingPath && !displayPath_.has_value()) {
        return ProjectSessionSavepointStatus::PathRequired;
    }
    if (intent.kind() == SessionPathIntentKind::ExistingPath && publishedPath.has_value()) {
        return ProjectSessionSavepointStatus::PathAuthorityMismatch;
    }
    if (intent.kind() == SessionPathIntentKind::ReplacementPath && !publishedPath.has_value()) {
        return ProjectSessionSavepointStatus::PathRequired;
    }

    if (intent.kind() == SessionPathIntentKind::ReplacementPath) {
        displayPath_ = std::move(publishedPath);
        pathIntentKind_ = SessionPathIntentKind::ExistingPath;
    }
    cleanRevision_ = publishedRevision;
    newestAcceptedPublicationIntent_ = publicationIntent;
    return ProjectSessionSavepointStatus::Accepted;
}

ProjectSessionCommandResult ProjectSession::unavailableCommandResult() const noexcept {
    return {.status = valid_ ? ProjectSessionCommandStatus::ReadOnly
                             : ProjectSessionCommandStatus::InvalidSession,
            .command = std::nullopt};
}

ProjectSessionCreateResult::ProjectSessionCreateResult(
    const ProjectSessionCreateStatus status) noexcept
    : status_(status) {}

ProjectSessionCreateResult::ProjectSessionCreateResult(ProjectSession session) noexcept
    : status_(ProjectSessionCreateStatus::Created), session_(std::move(session)) {}

ProjectSession ProjectSessionCreateResult::takeSession() && noexcept {
    if (!session_.has_value()) {
        std::terminate();
    }
    return std::move(*session_);
}

} // namespace bloom::host
