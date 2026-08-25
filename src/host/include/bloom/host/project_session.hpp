#pragma once

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/result.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/id.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/project.hpp>
#include <bloom/host/publication_coordinator.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace bloom::host {

struct ProjectSessionIdTag;
struct SessionResultAcceptanceGenerationTag;
struct OpenIntentGenerationTag;
struct SessionPathIntentGenerationTag;

using ProjectSessionId = core::Id<ProjectSessionIdTag>;
using SessionResultAcceptanceGeneration = core::Id<SessionResultAcceptanceGenerationTag>;
using OpenIntentGeneration = core::Id<OpenIntentGenerationTag>;
using SessionPathIntentGeneration = core::Id<SessionPathIntentGenerationTag>;

class ProjectSession;
class ProjectSessionTestAccess;

enum class ProjectSessionContentKind : std::uint8_t {
    DecodedDocument,
    PreservedReadOnly,
};

struct ProjectSessionIdentitySourceSnapshot final {
    ProjectSessionId lastIssuedSessionId;
    bool identityExhausted = false;
};

class ProjectSessionIdentitySource final {
  public:
    ProjectSessionIdentitySource() noexcept = default;
    ProjectSessionIdentitySource(const ProjectSessionIdentitySource&) = delete;
    ProjectSessionIdentitySource& operator=(const ProjectSessionIdentitySource&) = delete;
    ProjectSessionIdentitySource(ProjectSessionIdentitySource&&) = delete;
    ProjectSessionIdentitySource& operator=(ProjectSessionIdentitySource&&) = delete;
    ~ProjectSessionIdentitySource() = default;

    [[nodiscard]] ProjectSessionIdentitySourceSnapshot snapshot() const noexcept;

  private:
    friend class ProjectSession;
    friend class ProjectSessionTestAccess;

    [[nodiscard]] std::optional<ProjectSessionId> issue() noexcept;
    [[nodiscard]] bool setLastIssuedSessionIdForTesting(std::uint64_t value) noexcept;

    mutable std::mutex mutex_;
    std::uint64_t lastIssuedSessionId_ = 0;
};

class SessionResultAcceptanceCapture final {
  public:
    constexpr SessionResultAcceptanceCapture() noexcept = default;

    [[nodiscard]] constexpr ProjectSessionId projectSessionId() const noexcept {
        return projectSessionId_;
    }
    [[nodiscard]] constexpr SessionResultAcceptanceGeneration generation() const noexcept {
        return generation_;
    }
    [[nodiscard]] constexpr bool isValid() const noexcept {
        return projectSessionId_.isValid() && generation_.isValid();
    }

    friend constexpr bool operator==(const SessionResultAcceptanceCapture&,
                                     const SessionResultAcceptanceCapture&) noexcept = default;

  private:
    friend class ProjectSession;

    constexpr SessionResultAcceptanceCapture(
        const ProjectSessionId projectSessionId,
        const SessionResultAcceptanceGeneration generation) noexcept
        : projectSessionId_(projectSessionId), generation_(generation) {}

    ProjectSessionId projectSessionId_;
    SessionResultAcceptanceGeneration generation_;
};

class OpenIntentCapture final {
  public:
    constexpr OpenIntentCapture() noexcept = default;

    [[nodiscard]] constexpr SessionResultAcceptanceCapture resultAcceptance() const noexcept {
        return resultAcceptance_;
    }
    [[nodiscard]] constexpr OpenIntentGeneration generation() const noexcept { return generation_; }
    [[nodiscard]] constexpr ProjectSessionContentKind contentKind() const noexcept {
        return contentKind_;
    }
    [[nodiscard]] constexpr std::optional<document::Revision> decodedRevision() const noexcept {
        return decodedRevision_;
    }
    [[nodiscard]] constexpr bool isValid() const noexcept {
        return resultAcceptance_.isValid() && generation_.isValid() &&
               hasValidContentBinding(contentKind_, decodedRevision_);
    }

    friend constexpr bool operator==(const OpenIntentCapture&,
                                     const OpenIntentCapture&) noexcept = default;

  private:
    friend class ProjectSession;

    constexpr OpenIntentCapture(const SessionResultAcceptanceCapture resultAcceptance,
                                const OpenIntentGeneration generation,
                                const ProjectSessionContentKind contentKind,
                                const std::optional<document::Revision> decodedRevision) noexcept
        : resultAcceptance_(resultAcceptance), generation_(generation), contentKind_(contentKind),
          decodedRevision_(decodedRevision) {}

