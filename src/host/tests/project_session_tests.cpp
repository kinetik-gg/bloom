#include <bloom/host/project_session.hpp>

#include <bloom/commands/operations.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/round_trip_state.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace bloom::host {

class ProjectSessionTestAccess final {
  public:
    [[nodiscard]] static bool setLastIssuedSessionId(ProjectSessionIdentitySource& source,
                                                     const std::uint64_t value) noexcept {
        return source.setLastIssuedSessionIdForTesting(value);
    }

    [[nodiscard]] static bool setGenerations(ProjectSession& session,
                                             const std::uint64_t resultAcceptance,
                                             const std::uint64_t openIntent,
                                             const std::uint64_t pathIntent) noexcept {
        return session.setGenerationsForTesting(
            SessionResultAcceptanceGeneration::fromRaw(resultAcceptance),
            OpenIntentGeneration::fromRaw(openIntent),
            SessionPathIntentGeneration::fromRaw(pathIntent));
    }

    [[nodiscard]] static SessionResultAcceptanceAdvanceStatus
    advanceResultAcceptance(ProjectSession& session) noexcept {
        return session.advanceResultAcceptanceForInstalledReplacement();
    }
};

} // namespace bloom::host

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

using bloom::commands::CommandStatus;
using bloom::commands::SetCompositionName;
using bloom::commands::SetProjectName;
using bloom::commands::Transaction;
using bloom::host::DecodedProjectEditability;
using bloom::host::DecodedProjectSessionRequest;
using bloom::host::DecodedProjectSnapshotStatus;
using bloom::host::NewProjectSessionRequest;
using bloom::host::OpenIntentAdmissionStatus;
using bloom::host::OpenIntentCapture;
using bloom::host::ProjectDisplayPath;
using bloom::host::ProjectSession;
using bloom::host::ProjectSessionCommandStatus;
using bloom::host::ProjectSessionContentKind;
using bloom::host::ProjectSessionCreateStatus;
using bloom::host::ProjectSessionIdentitySource;
using bloom::host::ProjectSessionSavepointStatus;
using bloom::host::PublicationIntentId;
using bloom::host::SessionPathIntentAbandonStatus;
using bloom::host::SessionPathIntentAdvanceStatus;
using bloom::host::SessionPathIntentKind;
using bloom::host::SessionResultAcceptanceAdvanceStatus;
using bloom::host::SessionSaveInputStatus;

static_assert(!std::is_copy_constructible_v<ProjectSessionIdentitySource>);
static_assert(!std::is_move_constructible_v<ProjectSessionIdentitySource>);
static_assert(std::is_nothrow_default_constructible_v<OpenIntentCapture>);
static_assert(!OpenIntentCapture{}.isValid());
static_assert(noexcept(std::declval<const OpenIntentCapture&>().contentKind()));
static_assert(noexcept(std::declval<const OpenIntentCapture&>().decodedRevision()));
static_assert(
    !std::is_constructible_v<OpenIntentCapture, bloom::host::SessionResultAcceptanceCapture,
                             bloom::host::OpenIntentGeneration, ProjectSessionContentKind,
                             std::optional<bloom::document::Revision>>);

[[nodiscard]] CommandStatus commandStatus(const bloom::host::ProjectSessionCommandResult& result) {
    if (!result.command.has_value()) {
        throw std::logic_error("Expected a forwarded command result");
    }
    return result.command->status;
}

[[nodiscard]] bloom::document::ColorSettings neutralColorSettings() {
    std::array<std::uint8_t, 32> digestBytes{};
    std::iota(digestBytes.begin(), digestBytes.end(), std::uint8_t{0});
    return bloom::document::makeBloomNeutralColorSettingsV1(
        bloom::core::Sha256Digest::fromBytes(digestBytes));
}

[[nodiscard]] NewProjectSessionRequest newProjectRequest() {
    return {
        .projectName = "Project",
        .compositionName = "Main",
        .duration = bloom::core::RationalTime::fromInteger(10),
        .format = {},
    };
}

[[nodiscard]] ProjectSession newSession(Expectations& expectations,
                                        ProjectSessionIdentitySource& identitySource) {
    auto result = ProjectSession::createNew(identitySource, newProjectRequest());
    expectations.expect(static_cast<bool>(result), "the new-project fixture must be valid");
    if (!result) {
        throw std::logic_error("Could not create project-session fixture");
    }
    return std::move(result).takeSession();
}

[[nodiscard]] ProjectSession
decodedSessionWithPath(Expectations& expectations, ProjectSessionIdentitySource& identitySource,
                       const std::filesystem::path& displayPath = "project.bloom") {
    auto created = bloom::document::makeNewProject("Project", "Main",
                                                   bloom::core::RationalTime::fromInteger(10));
    auto path = ProjectDisplayPath::create(displayPath);
    if (!path.has_value()) {
        throw std::logic_error("Could not create project-session path fixture");
    }
    auto result = ProjectSession::createDecoded(identitySource,
                                                {.project = std::move(created.project),
                                                 .colorSettings = neutralColorSettings(),
                                                 .editability = DecodedProjectEditability::Editable,
                                                 .displayPath = std::move(path),
                                                 .persistedAllocatorHighWater = std::nullopt});
    expectations.expect(static_cast<bool>(result), "the decoded path fixture must be valid");
    if (!result) {
        throw std::logic_error("Could not create decoded project-session fixture");
    }
    return std::move(result).takeSession();
}

[[nodiscard]] constexpr PublicationIntentId publicationIntent(const std::uint64_t value) noexcept {
    return PublicationIntentId::fromRaw(value);
}

[[nodiscard]] std::string projectName(const ProjectSession& session) {
    const auto snapshot = session.decodedSnapshot();
    if (!snapshot) {
        throw std::logic_error("Expected a decoded document");
    }
    return snapshot.snapshot().project().name();
}

[[nodiscard]] bloom::document::Revision currentRevision(const ProjectSession& session) {
    const auto state = session.stateSnapshot();
    if (!state.currentRevision.has_value()) {
        throw std::logic_error("Expected a decoded revision");
    }
    return *state.currentRevision;
}

[[nodiscard]] Transaction rename(std::string value, const ProjectSession& session,
                                 std::string label = "Rename") {
    Transaction transaction(std::move(label), currentRevision(session));
    transaction.emplace<SetProjectName>(std::move(value));
    return transaction;
}

void testNewProjectBaselineAndSnapshots(Expectations& expectations,
                                        ProjectSessionIdentitySource& identitySource) {
    auto session = newSession(expectations, identitySource);
    const auto state = session.stateSnapshot();
    expectations.expect(session.isValid() && state.valid,
                        "a new session owns one coherent valid state");
    expectations.expect(
        state.projectSessionId.isValid() && state.resultAcceptanceGeneration.value() == 1 &&
            state.openIntentGeneration.value() == 1 && state.pathIntentGeneration.value() == 1 &&
            state.pathIntentKind == SessionPathIntentKind::ExistingPath &&
            !state.newestAcceptedPublicationIntent.isValid(),
        "a new runtime session starts with one identity and three generation ones");
    expectations.expect(state.contentKind == ProjectSessionContentKind::DecodedDocument &&
                            state.editability == DecodedProjectEditability::Editable,
                        "a new project is decoded and fully editable");
    expectations.expect(!state.displayPath.has_value() && state.currentRevision.has_value() &&
                            state.currentRevision->value() == 0 &&
                            state.cleanRevision == state.currentRevision && state.dirty == false,
                        "a pathless new project starts at its exact clean baseline");
    expectations.expect(!state.canUndo && !state.canRedo && state.historySize == 0 &&
                            !state.undoLabel.has_value() && !state.redoLabel.has_value(),
                        "a new project starts with empty command history");

    const auto snapshot = session.decodedSnapshot();
    expectations.expect(snapshot && snapshot.snapshot().project().name() == "Project" &&
                            snapshot.snapshot().project().compositions().size() == 1,
                        "decoded snapshot access returns immutable project truth");
}

