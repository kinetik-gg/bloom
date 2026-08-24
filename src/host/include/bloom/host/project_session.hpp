#pragma once

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/result.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/project.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace bloom::host {

enum class ProjectSessionContentKind : std::uint8_t {
    DecodedDocument,
    PreservedReadOnly,
};

enum class DecodedProjectEditability : std::uint8_t {
    Editable,
    DegradedEditable,
};

class ProjectDisplayPath final {
  public:
    [[nodiscard]] static std::optional<ProjectDisplayPath>
    create(std::filesystem::path value) noexcept;

    [[nodiscard]] const std::filesystem::path& value() const noexcept { return value_; }
    friend bool operator==(const ProjectDisplayPath&, const ProjectDisplayPath&) = default;

  private:
    explicit ProjectDisplayPath(std::filesystem::path value) noexcept : value_(std::move(value)) {}

    std::filesystem::path value_;
};

struct NewProjectSessionRequest final {
    std::string projectName;
    std::string compositionName;
    core::RationalTime duration;
    document::CompositionFormat format;
};

struct DecodedProjectSessionRequest final {
    document::Project project;
    DecodedProjectEditability editability = DecodedProjectEditability::Editable;
    std::optional<ProjectDisplayPath> displayPath;
    std::optional<document::IdAllocatorHighWater> persistedAllocatorHighWater;
};

enum class ProjectSessionCreateStatus : std::uint8_t {
    Created,
    InvalidNewProject,
    InvalidDecodedProject,
    InvalidDisplayPath,
    ResourceUnavailable,
};

enum class ProjectSessionCommandStatus : std::uint8_t {
    Completed,
    ReadOnly,
    InvalidSession,
};

struct ProjectSessionCommandResult final {
    ProjectSessionCommandStatus status = ProjectSessionCommandStatus::InvalidSession;
    std::optional<commands::CommandResult> command;

    [[nodiscard]] bool completed() const noexcept {
        return status == ProjectSessionCommandStatus::Completed && command.has_value();
    }
    [[nodiscard]] bool changed() const noexcept {
        if (status != ProjectSessionCommandStatus::Completed || !command.has_value()) {
            return false;
        }
        return command->changed();
    }
};

enum class ProjectSessionSavepointStatus : std::uint8_t {
    Accepted,
    ReadOnly,
    InvalidSession,
    UnknownRevision,
    PathRequired,
};

enum class DecodedProjectSnapshotStatus : std::uint8_t {
    Available,
    NoDecodedDocument,
    InvalidSession,
};

struct ProjectSessionStateSnapshot final {
    ProjectSessionContentKind contentKind = ProjectSessionContentKind::PreservedReadOnly;
    std::optional<DecodedProjectEditability> editability;
    std::optional<ProjectDisplayPath> displayPath;
    std::optional<document::Revision> currentRevision;
    std::optional<document::Revision> cleanRevision;
    std::optional<bool> dirty;
    bool canUndo = false;
    bool canRedo = false;
    std::size_t historySize = 0;
    std::optional<std::string> undoLabel;
    std::optional<std::string> redoLabel;
    bool valid = false;
};

class [[nodiscard]] DecodedProjectSnapshotResult final {
  public:
    [[nodiscard]] explicit operator bool() const noexcept {
        return status_ == DecodedProjectSnapshotStatus::Available && snapshot_.has_value();
    }
    [[nodiscard]] DecodedProjectSnapshotStatus status() const noexcept { return status_; }
    [[nodiscard]] const document::Snapshot& snapshot() const&;
    const document::Snapshot& snapshot() const&& = delete;

  private:
    friend class ProjectSession;

    explicit DecodedProjectSnapshotResult(DecodedProjectSnapshotStatus status) noexcept;
    explicit DecodedProjectSnapshotResult(document::Snapshot snapshot) noexcept;

    DecodedProjectSnapshotStatus status_ = DecodedProjectSnapshotStatus::InvalidSession;
    std::optional<document::Snapshot> snapshot_;
};

class ProjectSessionCreateResult;

class ProjectSession final {
  public:
    ProjectSession(const ProjectSession&) = delete;
    ProjectSession& operator=(const ProjectSession&) = delete;
    ProjectSession(ProjectSession&& other) noexcept;
    ProjectSession& operator=(ProjectSession&&) = delete;
    ~ProjectSession() = default;

    [[nodiscard]] static ProjectSessionCreateResult createNew(NewProjectSessionRequest request);
    [[nodiscard]] static ProjectSessionCreateResult
    createDecoded(DecodedProjectSessionRequest request);
    [[nodiscard]] static ProjectSessionCreateResult
    createPreservedReadOnly(std::filesystem::path displayPath);

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] ProjectSessionStateSnapshot stateSnapshot() const;
    [[nodiscard]] DecodedProjectSnapshotResult decodedSnapshot() const;

    [[nodiscard]] ProjectSessionCommandResult execute(commands::Transaction transaction);
    [[nodiscard]] ProjectSessionCommandResult undo();
    [[nodiscard]] ProjectSessionCommandResult redo();

    // This is an acceptance seam, not persistence. A future I/O owner calls it only after a
    // publication result has passed its session and path-intent checks.
    [[nodiscard]] ProjectSessionSavepointStatus
    acceptSavepoint(document::Revision publishedRevision,
                    std::optional<ProjectDisplayPath> publishedPath = std::nullopt);

  private:
    friend class ProjectSessionCreateResult;

    ProjectSession(std::unique_ptr<document::Document> document,
                   std::unique_ptr<commands::CommandStack> commandStack,
                   DecodedProjectEditability editability,
                   std::optional<ProjectDisplayPath> displayPath) noexcept;
    explicit ProjectSession(ProjectDisplayPath preservedDisplayPath) noexcept;

    [[nodiscard]] ProjectSessionCommandResult unavailableCommandResult() const noexcept;

    ProjectSessionContentKind contentKind_ = ProjectSessionContentKind::PreservedReadOnly;
    std::optional<DecodedProjectEditability> editability_;
    std::optional<ProjectDisplayPath> displayPath_;
    std::optional<document::Revision> cleanRevision_;
    std::unique_ptr<document::Document> document_;
    std::unique_ptr<commands::CommandStack> commandStack_;
    bool valid_ = false;
};

class [[nodiscard]] ProjectSessionCreateResult final {
  public:
    ProjectSessionCreateResult(const ProjectSessionCreateResult&) = delete;
    ProjectSessionCreateResult& operator=(const ProjectSessionCreateResult&) = delete;
    ProjectSessionCreateResult(ProjectSessionCreateResult&&) noexcept = default;
    ProjectSessionCreateResult& operator=(ProjectSessionCreateResult&&) = delete;
    ~ProjectSessionCreateResult() = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status_ == ProjectSessionCreateStatus::Created && session_.has_value();
    }
    [[nodiscard]] ProjectSessionCreateStatus status() const noexcept { return status_; }
    [[nodiscard]] ProjectSession takeSession() && noexcept;

  private:
    friend class ProjectSession;

    explicit ProjectSessionCreateResult(ProjectSessionCreateStatus status) noexcept;
    explicit ProjectSessionCreateResult(ProjectSession session) noexcept;

    ProjectSessionCreateStatus status_ = ProjectSessionCreateStatus::ResourceUnavailable;
    std::optional<ProjectSession> session_;
};

} // namespace bloom::host