    [[nodiscard]] static constexpr bool
    hasValidContentBinding(const ProjectSessionContentKind contentKind,
                           const std::optional<document::Revision> decodedRevision) noexcept {
        switch (contentKind) {
        case ProjectSessionContentKind::DecodedDocument:
            return decodedRevision.has_value();
        case ProjectSessionContentKind::PreservedReadOnly:
            return !decodedRevision.has_value();
        }
        return false;
    }

    SessionResultAcceptanceCapture resultAcceptance_;
    OpenIntentGeneration generation_;
    ProjectSessionContentKind contentKind_ = ProjectSessionContentKind::PreservedReadOnly;
    std::optional<document::Revision> decodedRevision_;
};

enum class SessionPathIntentKind : std::uint8_t {
    ExistingPath,
    ReplacementPath,
};

class SessionPathIntentCapture final {
  public:
    constexpr SessionPathIntentCapture() noexcept = default;

    [[nodiscard]] constexpr SessionResultAcceptanceCapture resultAcceptance() const noexcept {
        return resultAcceptance_;
    }
    [[nodiscard]] constexpr SessionPathIntentGeneration generation() const noexcept {
        return generation_;
    }
    [[nodiscard]] constexpr SessionPathIntentKind kind() const noexcept { return kind_; }
    [[nodiscard]] constexpr bool isValid() const noexcept {
        return resultAcceptance_.isValid() && generation_.isValid() && isKnownKind(kind_);
    }

    friend constexpr bool operator==(const SessionPathIntentCapture&,
                                     const SessionPathIntentCapture&) noexcept = default;

  private:
    friend class ProjectSession;

    constexpr SessionPathIntentCapture(const SessionResultAcceptanceCapture resultAcceptance,
                                       const SessionPathIntentGeneration generation,
                                       const SessionPathIntentKind kind) noexcept
        : resultAcceptance_(resultAcceptance), generation_(generation), kind_(kind) {}

    [[nodiscard]] static constexpr bool isKnownKind(const SessionPathIntentKind kind) noexcept {
        switch (kind) {
        case SessionPathIntentKind::ExistingPath:
        case SessionPathIntentKind::ReplacementPath:
            return true;
        }
        return false;
    }

    SessionResultAcceptanceCapture resultAcceptance_;
    SessionPathIntentGeneration generation_;
    SessionPathIntentKind kind_ = SessionPathIntentKind::ExistingPath;
};

enum class OpenIntentAdmissionStatus : std::uint8_t {
    Admitted,
    InvalidSession,
    RuntimeIdentityExhausted,
};

class [[nodiscard]] OpenIntentAdmissionResult final {
  public:
    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return status_ == OpenIntentAdmissionStatus::Admitted && capture_.isValid();
    }
    [[nodiscard]] constexpr OpenIntentAdmissionStatus status() const noexcept { return status_; }
    [[nodiscard]] constexpr OpenIntentCapture capture() const noexcept { return capture_; }

  private:
    friend class ProjectSession;

    explicit constexpr OpenIntentAdmissionResult(const OpenIntentAdmissionStatus status) noexcept
        : status_(status) {}
    explicit constexpr OpenIntentAdmissionResult(const OpenIntentCapture capture) noexcept
        : status_(OpenIntentAdmissionStatus::Admitted), capture_(capture) {}

    OpenIntentAdmissionStatus status_ = OpenIntentAdmissionStatus::InvalidSession;
    OpenIntentCapture capture_;
};

enum class SessionPathIntentAdvanceStatus : std::uint8_t {
    Advanced,
    ReadOnly,
    InvalidSession,
    RuntimeIdentityExhausted,
};

enum class SessionPathIntentAbandonStatus : std::uint8_t {
    Abandoned,
    ReadOnly,
    InvalidSession,
    StaleIntent,
};

class [[nodiscard]] SessionPathIntentAdvanceResult final {
  public:
    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return status_ == SessionPathIntentAdvanceStatus::Advanced && capture_.isValid();
    }
    [[nodiscard]] constexpr SessionPathIntentAdvanceStatus status() const noexcept {
        return status_;
    }
    [[nodiscard]] constexpr SessionPathIntentCapture capture() const noexcept { return capture_; }

  private:
    friend class ProjectSession;

    explicit constexpr SessionPathIntentAdvanceResult(
        const SessionPathIntentAdvanceStatus status) noexcept
        : status_(status) {}
    explicit constexpr SessionPathIntentAdvanceResult(
        const SessionPathIntentCapture capture) noexcept
        : status_(SessionPathIntentAdvanceStatus::Advanced), capture_(capture) {}

    SessionPathIntentAdvanceStatus status_ = SessionPathIntentAdvanceStatus::InvalidSession;
    SessionPathIntentCapture capture_;
};

