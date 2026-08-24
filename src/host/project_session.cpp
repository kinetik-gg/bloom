#include <bloom/host/project_session.hpp>

#include <bloom/document/new_project.hpp>

#include <exception>
#include <new>
#include <stdexcept>
#include <string>
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

} // namespace

namespace bloom::host {

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

ProjectSession::ProjectSession(std::unique_ptr<document::Document> document,
                               std::unique_ptr<commands::CommandStack> commandStack,
                               const DecodedProjectEditability editability,
                               std::optional<ProjectDisplayPath> displayPath) noexcept
    : contentKind_(ProjectSessionContentKind::DecodedDocument), editability_(editability),
      displayPath_(std::move(displayPath)), cleanRevision_(document->snapshot().revision()),
      document_(std::move(document)), commandStack_(std::move(commandStack)), valid_(true) {}

ProjectSession::ProjectSession(ProjectDisplayPath preservedDisplayPath) noexcept
    : contentKind_(ProjectSessionContentKind::PreservedReadOnly),
      displayPath_(std::move(preservedDisplayPath)), valid_(true) {}

ProjectSession::ProjectSession(ProjectSession&& other) noexcept
    : contentKind_(other.contentKind_), editability_(other.editability_),
      displayPath_(std::move(other.displayPath_)), cleanRevision_(other.cleanRevision_),
      document_(std::move(other.document_)), commandStack_(std::move(other.commandStack_)),
      valid_(std::exchange(other.valid_, false)) {}

ProjectSessionCreateResult ProjectSession::createNew(NewProjectSessionRequest request) {
    try {
        auto created = document::makeNewProject(std::move(request.projectName),
                                                std::move(request.compositionName),
                                                request.duration, request.format);
        auto document = std::make_unique<document::Document>(std::move(created.project));
        auto commandStack = std::make_unique<commands::CommandStack>(*document);
        return ProjectSessionCreateResult(
            ProjectSession(std::move(document), std::move(commandStack),
                           DecodedProjectEditability::Editable, std::nullopt));
    } catch (const std::bad_alloc&) {
        return ProjectSessionCreateResult(ProjectSessionCreateStatus::ResourceUnavailable);
    } catch (const std::logic_error&) {
        return ProjectSessionCreateResult(ProjectSessionCreateStatus::InvalidNewProject);
    }
}

ProjectSessionCreateResult ProjectSession::createDecoded(DecodedProjectSessionRequest request) {
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
        return ProjectSessionCreateResult(
            ProjectSession(std::move(document), std::move(commandStack), request.editability,
                           std::move(request.displayPath)));
    } catch (const std::bad_alloc&) {
        return ProjectSessionCreateResult(ProjectSessionCreateStatus::ResourceUnavailable);
    } catch (const std::invalid_argument&) {
        return ProjectSessionCreateResult(ProjectSessionCreateStatus::InvalidDecodedProject);
    }
}

ProjectSessionCreateResult
ProjectSession::createPreservedReadOnly(std::filesystem::path displayPath) {
    auto validatedPath = ProjectDisplayPath::create(std::move(displayPath));
    if (!validatedPath.has_value()) {
        return ProjectSessionCreateResult(ProjectSessionCreateStatus::InvalidDisplayPath);
    }
    return ProjectSessionCreateResult(ProjectSession(std::move(*validatedPath)));
}

bool ProjectSession::isValid() const noexcept {
    if (!valid_ || (!displayPath_.has_value() &&
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

ProjectSessionSavepointStatus
ProjectSession::acceptSavepoint(const document::Revision publishedRevision,
                                std::optional<ProjectDisplayPath> publishedPath) {
    if (!isValid()) {
        return ProjectSessionSavepointStatus::InvalidSession;
    }
    if (contentKind_ != ProjectSessionContentKind::DecodedDocument) {
        return ProjectSessionSavepointStatus::ReadOnly;
    }
    const auto current = document_->snapshot().revision();
    if (publishedRevision > current) {
        return ProjectSessionSavepointStatus::UnknownRevision;
    }
    if (!displayPath_.has_value() && !publishedPath.has_value()) {
        return ProjectSessionSavepointStatus::PathRequired;
    }

    if (publishedPath.has_value()) {
        displayPath_ = std::move(publishedPath);
    }
    cleanRevision_ = publishedRevision;
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