void testIdentitySourceAndExactExhaustion(Expectations& expectations) {
    ProjectSessionIdentitySource identitySource;
    auto firstResult = ProjectSession::createNew(identitySource, newProjectRequest());
    auto decodedProject = bloom::document::makeNewProject(
        "Decoded identity", "Main", bloom::core::RationalTime::fromInteger(5));
    auto secondResult = ProjectSession::createDecoded(
        identitySource, {.project = std::move(decodedProject.project),
                         .colorSettings = neutralColorSettings(),
                         .editability = DecodedProjectEditability::Editable,
                         .displayPath = std::nullopt,
                         .persistedAllocatorHighWater = std::nullopt});
    auto thirdResult =
        ProjectSession::createPreservedReadOnly(identitySource, "preserved-identity.bloom");
    expectations.expect(firstResult && secondResult && thirdResult,
                        "one application identity source creates every session content kind");
    if (!firstResult || !secondResult || !thirdResult) {
        return;
    }
    auto first = std::move(firstResult).takeSession();
    auto second = std::move(secondResult).takeSession();
    auto third = std::move(thirdResult).takeSession();
    const auto firstState = first.stateSnapshot();
    const auto secondState = second.stateSnapshot();
    const auto thirdState = third.stateSnapshot();
    expectations.expect(
        firstState.projectSessionId.value() == 1 && secondState.projectSessionId.value() == 2 &&
            thirdState.projectSessionId.value() == 3 &&
            firstState.projectSessionId != secondState.projectSessionId &&
            secondState.projectSessionId != thirdState.projectSessionId,
        "the explicit application source issues fresh monotonic process-local identities");
    expectations.expect(
        firstState.resultAcceptanceGeneration.value() == 1 &&
            secondState.resultAcceptanceGeneration.value() == 1 &&
            thirdState.resultAcceptanceGeneration.value() == 1 &&
            firstState.openIntentGeneration.value() == 1 &&
            secondState.openIntentGeneration.value() == 1 &&
            thirdState.openIntentGeneration.value() == 1 &&
            firstState.pathIntentGeneration.value() == 1 &&
            secondState.pathIntentGeneration.value() == 1 &&
            thirdState.pathIntentGeneration.value() == 1,
        "each independently created session initializes all three generations to one");

    ProjectSessionIdentitySource invalidSource;
    auto invalidRequest = newProjectRequest();
    invalidRequest.projectName.clear();
    const auto invalid = ProjectSession::createNew(invalidSource, std::move(invalidRequest));
    expectations.expect(
        invalid.status() == ProjectSessionCreateStatus::InvalidNewProject &&
            invalidSource.snapshot().lastIssuedSessionId.value() == 0,
        "validation completes before identity issue and cannot consume an ID on failure");

    ProjectSessionIdentitySource boundarySource;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    expectations.expect(
        bloom::host::ProjectSessionTestAccess::setLastIssuedSessionId(boundarySource, maximum - 1),
        "the identity boundary fixture reaches the final issuable value");
    auto finalResult =
        ProjectSession::createPreservedReadOnly(boundarySource, "final-session.bloom");
    expectations.expect(static_cast<bool>(finalResult),
                        "the final uint64 session identity remains issuable exactly once");
    if (!finalResult) {
        return;
    }
    auto finalSession = std::move(finalResult).takeSession();
    const auto finalState = finalSession.stateSnapshot();
    const auto exhausted =
        ProjectSession::createPreservedReadOnly(boundarySource, "wrapped-session.bloom");
    const auto boundarySnapshot = boundarySource.snapshot();
    expectations.expect(
        finalState.projectSessionId.value() == maximum &&
            exhausted.status() == ProjectSessionCreateStatus::RuntimeIdentityExhausted &&
            boundarySnapshot.lastIssuedSessionId.value() == maximum &&
            boundarySnapshot.identityExhausted,
        "session identity exhaustion is typed and never wraps or reuses the final value");
}

void testGenerationCapturesAndSubsetPredicates(Expectations& expectations) {
    ProjectSessionIdentitySource identitySource;
    auto session = decodedSessionWithPath(expectations, identitySource);
    const auto resultCapture = session.captureResultAcceptance();
    const auto plainSave = session.capturePlainSavePathIntent();
    const auto firstOpen = session.admitOpenIntent();
    const auto firstSaveAs = session.advancePathIntentForSaveAs();
    const auto plainWhileReplacementPending = session.capturePlainSavePathIntent();
    expectations.expect(
        resultCapture.isValid() && plainSave.isValid() &&
            plainSave.kind() == SessionPathIntentKind::ExistingPath && firstOpen && firstSaveAs &&
            firstOpen.capture().generation().value() == 2 &&
            firstOpen.capture().contentKind() == ProjectSessionContentKind::DecodedDocument &&
            firstOpen.capture().decodedRevision() == currentRevision(session) &&
            firstSaveAs.capture().generation().value() == 2 &&
            firstSaveAs.capture().kind() == SessionPathIntentKind::ReplacementPath &&
            !plainWhileReplacementPending.isValid() &&
            session.stateSnapshot().pathIntentKind == SessionPathIntentKind::ReplacementPath,
        "the first Open and Save As advance their independent generations to two");
    expectations.expect(session.matchesResultAcceptance(resultCapture) &&
                            session.isDesiredOpenIntent(firstOpen.capture()) &&
                            !session.matchesPathIntent(plainSave) &&
                            session.matchesPathIntent(firstSaveAs.capture()),
                        "Save As invalidates only the older path-intent capture");
    const auto stalePath = ProjectDisplayPath::create("stale-save-as.bloom");
    const auto staleSavepoint = session.acceptSavepoint(plainSave, publicationIntent(1),
                                                        currentRevision(session), stalePath);
    const auto afterStaleSavepoint = session.stateSnapshot();
    expectations.expect(
        stalePath.has_value() && staleSavepoint == ProjectSessionSavepointStatus::StaleIntent &&
            afterStaleSavepoint.displayPath.has_value() &&
            afterStaleSavepoint.displayPath->value() == std::filesystem::path("project.bloom"),
        "savepoint mutation consumes and rejects a stale session-path capture itself");

    const auto currentPath = firstSaveAs.capture();
    const auto secondOpen = session.admitOpenIntent();
    expectations.expect(
        secondOpen && secondOpen.capture().generation().value() == 3 &&
            !session.isDesiredOpenIntent(firstOpen.capture()) &&
            session.isDesiredOpenIntent(secondOpen.capture()) &&
            session.matchesPathIntent(currentPath),
        "a newer Open invalidates only Open intent and leaves Save acceptance valid");

    auto otherSession = decodedSessionWithPath(expectations, identitySource, "other.bloom");
    const auto otherResult = otherSession.captureResultAcceptance();
    const auto otherOpen = otherSession.admitOpenIntent();
    const auto otherPath = otherSession.capturePlainSavePathIntent();
    expectations.expect(otherOpen && !session.matchesResultAcceptance(otherResult) &&
                            !session.isDesiredOpenIntent(otherOpen.capture()) &&
                            !session.matchesPathIntent(otherPath),
                        "equal generation numbers from a different runtime session never match");

    const auto beforeInstalledReplacement = session.stateSnapshot();
    const auto advanced = bloom::host::ProjectSessionTestAccess::advanceResultAcceptance(session);
    const auto afterInstalledReplacement = session.stateSnapshot();
    expectations.expect(
        advanced == SessionResultAcceptanceAdvanceStatus::Advanced &&
            !session.matchesResultAcceptance(resultCapture) &&
            !session.isDesiredOpenIntent(secondOpen.capture()) &&
            !session.matchesPathIntent(currentPath) &&
            session.abandonSaveAsIntent(currentPath) ==
                SessionPathIntentAbandonStatus::StaleIntent &&
            afterInstalledReplacement.pathIntentKind == SessionPathIntentKind::ExistingPath &&
            afterInstalledReplacement.pathIntentGeneration ==
                beforeInstalledReplacement.pathIntentGeneration &&
            afterInstalledReplacement.displayPath == beforeInstalledReplacement.displayPath &&
            afterInstalledReplacement.cleanRevision == beforeInstalledReplacement.cleanRevision &&
            session.capturePlainSavePathIntent().isValid(),
        "installed-content replacement cancels pending path authority and invalidates old "
        "captures");
}