enum class SessionResultAcceptanceAdvanceStatus : std::uint8_t {
    Advanced,
    InvalidSession,
    RuntimeIdentityExhausted,
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
    RuntimeIdentityExhausted,
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
    InvalidPublicationIntent,
    StaleIntent,
    UnknownRevision,
    PathRequired,
    PathAuthorityMismatch,
};

enum class DecodedProjectSnapshotStatus : std::uint8_t {
    Available,
    NoDecodedDocument,
    InvalidSession,
};

struct ProjectSessionStateSnapshot final {
    ProjectSessionId projectSessionId;
    SessionResultAcceptanceGeneration resultAcceptanceGeneration;
    OpenIntentGeneration openIntentGeneration;
    SessionPathIntentGeneration pathIntentGeneration;
    SessionPathIntentKind pathIntentKind = SessionPathIntentKind::ExistingPath;
    // Scoped to the current result-acceptance/path-intent generation pair.
    PublicationIntentId newestAcceptedPublicationIntent;
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

    [[nodiscard]] static ProjectSessionCreateResult
    createNew(ProjectSessionIdentitySource& identitySource, NewProjectSessionRequest request);
    [[nodiscard]] static ProjectSessionCreateResult
    createDecoded(ProjectSessionIdentitySource& identitySource,
                  DecodedProjectSessionRequest request);
    [[nodiscard]] static ProjectSessionCreateResult
    createPreservedReadOnly(ProjectSessionIdentitySource& identitySource,
                            std::filesystem::path displayPath);

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] ProjectSessionStateSnapshot stateSnapshot() const;
    [[nodiscard]] DecodedProjectSnapshotResult decodedSnapshot() const;
    [[nodiscard]] SessionResultAcceptanceCapture captureResultAcceptance() const noexcept;
    [[nodiscard]] SessionPathIntentCapture capturePlainSavePathIntent() const noexcept;
    [[nodiscard]] OpenIntentAdmissionResult admitOpenIntent() noexcept;
    [[nodiscard]] SessionPathIntentAdvanceResult advancePathIntentForSaveAs() noexcept;
    [[nodiscard]] SessionPathIntentAbandonStatus
    abandonSaveAsIntent(SessionPathIntentCapture intent) noexcept;
    [[nodiscard]] bool
    matchesResultAcceptance(SessionResultAcceptanceCapture capture) const noexcept;
    [[nodiscard]] bool isDesiredOpenIntent(OpenIntentCapture capture) const noexcept;
    [[nodiscard]] bool matchesPathIntent(SessionPathIntentCapture capture) const noexcept;

    [[nodiscard]] ProjectSessionCommandResult execute(commands::Transaction transaction);
    [[nodiscard]] ProjectSessionCommandResult undo();
    [[nodiscard]] ProjectSessionCommandResult redo();

    // This is an acceptance seam, not persistence. A future I/O owner calls it only after a
    // publication result has succeeded; this method owns its session and path-intent checks.
    [[nodiscard]] ProjectSessionSavepointStatus
    acceptSavepoint(SessionPathIntentCapture intent, PublicationIntentId publicationIntent,
                    document::Revision publishedRevision,
                    std::optional<ProjectDisplayPath> publishedPath = std::nullopt);

  private:
    friend class ProjectSessionCreateResult;
    friend class ProjectSessionTestAccess;

    ProjectSession(ProjectSessionId projectSessionId, std::unique_ptr<document::Document> document,
                   std::unique_ptr<commands::CommandStack> commandStack,
                   DecodedProjectEditability editability, document::Revision cleanRevision,
                   std::optional<ProjectDisplayPath> displayPath) noexcept;
    ProjectSession(ProjectSessionId projectSessionId,
                   ProjectDisplayPath preservedDisplayPath) noexcept;

    [[nodiscard]] ProjectSessionCommandResult unavailableCommandResult() const noexcept;
    [[nodiscard]] SessionResultAcceptanceAdvanceStatus
    advanceResultAcceptanceForInstalledReplacement() noexcept;
    [[nodiscard]] bool
    setGenerationsForTesting(SessionResultAcceptanceGeneration resultAcceptanceGeneration,
                             OpenIntentGeneration openIntentGeneration,
                             SessionPathIntentGeneration pathIntentGeneration) noexcept;

    ProjectSessionId projectSessionId_;
    SessionResultAcceptanceGeneration resultAcceptanceGeneration_ =
        SessionResultAcceptanceGeneration::fromRaw(1);
    OpenIntentGeneration openIntentGeneration_ = OpenIntentGeneration::fromRaw(1);
    SessionPathIntentGeneration pathIntentGeneration_ = SessionPathIntentGeneration::fromRaw(1);
    SessionPathIntentKind pathIntentKind_ = SessionPathIntentKind::ExistingPath;
    PublicationIntentId newestAcceptedPublicationIntent_;
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
