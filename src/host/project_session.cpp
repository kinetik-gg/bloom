#include <bloom/host/project_session.hpp>

#include <bloom/document/new_project.hpp>
#include <bloom/host/bloom_neutral_profile.hpp>

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

ProjectSession::ProjectSession(
    const ProjectSessionId projectSessionId, std::unique_ptr<document::Document> document,
    std::unique_ptr<commands::CommandStack> commandStack,
    const DecodedProjectEditability editability, const document::Revision cleanRevision,
    std::optional<ProjectDisplayPath> displayPath,
    std::optional<document::ColorSettings> colorSettings,
    std::optional<project::RoundTripState> roundTrip, const std::uint32_t schemaMinor,
    std::vector<project::ManifestRequirement> retainedRequirements) noexcept
    : projectSessionId_(projectSessionId), contentKind_(ProjectSessionContentKind::DecodedDocument),
      editability_(editability), displayPath_(std::move(displayPath)),
      cleanRevision_(cleanRevision), colorSettings_(std::move(colorSettings)),
      roundTrip_(std::move(roundTrip)), schemaMinor_(schemaMinor),
      retainedRequirements_(std::move(retainedRequirements)), document_(std::move(document)),
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
      colorSettings_(std::move(other.colorSettings_)), roundTrip_(std::move(other.roundTrip_)),
      schemaMinor_(std::exchange(other.schemaMinor_, std::uint32_t{0})),
      retainedRequirements_(std::move(other.retainedRequirements_)),
      decodedContentReservations_(std::move(other.decodedContentReservations_)),
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
        // A brand-new project installs the immutable Bloom Neutral v1 built-in color settings
        // (docs/architecture/color-management.md: "New version 1 projects use the immutable
        // built-in configuration Bloom Neutral v1"), keyed by the reviewed, checked-in
        // kBloomNeutralV1ConfigDigest constant (bloom_neutral_profile.hpp) -- no roundTrip,
        // schemaMinor stays 0, and retainedRequirements stays empty, exactly as for any other
        // freshly authored (not reopened) document.
        return ProjectSessionCreateResult(
            ProjectSession(*projectSessionId, std::move(document), std::move(commandStack),
                           DecodedProjectEditability::Editable, cleanRevision, std::nullopt,
                           document::makeBloomNeutralColorSettingsV1(kBloomNeutralV1ConfigDigest),
                           std::nullopt, 0, {}));
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
    // A roundTrip view with schemaMinor 0 cannot name the newer minor it was captured against
    // (see canonical_document.hpp's CanonicalDocumentV1::schemaMinor comment): typed create
    // failure rather than silently writing it back at {1, 0}.
    if (request.roundTrip.has_value() && request.schemaMinor == 0) {
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
                           request.editability, cleanRevision, std::move(request.displayPath),
                           std::move(request.colorSettings), std::move(request.roundTrip),
                           request.schemaMinor, std::move(request.retainedRequirements)));
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