void testOpenIntentBindsExactContent(Expectations& expectations) {
    ProjectSessionIdentitySource identitySource;
    auto session = newSession(expectations, identitySource);
    const auto initialAdmission = session.admitOpenIntent();
    if (!initialAdmission) {
        expectations.expect(false, "the decoded Open binding fixture admits its initial intent");
        return;
    }
    const auto initial = initialAdmission.capture();
    expectations.expect(initial.contentKind() == ProjectSessionContentKind::DecodedDocument &&
                            initial.decodedRevision() == currentRevision(session) &&
                            session.isDesiredOpenIntent(initial),
                        "a decoded Open intent captures its exact command-stack revision");

    Transaction noChange("No-op while Open", currentRevision(session));
    noChange.emplace<SetProjectName>("Project");
    const auto noChangeResult = session.execute(std::move(noChange));
    expectations.expect(noChangeResult.completed() &&
                            commandStatus(noChangeResult) == CommandStatus::NoChange &&
                            session.isDesiredOpenIntent(initial) &&
                            initial.decodedRevision() == currentRevision(session),
                        "an idempotent edit leaves the exact decoded Open binding desired");

    Transaction rejected("Rejected while Open", currentRevision(session));
    rejected.emplace<SetCompositionName>(bloom::document::CompositionId::fromRaw(999), "Missing");
    const auto rejectedResult = session.execute(std::move(rejected));
    expectations.expect(rejectedResult.completed() &&
                            commandStatus(rejectedResult) == CommandStatus::Rejected &&
                            session.isDesiredOpenIntent(initial) &&
                            initial.decodedRevision() == currentRevision(session),
                        "a rejected edit leaves the exact decoded Open binding desired");

    const auto successful = session.execute(rename("Changed while Open", session));
    expectations.expect(successful.changed() && !session.isDesiredOpenIntent(initial) &&
                            initial.decodedRevision().has_value() &&
                            *initial.decodedRevision() < currentRevision(session),
                        "a successful edit invalidates the captured decoded Open content");

    const auto editedAdmission = session.admitOpenIntent();
    if (!editedAdmission) {
        expectations.expect(false, "the edited Open binding fixture admits a new intent");
        return;
    }
    const auto edited = editedAdmission.capture();
    const auto editedRevision = currentRevision(session);
    expectations.expect(edited.decodedRevision() == editedRevision &&
                            session.isDesiredOpenIntent(edited),
                        "a newer Open binds to the newly edited revision");

    const auto undoResult = session.undo();
    const auto undoRevision = currentRevision(session);
    expectations.expect(
        undoResult.changed() && projectName(session) == "Project" &&
            undoRevision > editedRevision && !session.isDesiredOpenIntent(edited),
        "undo restores artist content with a newer revision and cannot revive an older Open");

    const auto undoneAdmission = session.admitOpenIntent();
    if (!undoneAdmission) {
        expectations.expect(false, "the undone Open binding fixture admits a new intent");
        return;
    }
    const auto undone = undoneAdmission.capture();
    const auto redoResult = session.redo();
    expectations.expect(
        undone.decodedRevision() == undoRevision && redoResult.changed() &&
            currentRevision(session) > undoRevision && !session.isDesiredOpenIntent(undone),
        "redo advances monotonically and invalidates an intent captured after undo");

    auto other = newSession(expectations, identitySource);
    const auto otherOpen = other.admitOpenIntent();
    expectations.expect(otherOpen && !session.isDesiredOpenIntent(otherOpen.capture()) &&
                            !other.isDesiredOpenIntent(undone),
                        "content bindings never cross runtime project sessions");
}

