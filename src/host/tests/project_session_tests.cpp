#include <bloom/host/project_session.hpp>

#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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
using bloom::host::ProjectDisplayPath;
using bloom::host::ProjectSession;
using bloom::host::ProjectSessionCommandStatus;
using bloom::host::ProjectSessionContentKind;
using bloom::host::ProjectSessionCreateStatus;
using bloom::host::ProjectSessionSavepointStatus;

[[nodiscard]] CommandStatus commandStatus(const bloom::host::ProjectSessionCommandResult& result) {
    if (!result.command.has_value()) {
        throw std::logic_error("Expected a forwarded command result");
    }
    return result.command->status;
}

[[nodiscard]] NewProjectSessionRequest newProjectRequest() {
    return {
        .projectName = "Project",
        .compositionName = "Main",
        .duration = bloom::core::RationalTime::fromInteger(10),
        .format = {},
    };
}

[[nodiscard]] ProjectSession newSession(Expectations& expectations) {
    auto result = ProjectSession::createNew(newProjectRequest());
    expectations.expect(static_cast<bool>(result), "the new-project fixture must be valid");
    if (!result) {
        throw std::logic_error("Could not create project-session fixture");
    }
    return std::move(result).takeSession();
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

void testNewProjectBaselineAndSnapshots(Expectations& expectations) {
    auto session = newSession(expectations);
    const auto state = session.stateSnapshot();
    expectations.expect(session.isValid() && state.valid,
                        "a new session owns one coherent valid state");
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

void testDirtySavepointBranching(Expectations& expectations) {
    auto session = newSession(expectations);
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

    const auto missingPath = session.acceptSavepoint(firstRevision);
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
    expectations.expect(session.acceptSavepoint(firstRevision, path) ==
                                ProjectSessionSavepointStatus::Accepted &&
                            session.stateSnapshot().dirty == false,
                        "an accepted publication records its exact clean revision and path");

    expectations.expect(session.execute(rename("Second", session, "Second edit")).changed(),
                        "a second edit commits");
    const auto secondRevision = currentRevision(session);
    expectations.expect(secondRevision.value() == 2 && session.stateSnapshot().dirty == true,
                        "editing after a savepoint is dirty");
    expectations.expect(session.acceptSavepoint(firstRevision) ==
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
    expectations.expect(session.acceptSavepoint(currentRevision(session)) ==
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
    expectations.expect(session.acceptSavepoint(future) ==
                                ProjectSessionSavepointStatus::UnknownRevision &&
                            session.stateSnapshot().cleanRevision == beforeFuture.cleanRevision,
                        "an unobserved future revision cannot move the savepoint");
}

void testCommandResultsAndNoChange(Expectations& expectations) {
    auto session = newSession(expectations);
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

void testDegradedEditableAuthorization(Expectations& expectations) {
    auto createdProject = bloom::document::makeNewProject(
        "Degraded", "Main", bloom::core::RationalTime::fromInteger(5));
    auto path = ProjectDisplayPath::create(std::filesystem::path("degraded.bloom"));
    if (!path.has_value()) {
        expectations.expect(false, "the degraded fixture path is valid");
        return;
    }
    auto result = ProjectSession::createDecoded({
        .project = std::move(createdProject.project),
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
    expectations.expect(session.acceptSavepoint(currentRevision(session)) ==
                            ProjectSessionSavepointStatus::Accepted,
                        "the acceptance seam can record a proven degraded save result");
}

void testPreservedReadOnlyState(Expectations& expectations) {
    auto result = ProjectSession::createPreservedReadOnly("preserved.bloom");
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

    Transaction blocked("Blocked edit");
    blocked.emplace<SetProjectName>("Must not apply");
    expectations.expect(session.execute(std::move(blocked)).status ==
                                ProjectSessionCommandStatus::ReadOnly &&
                            session.undo().status == ProjectSessionCommandStatus::ReadOnly &&
                            session.redo().status == ProjectSessionCommandStatus::ReadOnly,
                        "execute, undo, and redo are explicitly rejected as read-only");
    expectations.expect(session.acceptSavepoint(bloom::document::Revision{}) ==
                            ProjectSessionSavepointStatus::ReadOnly,
                        "preserved read-only content cannot establish a native savepoint");
}

void testInvalidConstruction(Expectations& expectations) {
    auto emptyName = newProjectRequest();
    emptyName.projectName.clear();
    expectations.expect(ProjectSession::createNew(std::move(emptyName)).status() ==
                            ProjectSessionCreateStatus::InvalidNewProject,
                        "an empty project name is rejected without a partial session");

    auto invalidDuration = newProjectRequest();
    invalidDuration.duration = {};
    expectations.expect(ProjectSession::createNew(std::move(invalidDuration)).status() ==
                            ProjectSessionCreateStatus::InvalidNewProject,
                        "a nonpositive composition duration is rejected");
    expectations.expect(!ProjectDisplayPath::create({}).has_value() &&
                            ProjectSession::createPreservedReadOnly({}).status() ==
                                ProjectSessionCreateStatus::InvalidDisplayPath,
                        "empty display paths are rejected at both construction seams");

    bloom::document::Project invalidProject(bloom::document::ProjectId{}, "Invalid");
    expectations.expect(
        ProjectSession::createDecoded({.project = std::move(invalidProject),
                                       .editability = DecodedProjectEditability::Editable,
                                       .displayPath = std::nullopt,
                                       .persistedAllocatorHighWater = std::nullopt})
                .status() == ProjectSessionCreateStatus::InvalidDecodedProject,
        "an invalid decoded project cannot install");

    auto unknownEditability = bloom::document::makeNewProject(
        "Unknown editability", "Main", bloom::core::RationalTime::fromInteger(5));
    expectations.expect(ProjectSession::createDecoded(
                            {.project = std::move(unknownEditability.project),
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
            {
                .project = std::move(insufficientHighWater.project),
                .editability = DecodedProjectEditability::Editable,
                .displayPath = std::nullopt,
                .persistedAllocatorHighWater = bloom::document::IdAllocatorHighWater{},
            })
                .status() == ProjectSessionCreateStatus::InvalidDecodedProject,
        "persisted allocator high-water must cover every decoded durable ID");
}

void testMoveAndLifetimeSafety(Expectations& expectations) {
    auto original = newSession(expectations);
    expectations.expect(original.execute(rename("Before move", original, "Owned label")).changed(),
                        "the move fixture has history");
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
    expectations.expect(moved.isValid() && projectName(moved) == "Before move" &&
                            moved.stateSnapshot().undoLabel == "Owned label" &&
                            beforeMove.undoLabel == "Owned label",
                        "moving preserves document, history references, and owned snapshots");
    expectations.expect(moved.undo().changed() && projectName(moved) == "Project",
                        "the moved command stack remains bound to its heap-owned document");
}

} // namespace

int main() {
    Expectations expectations;
    try {
        testNewProjectBaselineAndSnapshots(expectations);
        testDirtySavepointBranching(expectations);
        testCommandResultsAndNoChange(expectations);
        testDegradedEditableAuthorization(expectations);
        testPreservedReadOnlyState(expectations);
        testInvalidConstruction(expectations);
        testMoveAndLifetimeSafety(expectations);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: unexpected fixture exception: " << error.what() << '\n';
        return 1;
    }
    return expectations.failures() == 0 ? 0 : 1;
}