std::optional<SessionInstallStatus>
ProjectSession::checkInstallAcceptanceGates(const OpenIntentCapture intent) const noexcept {
    if (!isValid()) {
        return SessionInstallStatus::InvalidSession;
    }
    // Gate 1 (StaleOpenIntent): the same runtime-session/generation/content-kind identity check
    // isDesiredOpenIntent() performs, but WITHOUT that method's bundled content-revision leg. A
    // literal isDesiredOpenIntent(intent) call cannot be used to drive this gate order: that method
    // already folds the decoded-revision comparison into its single boolean (see
    // testOpenIntentBindsExactContent in project_session_tests.cpp, which pins that a successful
    // edit alone makes isDesiredOpenIntent() false), so an edit-during-Open would be
    // indistinguishable from a superseded/stale Open admission and could never report
    // RevisionChanged -- the exact status the task's edit-during-Open test requires. Splitting the
    // check into three orthogonal gates (identity/generation, result-acceptance, content-revision)
    // also makes AcceptanceMismatch genuinely reachable: installing a capture taken before a
    // successful prior install has an unchanged OpenIntentGeneration (installation never advances
    // that counter) but a stale SessionResultAcceptanceGeneration, so it is refused here at Gate 2,
    // not Gate 1. isDesiredOpenIntent() itself is completely unchanged -- this is new, install-only
    // logic living beside it, not a redefinition of existing frozen semantics.
    if (!intent.isValid() || intent.generation() != openIntentGeneration_ ||
        intent.contentKind() != contentKind_) {
        return SessionInstallStatus::StaleOpenIntent;
    }
    // Gate 2 (AcceptanceMismatch).
    if (!matchesResultAcceptance(intent.resultAcceptance())) {
        return SessionInstallStatus::AcceptanceMismatch;
    }
    // Gate 3 (RevisionChanged): only meaningful for a session currently holding decoded content --
    // Gate 1 already proved intent.contentKind() == contentKind_, and OpenIntentCapture's own
    // invariant (hasValidContentBinding) guarantees a PreservedReadOnly capture carries no decoded
    // revision, so preserved-read-only current content has nothing further to check here (matching
    // docs/architecture/project-session.md's "Open Intent": "preserved read-only content remains
    // bound by its runtime session and acceptance generation without a decoded revision").
    if (contentKind_ == ProjectSessionContentKind::DecodedDocument &&
        commandStack_->trackedRevision() != intent.decodedRevision()) {
        return SessionInstallStatus::RevisionChanged;
    }
    return std::nullopt;
}

SessionInstallStatus ProjectSession::installDecodedReplacement(const OpenIntentCapture intent,
                                                               DecodedReplacementContent content) {
    if (const auto gate = checkInstallAcceptanceGates(intent)) {
        return *gate;
    }
    // Mirrors createDecoded()'s own request validation exactly (isKnownEditability(), and the
    // roundTrip/schemaMinor pairing check) -- a decoded install is otherwise the same kind of
    // content createDecoded() accepts. See SessionInstallStatus::InvalidContent's comment.
    if (content.document_ == nullptr || !isKnownEditability(content.editability_) ||
        (content.roundTrip_.has_value() && content.schemaMinor_ == 0)) {
        return SessionInstallStatus::InvalidContent;
    }
    // Both generations this method advances must be checked for exhaustion BEFORE the allocation
    // below, so a RuntimeIdentityExhausted refusal never allocates and the strong exception
    // guarantee (see this method's declaration comment) covers the whole method, not just the
    // allocating step.
    if (resultAcceptanceGeneration_.value() == std::numeric_limits<std::uint64_t>::max() ||
        pathIntentGeneration_.value() == std::numeric_limits<std::uint64_t>::max()) {
        return SessionInstallStatus::RuntimeIdentityExhausted;
    }

    // The one allocation this method performs: a fresh CommandStack bound to the new document.
    // Constructed before any session-state mutation below, so a thrown std::bad_alloc here leaves
    // the session completely untouched (strong exception guarantee).
    auto freshStack = std::make_unique<commands::CommandStack>(*content.document_);

    // Atomic installation (docs/architecture/project-session.md, "Session Publication").
    const auto advanceStatus = advanceResultAcceptanceForInstalledReplacement();
    if (advanceStatus != SessionResultAcceptanceAdvanceStatus::Advanced) {
        // Unreachable in practice: the exhaustion boundary was already checked above under the
        // same single-threaded synchronous call, so resultAcceptanceGeneration_ cannot have changed
        // in between. Handled anyway so this method's control flow stays provably total.
        return SessionInstallStatus::RuntimeIdentityExhausted;
    }
    // A pending Save As replacement phase is cancelled by installation (project-session.md, "Save
    // Inputs And Intent": "Installing replacement session content also cancels a pending
    // replacement phase"). advanceResultAcceptanceForInstalledReplacement() already reset
    // pathIntentKind_ to ExistingPath; advancing the generation too keeps every session generation
    // moving forward on a real content replacement (both the old plain-save and the old Save As
    // capture are already stale from the resultAcceptance change alone -- matchesPathIntent()
    // requires resultAcceptance equality -- so this generation advance is state hygiene, not load-
    // bearing for that exclusion).
    pathIntentGeneration_ = SessionPathIntentGeneration::fromRaw(pathIntentGeneration_.value() + 1);

    document_ = std::move(content.document_);
    commandStack_ = std::move(freshStack);
    colorSettings_ = std::move(content.colorSettings_);
    roundTrip_ = std::move(content.roundTrip_);
    schemaMinor_ = content.schemaMinor_;
    retainedRequirements_ = std::move(content.requirements_);
    contentKind_ = ProjectSessionContentKind::DecodedDocument;
    editability_ = content.editability_;
    displayPath_ = std::move(content.displayPath_);
    decodedContentReservations_ = std::move(content.reservations_);
    cleanRevision_ = commandStack_->trackedRevision();

    return SessionInstallStatus::Installed;
}