void testGenerationExhaustionBoundaries(Expectations& expectations) {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();

    ProjectSessionIdentitySource acceptanceSource;
    auto acceptanceSession =
        decodedSessionWithPath(expectations, acceptanceSource, "maximum-acceptance.bloom");
    expectations.expect(
        bloom::host::ProjectSessionTestAccess::setGenerations(acceptanceSession, maximum - 1, 5, 7),
        "the acceptance fixture reaches its penultimate value");
    expectations.expect(
        acceptanceSession.acceptSavepoint(
            acceptanceSession.capturePlainSavePathIntent(), publicationIntent(49),
            currentRevision(acceptanceSession)) == ProjectSessionSavepointStatus::Accepted,
        "the acceptance boundary fixture has a current-generation publication frontier");
    const auto acceptanceAdvanced =
        bloom::host::ProjectSessionTestAccess::advanceResultAcceptance(acceptanceSession);
    const auto afterAcceptanceAdvanced = acceptanceSession.stateSnapshot();
    const auto acceptanceFinal = acceptanceSession.captureResultAcceptance();
    expectations.expect(
        acceptanceSession.acceptSavepoint(
            acceptanceSession.capturePlainSavePathIntent(), publicationIntent(50),
            currentRevision(acceptanceSession)) == ProjectSessionSavepointStatus::Accepted,
        "the final acceptance generation can establish its own publication frontier");
    const auto beforeAcceptanceExhausted = acceptanceSession.stateSnapshot();
    const auto acceptanceExhausted =
        bloom::host::ProjectSessionTestAccess::advanceResultAcceptance(acceptanceSession);
    const auto afterAcceptanceExhausted = acceptanceSession.stateSnapshot();
    expectations.expect(
        acceptanceAdvanced == SessionResultAcceptanceAdvanceStatus::Advanced &&
            !afterAcceptanceAdvanced.newestAcceptedPublicationIntent.isValid() &&
            acceptanceFinal.generation().value() == maximum && acceptanceFinal.isValid() &&
            acceptanceExhausted == SessionResultAcceptanceAdvanceStatus::RuntimeIdentityExhausted &&
            acceptanceSession.captureResultAcceptance() == acceptanceFinal &&
            beforeAcceptanceExhausted.newestAcceptedPublicationIntent == publicationIntent(50) &&
            afterAcceptanceExhausted.newestAcceptedPublicationIntent == publicationIntent(50) &&
            afterAcceptanceExhausted.pathIntentKind == beforeAcceptanceExhausted.pathIntentKind,
        "result-acceptance advance resets the scoped frontier while exhaustion preserves it");

    const auto beforeAcceptanceBlockedOpen = acceptanceSession.stateSnapshot();
    const auto acceptanceBlockedOpen = acceptanceSession.admitOpenIntent();
    const auto afterAcceptanceBlockedOpen = acceptanceSession.stateSnapshot();
    expectations.expect(
        acceptanceBlockedOpen.status() == OpenIntentAdmissionStatus::RuntimeIdentityExhausted &&
            afterAcceptanceBlockedOpen.resultAcceptanceGeneration ==
                beforeAcceptanceBlockedOpen.resultAcceptanceGeneration &&
            afterAcceptanceBlockedOpen.openIntentGeneration ==
                beforeAcceptanceBlockedOpen.openIntentGeneration &&
            afterAcceptanceBlockedOpen.pathIntentGeneration ==
                beforeAcceptanceBlockedOpen.pathIntentGeneration,
        "Open admission with exhausted acceptance headroom atomically changes no generation");

    ProjectSessionIdentitySource openSource;
    auto openSession = newSession(expectations, openSource);
    expectations.expect(
        bloom::host::ProjectSessionTestAccess::setGenerations(openSession, 1, maximum - 1, 3),
        "the Open fixture reaches its penultimate value");
    const auto finalOpen = openSession.admitOpenIntent();
    const auto beforeOpenExhaustion = openSession.stateSnapshot();
    const auto exhaustedOpen = openSession.admitOpenIntent();
    const auto afterOpenExhaustion = openSession.stateSnapshot();
    expectations.expect(
        finalOpen && finalOpen.capture().generation().value() == maximum &&
            finalOpen.capture().contentKind() == ProjectSessionContentKind::DecodedDocument &&
            finalOpen.capture().decodedRevision() == currentRevision(openSession) &&
            exhaustedOpen.status() == OpenIntentAdmissionStatus::RuntimeIdentityExhausted &&
            openSession.isDesiredOpenIntent(finalOpen.capture()) &&
            afterOpenExhaustion.resultAcceptanceGeneration ==
                beforeOpenExhaustion.resultAcceptanceGeneration &&
            afterOpenExhaustion.openIntentGeneration == beforeOpenExhaustion.openIntentGeneration &&
            afterOpenExhaustion.pathIntentGeneration == beforeOpenExhaustion.pathIntentGeneration,
        "Open generation issues UINT64_MAX once and rejects the next admission without mutation");

    ProjectSessionIdentitySource pathSource;
    auto pathSession = decodedSessionWithPath(expectations, pathSource, "maximum-path.bloom");
    expectations.expect(
        bloom::host::ProjectSessionTestAccess::setGenerations(pathSession, 1, 4, maximum - 1),
        "the path fixture reaches its penultimate value");
    expectations.expect(pathSession.acceptSavepoint(pathSession.capturePlainSavePathIntent(),
                                                    publicationIntent(59),
                                                    currentRevision(pathSession)) ==
                            ProjectSessionSavepointStatus::Accepted,
                        "the path boundary fixture has an existing-path publication frontier");
    const auto finalPath = pathSession.advancePathIntentForSaveAs();
    const auto afterFinalPathAdvance = pathSession.stateSnapshot();
    const auto plainWhileFinalReplacementPending = pathSession.capturePlainSavePathIntent();
    const auto beforeReplacementPathExhaustion = pathSession.stateSnapshot();
    const auto exhaustedReplacementPath = pathSession.advancePathIntentForSaveAs();
    const auto afterReplacementPathExhaustion = pathSession.stateSnapshot();
    const auto replacementPath = ProjectDisplayPath::create("maximum-replacement.bloom");
    expectations.expect(replacementPath.has_value() && finalPath &&
                            pathSession.acceptSavepoint(finalPath.capture(), publicationIntent(60),
                                                        currentRevision(pathSession),
                                                        replacementPath) ==
                                ProjectSessionSavepointStatus::Accepted,
                        "the final replacement generation can install its resolved path");
    const auto plainAtMaximum = pathSession.capturePlainSavePathIntent();
    const auto beforeExistingPathExhaustion = pathSession.stateSnapshot();
    const auto exhaustedExistingPath = pathSession.advancePathIntentForSaveAs();
    const auto afterExistingPathExhaustion = pathSession.stateSnapshot();
    expectations.expect(
        finalPath && finalPath.capture().generation().value() == maximum &&
            finalPath.capture().kind() == SessionPathIntentKind::ReplacementPath &&
            afterFinalPathAdvance.pathIntentKind == SessionPathIntentKind::ReplacementPath &&
            !afterFinalPathAdvance.newestAcceptedPublicationIntent.isValid() &&
            !plainWhileFinalReplacementPending.isValid() &&
            exhaustedReplacementPath.status() ==
                SessionPathIntentAdvanceStatus::RuntimeIdentityExhausted &&
            afterReplacementPathExhaustion.pathIntentKind ==
                beforeReplacementPathExhaustion.pathIntentKind &&
            afterReplacementPathExhaustion.newestAcceptedPublicationIntent ==
                beforeReplacementPathExhaustion.newestAcceptedPublicationIntent &&
            plainAtMaximum.isValid() &&
            plainAtMaximum.kind() == SessionPathIntentKind::ExistingPath &&
            pathSession.matchesPathIntent(plainAtMaximum) &&
            exhaustedExistingPath.status() ==
                SessionPathIntentAdvanceStatus::RuntimeIdentityExhausted &&
            afterExistingPathExhaustion.pathIntentKind == SessionPathIntentKind::ExistingPath &&
            afterExistingPathExhaustion.newestAcceptedPublicationIntent == publicationIntent(60) &&
            afterExistingPathExhaustion.resultAcceptanceGeneration ==
                beforeExistingPathExhaustion.resultAcceptanceGeneration &&
            afterExistingPathExhaustion.openIntentGeneration ==
                beforeExistingPathExhaustion.openIntentGeneration &&
            afterExistingPathExhaustion.pathIntentGeneration ==
                beforeExistingPathExhaustion.pathIntentGeneration,
        "path exhaustion preserves both replacement/existing phase and the scoped frontier");
}

void testDirtySavepointBranching(Expectations& expectations,
                                 ProjectSessionIdentitySource& identitySource) {
    auto session = newSession(expectations, identitySource);
    const auto originalResult = session.decodedSnapshot();
    const auto& original = originalResult.snapshot();

    const auto first = session.execute(rename("First", session, "First edit"));
    expectations.expect(first.completed() && first.changed() &&
                            commandStatus(first) == CommandStatus::Succeeded,
                        "an editable session forwards a transaction to its command stack");
    const auto firstRevision = currentRevision(session);
    expectations.expect(firstRevision.value() == 1 && session.stateSnapshot().dirty == true,
                        "a committed edit advances revision and becomes dirty");
    expectations.expect(original.project().name() == "Project",
                        "a previously returned document snapshot remains immutable");

    const auto pathlessPlainSave = session.capturePlainSavePathIntent();
    const auto saveAs = session.advancePathIntentForSaveAs();
    expectations.expect(!pathlessPlainSave.isValid() && saveAs &&
                            saveAs.capture().kind() == SessionPathIntentKind::ReplacementPath,
                        "a pathless session can only establish its first path through Save As");
    if (!saveAs) {
        return;
    }
    const auto missingPath =
        session.acceptSavepoint(saveAs.capture(), publicationIntent(1), firstRevision);
    const auto missingPathState = session.stateSnapshot();
    expectations.expect(
        missingPath == ProjectSessionSavepointStatus::PathRequired &&
            missingPathState.cleanRevision.value_or(bloom::document::Revision::fromRaw(99))
                    .value() == 0,
        "a pathless project cannot establish a published savepoint without a path");
    const auto path = ProjectDisplayPath::create(std::filesystem::path("project.bloom"));
    expectations.expect(path.has_value(), "the savepoint fixture path is valid");
    if (!path.has_value()) {
        return;
    }
    expectations.expect(session.acceptSavepoint(saveAs.capture(), publicationIntent(1),
                                                firstRevision,
                                                path) == ProjectSessionSavepointStatus::Accepted &&
                            session.stateSnapshot().dirty == false,
                        "an accepted publication records its exact clean revision and path");

    expectations.expect(session.execute(rename("Second", session, "Second edit")).changed(),
                        "a second edit commits");
    const auto secondRevision = currentRevision(session);
    expectations.expect(secondRevision.value() == 2 && session.stateSnapshot().dirty == true,
                        "editing after a savepoint is dirty");
    expectations.expect(session.acceptSavepoint(session.capturePlainSavePathIntent(),
                                                publicationIntent(2), firstRevision) ==
                                ProjectSessionSavepointStatus::Accepted &&
                            session.stateSnapshot().dirty == true,
                        "a late older publication keeps the newer live document dirty");

    const auto undo = session.undo();
    const auto afterUndo = session.stateSnapshot();
    expectations.expect(undo.changed() && currentRevision(session).value() == 3 &&
                            projectName(session) == "First" && afterUndo.dirty == true,
                        "undo restores content at a new revision and stays conservatively dirty");
    expectations.expect(afterUndo.canRedo && afterUndo.redoLabel == "Second edit",
                        "undo exposes an owned redo label");

    const auto beforeBranch = session.stateSnapshot();
    expectations.expect(session.execute(rename("Branched", session, "Branch edit")).changed(),
                        "a branch edit after undo commits");
    const auto branched = session.stateSnapshot();
    expectations.expect(!branched.canRedo && branched.historySize == 2 &&
                            beforeBranch.redoLabel == "Second edit",
                        "a branch discards redo while earlier state snapshots stay self-contained");
    expectations.expect(session.acceptSavepoint(session.capturePlainSavePathIntent(),
                                                publicationIntent(3), currentRevision(session)) ==
                                ProjectSessionSavepointStatus::Accepted &&
                            session.stateSnapshot().dirty == false,
                        "plain-save acceptance retains the established path");

    const auto savedBranchRevision = currentRevision(session);
    expectations.expect(session.undo().changed() && session.stateSnapshot().dirty == true,
                        "undo after savepoint publishes a distinct dirty revision");
    expectations.expect(session.redo().changed() && projectName(session) == "Branched" &&
                            session.stateSnapshot().dirty == true &&
                            currentRevision(session) > savedBranchRevision,
                        "redoing saved content still has a new conservative dirty revision");

    const auto future = bloom::document::Revision::fromRaw(currentRevision(session).value() + 1);
    const auto beforeFuture = session.stateSnapshot();
    expectations.expect(
        session.acceptSavepoint(session.capturePlainSavePathIntent(), publicationIntent(4),
                                future) == ProjectSessionSavepointStatus::UnknownRevision &&
            session.stateSnapshot().cleanRevision == beforeFuture.cleanRevision &&
            session.stateSnapshot().newestAcceptedPublicationIntent ==
                beforeFuture.newestAcceptedPublicationIntent,
        "an unobserved future revision cannot move the savepoint or publication frontier");
}

void testSavepointPathAuthority(Expectations& expectations) {
    ProjectSessionIdentitySource identitySource;
    auto session = decodedSessionWithPath(expectations, identitySource, "existing.bloom");
    expectations.expect(session.execute(rename("Edited", session)).changed(),
                        "the path-authority fixture becomes dirty");
    const auto revision = currentRevision(session);
    const auto plainSave = session.capturePlainSavePathIntent();
    const auto replacement = ProjectDisplayPath::create("replacement.bloom");
    expectations.expect(plainSave.isValid() &&
                            plainSave.kind() == SessionPathIntentKind::ExistingPath &&
                            replacement.has_value(),
                        "plain Save captures only existing-path authority");
    if (!replacement.has_value()) {
        return;
    }

    const auto beforeRejected = session.stateSnapshot();
    expectations.expect(
        session.acceptSavepoint(plainSave, PublicationIntentId{}, revision) ==
                ProjectSessionSavepointStatus::InvalidPublicationIntent &&
            session.acceptSavepoint(plainSave, publicationIntent(1), revision, replacement) ==
                ProjectSessionSavepointStatus::PathAuthorityMismatch,
        "invalid publication identity and plain-Save path replacement are rejected explicitly");
    const auto afterRejected = session.stateSnapshot();
    expectations.expect(
        afterRejected.displayPath == beforeRejected.displayPath &&
            afterRejected.cleanRevision == beforeRejected.cleanRevision &&
            afterRejected.dirty == true && !afterRejected.newestAcceptedPublicationIntent.isValid(),
        "misusing a plain token cannot change path, clean state, or publication frontier");

    expectations.expect(session.acceptSavepoint(plainSave, publicationIntent(1), revision) ==
                                ProjectSessionSavepointStatus::Accepted &&
                            session.stateSnapshot().dirty == false &&
                            session.stateSnapshot().newestAcceptedPublicationIntent ==
                                publicationIntent(1),
                        "a rejected callback does not consume its publication identity");

    const auto saveAs = session.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAs), "the replacement-path fixture admits Save As");
    if (!saveAs) {
        return;
    }
    const auto beforeMissingReplacement = session.stateSnapshot();
    const auto plainWhileReplacementPending = session.capturePlainSavePathIntent();
    expectations.expect(
        beforeMissingReplacement.pathIntentKind == SessionPathIntentKind::ReplacementPath &&
            !beforeMissingReplacement.newestAcceptedPublicationIntent.isValid() &&
            !plainWhileReplacementPending.isValid() &&
            session.acceptSavepoint(saveAs.capture(), publicationIntent(2), revision) ==
                ProjectSessionSavepointStatus::PathRequired &&
            session.stateSnapshot().newestAcceptedPublicationIntent ==
                beforeMissingReplacement.newestAcceptedPublicationIntent &&
            session.acceptSavepoint(saveAs.capture(), publicationIntent(2), revision,
                                    replacement) == ProjectSessionSavepointStatus::Accepted &&
            session.stateSnapshot().displayPath == replacement &&
            session.stateSnapshot().pathIntentKind == SessionPathIntentKind::ExistingPath &&
            session.capturePlainSavePathIntent().isValid() &&
            session.stateSnapshot().newestAcceptedPublicationIntent == publicationIntent(2),
        "Save As requires a published replacement path and advances only after acceptance");
}

void testPublicationCallbackOrdering(Expectations& expectations) {
    ProjectSessionIdentitySource identitySource;
    auto session = decodedSessionWithPath(expectations, identitySource, "ordered.bloom");

    expectations.expect(session.execute(rename("R1", session, "R1")).changed(),
                        "the first same-path publication captures revision one");
    const auto revisionOne = currentRevision(session);
    const auto r1PathIntent = session.capturePlainSavePathIntent();
    expectations.expect(session.execute(rename("R2", session, "R2")).changed(),
                        "the second same-path publication captures revision two");
    const auto revisionTwo = currentRevision(session);
    const auto r2PathIntent = session.capturePlainSavePathIntent();

    expectations.expect(session.acceptSavepoint(r2PathIntent, publicationIntent(2), revisionTwo) ==
                                ProjectSessionSavepointStatus::Accepted &&
                            session.stateSnapshot().cleanRevision == revisionTwo &&
                            session.stateSnapshot().dirty == false,
                        "the newer same-path callback establishes the exact clean revision");
    const auto afterR2 = session.stateSnapshot();
    expectations.expect(
        session.acceptSavepoint(r1PathIntent, publicationIntent(1), revisionOne) ==
                ProjectSessionSavepointStatus::StaleIntent &&
            session.acceptSavepoint(r1PathIntent, publicationIntent(2), revisionOne) ==
                ProjectSessionSavepointStatus::StaleIntent,
        "older and duplicate publication callbacks are rejected before state mutation");
    const auto afterReverseCallbacks = session.stateSnapshot();
    expectations.expect(
        afterReverseCallbacks.displayPath == afterR2.displayPath &&
            afterReverseCallbacks.currentRevision == revisionTwo &&
            afterReverseCallbacks.cleanRevision == revisionTwo &&
            afterReverseCallbacks.dirty == false &&
            afterReverseCallbacks.newestAcceptedPublicationIntent == publicationIntent(2),
        "reverse r1/r2 callbacks cannot roll back the path, clean revision, or dirty truth");
}