SessionInstallStatus
ProjectSession::installPreservedReadOnlyReplacement(const OpenIntentCapture intent,
                                                    ProjectDisplayPath displayPath) {
    if (const auto gate = checkInstallAcceptanceGates(intent)) {
        return *gate;
    }
    if (resultAcceptanceGeneration_.value() == std::numeric_limits<std::uint64_t>::max() ||
        pathIntentGeneration_.value() == std::numeric_limits<std::uint64_t>::max()) {
        return SessionInstallStatus::RuntimeIdentityExhausted;
    }

    // No allocating step in this variant (releasing document_/commandStack_/etc. and moving
    // displayPath cannot throw), so the strong exception guarantee is trivially satisfied by the
    // mutation order alone -- there is nothing that can throw partway through.
    const auto advanceStatus = advanceResultAcceptanceForInstalledReplacement();
    if (advanceStatus != SessionResultAcceptanceAdvanceStatus::Advanced) {
        return SessionInstallStatus::RuntimeIdentityExhausted; // unreachable; see the sibling
                                                               // method
    }
    pathIntentGeneration_ = SessionPathIntentGeneration::fromRaw(pathIntentGeneration_.value() + 1);

    // Mirrors createPreservedReadOnly()'s internal state exactly.
    document_.reset();
    commandStack_.reset();
    colorSettings_.reset();
    roundTrip_.reset();
    schemaMinor_ = 0;
    retainedRequirements_.clear();
    cleanRevision_.reset();
    editability_.reset();
    decodedContentReservations_.reset();
    contentKind_ = ProjectSessionContentKind::PreservedReadOnly;
    displayPath_ = std::move(displayPath);

    return SessionInstallStatus::Installed;
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

SessionSaveInputResult
ProjectSession::captureSaveInput(const SessionPathIntentCapture intent) const {
    if (!isValid()) {
        return SessionSaveInputResult(SessionSaveInputStatus::InvalidSession);
    }
    if (contentKind_ != ProjectSessionContentKind::DecodedDocument) {
        return SessionSaveInputResult(SessionSaveInputStatus::ReadOnly);
    }
    if (!colorSettings_.has_value()) {
        return SessionSaveInputResult(SessionSaveInputStatus::ColorSettingsUnavailable);
    }
    // Mirrors acceptSavepoint()'s own ExistingPath/no-displayPath check, but reported first and
    // distinctly from StaleIntent: this is the ordinary pathless-plain-save shape (a
    // capturePlainSavePathIntent() call on a pathless session always returns this exact invalid
    // capture), not a generation mismatch, so callers get an actionable "route to Save As"
    // diagnosis rather than a generic staleness report.
    if (intent.kind() == SessionPathIntentKind::ExistingPath && !displayPath_.has_value()) {
        return SessionSaveInputResult(SessionSaveInputStatus::PathRequired);
    }
    if (!matchesPathIntent(intent)) {
        return SessionSaveInputResult(SessionSaveInputStatus::StaleIntent);
    }
    return SessionSaveInputResult(SessionSaveInput(
        document_->snapshot(), *colorSettings_, roundTrip_.has_value() ? &*roundTrip_ : nullptr,
        schemaMinor_, retainedRequirements_, displayPath_, intent, captureResultAcceptance()));
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