void testPublicationFrontierScopesToPathGeneration(Expectations& expectations) {
    ProjectSessionIdentitySource identitySource;
    auto bThenA = decodedSessionWithPath(expectations, identitySource, "current-a.bloom");
    const auto bCurrentPath = bThenA.capturePlainSavePathIntent();
    expectations.expect(
        bThenA.acceptSavepoint(bCurrentPath, publicationIntent(2), currentRevision(bThenA)) ==
                ProjectSessionSavepointStatus::Accepted &&
            bThenA.stateSnapshot().newestAcceptedPublicationIntent == publicationIntent(2),
        "B may accept on the current path generation while A is still resolving its target");

    const auto aResolved = bThenA.advancePathIntentForSaveAs();
    const auto aTarget = ProjectDisplayPath::create("resolved-a.bloom");
    expectations.expect(
        aResolved && aTarget.has_value() &&
            !bThenA.stateSnapshot().newestAcceptedPublicationIntent.isValid() &&
            bThenA.acceptSavepoint(aResolved.capture(), publicationIntent(1),
                                   currentRevision(bThenA),
                                   aTarget) == ProjectSessionSavepointStatus::Accepted,
        "A's lower publication ID is valid after resolving into a newer path generation");
    const auto afterA = bThenA.stateSnapshot();
    expectations.expect(
        afterA.displayPath == aTarget &&
            afterA.pathIntentKind == SessionPathIntentKind::ExistingPath &&
            afterA.newestAcceptedPublicationIntent == publicationIntent(1) &&
            bThenA.acceptSavepoint(bCurrentPath, publicationIntent(3), currentRevision(bThenA)) ==
                ProjectSessionSavepointStatus::StaleIntent &&
            bThenA.stateSnapshot().displayPath == afterA.displayPath &&
            bThenA.stateSnapshot().newestAcceptedPublicationIntent == publicationIntent(1),
        "an old-generation callback remains stale even when its publication ID is newer");

    auto aThenB = decodedSessionWithPath(expectations, identitySource, "current-b.bloom");
    const auto delayedB = aThenB.capturePlainSavePathIntent();
    const auto newerPathA = aThenB.advancePathIntentForSaveAs();
    const auto newerTargetA = ProjectDisplayPath::create("newer-a.bloom");
    expectations.expect(
        newerPathA && newerTargetA.has_value() &&
            aThenB.acceptSavepoint(delayedB, publicationIntent(2), currentRevision(aThenB)) ==
                ProjectSessionSavepointStatus::StaleIntent &&
            aThenB.acceptSavepoint(newerPathA.capture(), publicationIntent(1),
                                   currentRevision(aThenB),
                                   newerTargetA) == ProjectSessionSavepointStatus::Accepted &&
            aThenB.acceptSavepoint(newerPathA.capture(), publicationIntent(3),
                                   currentRevision(aThenB),
                                   newerTargetA) == ProjectSessionSavepointStatus::StaleIntent,
        "advancing A first rejects delayed B and makes A's replacement capture single-use");
}

void testSaveAsAbandonment(Expectations& expectations) {
    ProjectSessionIdentitySource identitySource;
    auto pathful = decodedSessionWithPath(expectations, identitySource, "abandon-current.bloom");
    const auto pending = pathful.advancePathIntentForSaveAs();
    expectations.expect(pending && !pathful.capturePlainSavePathIntent().isValid(),
                        "pending replacement authority excludes plain Save capture");
    const auto beforeAbandon = pathful.stateSnapshot();
    expectations.expect(pathful.abandonSaveAsIntent(pending.capture()) ==
                            SessionPathIntentAbandonStatus::Abandoned,
                        "the still-current replacement intent can be abandoned");
    const auto afterAbandon = pathful.stateSnapshot();
    const auto lateTarget = ProjectDisplayPath::create("late-abandoned.bloom");
    expectations.expect(
        afterAbandon.pathIntentKind == SessionPathIntentKind::ExistingPath &&
            afterAbandon.pathIntentGeneration == beforeAbandon.pathIntentGeneration &&
            afterAbandon.displayPath == beforeAbandon.displayPath &&
            afterAbandon.cleanRevision == beforeAbandon.cleanRevision &&
            afterAbandon.newestAcceptedPublicationIntent ==
                beforeAbandon.newestAcceptedPublicationIntent &&
            pathful.capturePlainSavePathIntent().isValid() &&
            pathful.abandonSaveAsIntent(pending.capture()) ==
                SessionPathIntentAbandonStatus::StaleIntent &&
            lateTarget.has_value() &&
            pathful.acceptSavepoint(pending.capture(), publicationIntent(1),
                                    currentRevision(pathful),
                                    lateTarget) == ProjectSessionSavepointStatus::StaleIntent,
        "abandonment changes only phase and makes the replacement callback permanently stale");

    const auto superseded = pathful.advancePathIntentForSaveAs();
    const auto current = pathful.advancePathIntentForSaveAs();
    expectations.expect(
        superseded && current &&
            pathful.abandonSaveAsIntent(superseded.capture()) ==
                SessionPathIntentAbandonStatus::StaleIntent &&
            pathful.stateSnapshot().pathIntentKind == SessionPathIntentKind::ReplacementPath &&
            pathful.abandonSaveAsIntent(current.capture()) ==
                SessionPathIntentAbandonStatus::Abandoned,
        "a newer Save As supersedes pending replacement authority before abandonment");

    auto pathless = newSession(expectations, identitySource);
    const auto pathlessPending = pathless.advancePathIntentForSaveAs();
    expectations.expect(
        pathlessPending &&
            pathless.abandonSaveAsIntent(pathlessPending.capture()) ==
                SessionPathIntentAbandonStatus::Abandoned &&
            pathless.stateSnapshot().pathIntentKind == SessionPathIntentKind::ExistingPath &&
            !pathless.capturePlainSavePathIntent().isValid(),
        "abandoning pathless Save As restores Existing phase without fabricating a plain path");
    const auto replacedPathlessPending = pathless.advancePathIntentForSaveAs();
    expectations.expect(
        replacedPathlessPending &&
            bloom::host::ProjectSessionTestAccess::advanceResultAcceptance(pathless) ==
                SessionResultAcceptanceAdvanceStatus::Advanced &&
            pathless.stateSnapshot().pathIntentKind == SessionPathIntentKind::ExistingPath &&
            !pathless.capturePlainSavePathIntent().isValid() &&
            pathless.abandonSaveAsIntent(replacedPathlessPending.capture()) ==
                SessionPathIntentAbandonStatus::StaleIntent,
        "result replacement cancels pathless replacement authority without fabricating a path");
}

void testCommandResultsAndNoChange(Expectations& expectations,
                                   ProjectSessionIdentitySource& identitySource) {
    auto session = newSession(expectations, identitySource);
    const auto undo = session.undo();
    const auto redo = session.redo();
    expectations.expect(undo.completed() && commandStatus(undo) == CommandStatus::NothingToUndo,
                        "empty undo returns the exact command result");
    expectations.expect(redo.completed() && commandStatus(redo) == CommandStatus::NothingToRedo,
                        "empty redo returns the exact command result");

    Transaction noChange("No change", currentRevision(session));
    noChange.emplace<SetProjectName>("Project");
    const auto unchanged = session.execute(std::move(noChange));
    expectations.expect(unchanged.completed() && !unchanged.changed() &&
                            commandStatus(unchanged) == CommandStatus::NoChange &&
                            currentRevision(session).value() == 0 &&
                            session.stateSnapshot().historySize == 0 &&
                            session.stateSnapshot().dirty == false,
                        "an idempotent command does not create revision, history, or dirtiness");

    Transaction rejected("Missing composition", currentRevision(session));
    rejected.emplace<SetCompositionName>(bloom::document::CompositionId::fromRaw(999), "Missing");
    const auto rejection = session.execute(std::move(rejected));
    expectations.expect(
        rejection.completed() && commandStatus(rejection) == CommandStatus::Rejected &&
            currentRevision(session).value() == 0 && session.stateSnapshot().historySize == 0,
        "a rejected transaction preserves the complete session state");
}

void testDegradedEditableAuthorization(Expectations& expectations,
                                       ProjectSessionIdentitySource& identitySource) {
    auto createdProject = bloom::document::makeNewProject(
        "Degraded", "Main", bloom::core::RationalTime::fromInteger(5));
    auto path = ProjectDisplayPath::create(std::filesystem::path("degraded.bloom"));
    if (!path.has_value()) {
        expectations.expect(false, "the degraded fixture path is valid");
        return;
    }
    auto result = ProjectSession::createDecoded(
        identitySource, {
                            .project = std::move(createdProject.project),
                            .colorSettings = neutralColorSettings(),
                            .editability = DecodedProjectEditability::DegradedEditable,
                            .displayPath = path,
                            .persistedAllocatorHighWater = std::nullopt,
                        });
    expectations.expect(static_cast<bool>(result), "a valid degraded document installs");
    if (!result) {
        return;
    }
    auto session = std::move(result).takeSession();
    expectations.expect(session.stateSnapshot().editability ==
                            DecodedProjectEditability::DegradedEditable,
                        "degraded capability remains visible to callers");
    expectations.expect(session.execute(rename("Edited degraded", session)).changed(),
                        "degraded-editable documents remain authorable");
    expectations.expect(session.acceptSavepoint(session.capturePlainSavePathIntent(),
                                                publicationIntent(1), currentRevision(session)) ==
                            ProjectSessionSavepointStatus::Accepted,
                        "the acceptance seam can record a proven degraded save result");
}

void testPreservedReadOnlyState(Expectations& expectations,
                                ProjectSessionIdentitySource& identitySource) {
    auto result = ProjectSession::createPreservedReadOnly(identitySource, "preserved.bloom");
    expectations.expect(static_cast<bool>(result), "a non-empty preserved path is accepted");
    if (!result) {
        return;
    }
    auto session = std::move(result).takeSession();
    const auto state = session.stateSnapshot();
    expectations.expect(
        state.valid && state.contentKind == ProjectSessionContentKind::PreservedReadOnly &&
            !state.editability.has_value() && !state.currentRevision.has_value() &&
            !state.cleanRevision.has_value() && !state.dirty.has_value(),
        "preserved read-only state claims no decoded document or mutable dirtiness");
    expectations.expect(state.displayPath.has_value() &&
                            state.displayPath->value() == std::filesystem::path("preserved.bloom"),
                        "the preserved path is presentation state, not a fabricated target key");
    expectations.expect(session.decodedSnapshot().status() ==
                            DecodedProjectSnapshotStatus::NoDecodedDocument,
                        "decoded access fails with a typed no-document status");
    expectations.expect(!session.capturePlainSavePathIntent().isValid() &&
                            session.advancePathIntentForSaveAs().status() ==
                                SessionPathIntentAdvanceStatus::ReadOnly &&
                            session.abandonSaveAsIntent({}) ==
                                SessionPathIntentAbandonStatus::ReadOnly,
                        "preserved content cannot manufacture a native Save or Save As intent");

    const auto firstOpen = session.admitOpenIntent();
    const auto secondOpen = session.admitOpenIntent();
    expectations.expect(
        firstOpen && secondOpen &&
            firstOpen.capture().contentKind() == ProjectSessionContentKind::PreservedReadOnly &&
            !firstOpen.capture().decodedRevision().has_value() &&
            secondOpen.capture().contentKind() == ProjectSessionContentKind::PreservedReadOnly &&
            !secondOpen.capture().decodedRevision().has_value() &&
            !session.isDesiredOpenIntent(firstOpen.capture()) &&
            session.isDesiredOpenIntent(secondOpen.capture()),
        "preserved Open intent binds to read-only content without fabricating a revision");

    Transaction blocked("Blocked edit");
    blocked.emplace<SetProjectName>("Must not apply");
    expectations.expect(session.execute(std::move(blocked)).status ==
                                ProjectSessionCommandStatus::ReadOnly &&
                            session.undo().status == ProjectSessionCommandStatus::ReadOnly &&
                            session.redo().status == ProjectSessionCommandStatus::ReadOnly,
                        "execute, undo, and redo are explicitly rejected as read-only");
    expectations.expect(session.acceptSavepoint({}, {}, bloom::document::Revision{}) ==
                            ProjectSessionSavepointStatus::ReadOnly,
                        "preserved read-only content cannot establish a native savepoint");
}

void testInvalidConstruction(Expectations& expectations,
                             ProjectSessionIdentitySource& identitySource) {
    const auto beforeInvalid = identitySource.snapshot();
    auto emptyName = newProjectRequest();
    emptyName.projectName.clear();
    expectations.expect(ProjectSession::createNew(identitySource, std::move(emptyName)).status() ==
                            ProjectSessionCreateStatus::InvalidNewProject,
                        "an empty project name is rejected without a partial session");

    auto invalidDuration = newProjectRequest();
    invalidDuration.duration = {};
    expectations.expect(
        ProjectSession::createNew(identitySource, std::move(invalidDuration)).status() ==
            ProjectSessionCreateStatus::InvalidNewProject,
        "a nonpositive composition duration is rejected");
    expectations.expect(!ProjectDisplayPath::create({}).has_value() &&
                            ProjectSession::createPreservedReadOnly(identitySource, {}).status() ==
                                ProjectSessionCreateStatus::InvalidDisplayPath,
                        "empty display paths are rejected at both construction seams");

    bloom::document::Project invalidProject(bloom::document::ProjectId{}, "Invalid");
    expectations.expect(ProjectSession::createDecoded(
                            identitySource, {.project = std::move(invalidProject),
                                             .colorSettings = neutralColorSettings(),
                                             .editability = DecodedProjectEditability::Editable,
                                             .displayPath = std::nullopt,
                                             .persistedAllocatorHighWater = std::nullopt})
                                .status() == ProjectSessionCreateStatus::InvalidDecodedProject,
                        "an invalid decoded project cannot install");

    auto unknownEditability = bloom::document::makeNewProject(
        "Unknown editability", "Main", bloom::core::RationalTime::fromInteger(5));
    expectations.expect(
        ProjectSession::createDecoded(
            identitySource, {.project = std::move(unknownEditability.project),
                             .colorSettings = neutralColorSettings(),
                             // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
                             .editability = static_cast<DecodedProjectEditability>(255),
                             .displayPath = std::nullopt,
                             .persistedAllocatorHighWater = std::nullopt})
                .status() == ProjectSessionCreateStatus::InvalidDecodedProject,
        "an unknown editability discriminator cannot authorize commands");

    auto insufficientHighWater = bloom::document::makeNewProject(
        "Persisted", "Main", bloom::core::RationalTime::fromInteger(5));
    expectations.expect(
        ProjectSession::createDecoded(
            identitySource,
            {
                .project = std::move(insufficientHighWater.project),
                .colorSettings = neutralColorSettings(),
                .editability = DecodedProjectEditability::Editable,
                .displayPath = std::nullopt,
                .persistedAllocatorHighWater = bloom::document::IdAllocatorHighWater{},
            })
                .status() == ProjectSessionCreateStatus::InvalidDecodedProject,
        "persisted allocator high-water must cover every decoded durable ID");
    const auto afterInvalid = identitySource.snapshot();
    expectations.expect(afterInvalid.lastIssuedSessionId == beforeInvalid.lastIssuedSessionId &&
                            afterInvalid.identityExhausted == beforeInvalid.identityExhausted,
                        "invalid construction never consumes a runtime session identity");
}

void testColorSettingsGatingAndRoundTripValidation(Expectations& expectations,
                                                   ProjectSessionIdentitySource& identitySource) {
    // createNew() has no color settings to synthesize (see color_settings.hpp's
    // makeBloomNeutralColorSettingsV1(), which requires a real caller-supplied content-revision
    // digest); such a session is gated unsaveable-pending-color.
    auto session = newSession(expectations, identitySource);
    const auto plainSave = session.capturePlainSavePathIntent();
    expectations.expect(session.captureSaveInput(plainSave).status() ==
                            SessionSaveInputStatus::ColorSettingsUnavailable,
                        "a createNew session has no color settings and is gated "
                        "unsaveable-pending-color");

    // A decoded session's captureSaveInput carries the exact captured fields.
    auto decoded = decodedSessionWithPath(expectations, identitySource, "capture.bloom");
    const auto decodedIntent = decoded.capturePlainSavePathIntent();
    const auto decodedCaptured = decoded.captureSaveInput(decodedIntent);
    expectations.expect(static_cast<bool>(decodedCaptured) && decodedCaptured.value() != nullptr &&
                            decodedCaptured.value()->revision() == currentRevision(decoded) &&
                            decodedCaptured.value()->roundTrip() == nullptr &&
                            decodedCaptured.value()->schemaMinor() == 0 &&
                            decodedCaptured.value()->retainedRequirements().empty() &&
                            decodedCaptured.value()->displayPath().has_value() &&
                            decodedCaptured.value()->displayPath()->value() ==
                                std::filesystem::path("capture.bloom") &&
                            decodedCaptured.value()->pathIntent() == decodedIntent &&
                            decodedCaptured.value()->resultAcceptance() ==
                                decoded.captureResultAcceptance(),
                        "captureSaveInput carries the exact captured save-input fields");

    // ReadOnly for preserved content.
    auto preservedResult =
        ProjectSession::createPreservedReadOnly(identitySource, "preserved-capture.bloom");
    expectations.expect(static_cast<bool>(preservedResult),
                        "the preserved-capture fixture installs");
    if (preservedResult) {
        auto preserved = std::move(preservedResult).takeSession();
        expectations.expect(preserved.captureSaveInput({}).status() ==
                                SessionSaveInputStatus::ReadOnly,
                            "preserved-read-only content cannot capture save input");
    }

    // A present roundTrip with schemaMinor 0 cannot name the newer minor it was captured
    // against: typed create failure.
    auto rtProject = bloom::document::makeNewProject("RT invalid", "Main",
                                                     bloom::core::RationalTime::fromInteger(5));
    bloom::project::RoundTripState roundTrip;
    const auto invalidRoundTrip = ProjectSession::createDecoded(
        identitySource, {.project = std::move(rtProject.project),
                         .colorSettings = neutralColorSettings(),
                         .editability = DecodedProjectEditability::Editable,
                         .displayPath = std::nullopt,
                         .persistedAllocatorHighWater = std::nullopt,
                         .roundTrip = std::move(roundTrip),
                         .schemaMinor = 0});
    expectations.expect(invalidRoundTrip.status() ==
                            ProjectSessionCreateStatus::InvalidDecodedProject,
                        "a present roundTrip with schemaMinor 0 is a typed create failure");
}

void testMoveAndLifetimeSafety(Expectations& expectations,
                               ProjectSessionIdentitySource& identitySource) {
    auto original = newSession(expectations, identitySource);
    expectations.expect(original.execute(rename("Before move", original, "Owned label")).changed(),
                        "the move fixture has history");
    const auto acceptanceCapture = original.captureResultAcceptance();
    const auto openCapture = original.admitOpenIntent().capture();
    const auto replacementCapture = original.advancePathIntentForSaveAs().capture();
    const auto movedPath = ProjectDisplayPath::create("moved.bloom");
    expectations.expect(movedPath.has_value() &&
                            original.acceptSavepoint(replacementCapture, publicationIntent(9),
                                                     currentRevision(original), movedPath) ==
                                ProjectSessionSavepointStatus::Accepted,
                        "the move fixture owns an accepted publication frontier");
    const auto pathCapture = original.capturePlainSavePathIntent();
    const auto beforeMove = original.stateSnapshot();
    auto moved = std::move(original);
    expectations.expect(
        !original.isValid() && // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
            original.decodedSnapshot()
                    .status() == // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
                DecodedProjectSnapshotStatus::InvalidSession,
        "a moved-from session exposes typed invalid state");
    Transaction invalidEdit("Moved-from edit");
    expectations.expect(
        original.execute(std::move(invalidEdit))
                .status == // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
            ProjectSessionCommandStatus::InvalidSession,
        "a moved-from session cannot mutate transferred ownership");
    expectations.expect(
        !original.captureResultAcceptance()
                .isValid() && // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
            !original.capturePlainSavePathIntent()
                 .isValid() && // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
            original.admitOpenIntent()
                    .status() == // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
                OpenIntentAdmissionStatus::InvalidSession &&
            original.advancePathIntentForSaveAs()
                    .status() == // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
                SessionPathIntentAdvanceStatus::InvalidSession &&
            original.abandonSaveAsIntent(replacementCapture) ==
                SessionPathIntentAbandonStatus::InvalidSession &&
            !original.matchesResultAcceptance(
                acceptanceCapture) && // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
            !original.isDesiredOpenIntent(
                openCapture) && // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
            !original.matchesPathIntent(
                pathCapture), // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
        "a moved-from session exposes only invalid captures and cannot accept prior work");
    expectations.expect(
        moved.isValid() && projectName(moved) == "Before move" &&
            moved.stateSnapshot().undoLabel == "Owned label" &&
            beforeMove.undoLabel == "Owned label" &&
            moved.stateSnapshot().projectSessionId == beforeMove.projectSessionId &&
            moved.stateSnapshot().newestAcceptedPublicationIntent ==
                beforeMove.newestAcceptedPublicationIntent &&
            openCapture.contentKind() == ProjectSessionContentKind::DecodedDocument &&
            openCapture.decodedRevision() == beforeMove.currentRevision &&
            moved.matchesResultAcceptance(acceptanceCapture) &&
            moved.isDesiredOpenIntent(openCapture) && moved.matchesPathIntent(pathCapture),
        "moving preserves identity, generations, history, and captured authority");
    expectations.expect(moved.undo().changed() && projectName(moved) == "Project",
                        "the moved command stack remains bound to its heap-owned document");
}

} // namespace

int main() {
    Expectations expectations;
    ProjectSessionIdentitySource identitySource;
    try {
        testIdentitySourceAndExactExhaustion(expectations);
        testGenerationCapturesAndSubsetPredicates(expectations);
        testOpenIntentBindsExactContent(expectations);
        testGenerationExhaustionBoundaries(expectations);
        testNewProjectBaselineAndSnapshots(expectations, identitySource);
        testDirtySavepointBranching(expectations, identitySource);
        testSavepointPathAuthority(expectations);
        testPublicationCallbackOrdering(expectations);
        testPublicationFrontierScopesToPathGeneration(expectations);
        testSaveAsAbandonment(expectations);
        testCommandResultsAndNoChange(expectations, identitySource);
        testDegradedEditableAuthorization(expectations, identitySource);
        testPreservedReadOnlyState(expectations, identitySource);
        testInvalidConstruction(expectations, identitySource);
        testColorSettingsGatingAndRoundTripValidation(expectations, identitySource);
        testMoveAndLifetimeSafety(expectations, identitySource);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: unexpected fixture exception: " << error.what() << '\n';
        return 1;
    }
    return expectations.failures() == 0 ? 0 : 1;
}
