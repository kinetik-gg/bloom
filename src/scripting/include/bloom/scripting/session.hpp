#pragma once

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/session_open.hpp>
#include <bloom/host/session_save.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/scripting/operation_registry.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace bloom::scripting {

class EventStream;

struct SessionDiagnostic final {
    std::string code;
    std::string message;
};

class Session;

class [[nodiscard]] SessionCreateResult final {
  public:
    SessionCreateResult(std::unique_ptr<Session> session, SessionDiagnostic diagnostic) noexcept
        : session_(std::move(session)), diagnostic_(std::move(diagnostic)) {}
    SessionCreateResult(SessionCreateResult&&) noexcept = default;
    SessionCreateResult& operator=(SessionCreateResult&&) noexcept = default;
    SessionCreateResult(const SessionCreateResult&) = delete;
    SessionCreateResult& operator=(const SessionCreateResult&) = delete;
    ~SessionCreateResult() = default;

    [[nodiscard]] explicit operator bool() const noexcept { return session_ != nullptr; }
    [[nodiscard]] Session* session() const noexcept { return session_.get(); }
    [[nodiscard]] const SessionDiagnostic& diagnostic() const noexcept { return diagnostic_; }
    [[nodiscard]] std::unique_ptr<Session> takeSession() && noexcept { return std::move(session_); }

  private:
    std::unique_ptr<Session> session_;
    SessionDiagnostic diagnostic_;
};

enum class SessionSaveStatus : std::uint8_t {
    Saved,
    ReadOnly,
    InvalidSession,
    Failed,
};

struct SessionSaveResult final {
    SessionSaveStatus status = SessionSaveStatus::InvalidSession;
    std::string code;
    std::string message;

    [[nodiscard]] bool succeeded() const noexcept { return status == SessionSaveStatus::Saved; }
};

struct SessionCommandResult final {
    host::ProjectSessionCommandStatus status = host::ProjectSessionCommandStatus::InvalidSession;
    std::optional<commands::CommandResult> command;
    std::optional<OperationDiagnostic> diagnostic;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == host::ProjectSessionCommandStatus::Completed && command.has_value() &&
               command->succeeded() && !diagnostic.has_value();
    }
};

// A live client borrows typed host services and immutable reads, never mutable document
// pointers. The owner revokes valid() before replacing or destroying its authoring session.
struct SessionBinding final {
    std::function<bool()> valid;
    std::function<document::Snapshot()> snapshot;
    std::function<SessionCommandResult(commands::Transaction)> execute;
    std::function<SessionCommandResult(bool)> history;
    std::shared_ptr<EventStream> events;
    host::PublicationCoordinator* publication = nullptr;
    platform::StagedArtifactCoordinator* artifacts = nullptr;
};

class Session final {
  public:
    [[nodiscard]] static SessionCreateResult
    createNew(std::string projectName = "Untitled", std::string compositionName = "Composition",
              core::RationalTime duration = core::RationalTime::fromInteger(48),
              document::CompositionFormat format = {});
    [[nodiscard]] static SessionCreateResult open(const std::filesystem::path& path);
    [[nodiscard]] static SessionCreateResult openReadOnly(const std::filesystem::path& path);
    [[nodiscard]] static SessionCreateResult attach(SessionBinding binding);
    [[nodiscard]] std::shared_ptr<EventStream> eventStream() const noexcept {
        return binding_ ? binding_->events : nullptr;
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
    ~Session() = default;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] const host::ProjectSession* hostSession() const noexcept {
        return session_.get();
    }
    [[nodiscard]] const OperationRegistry& operations() const noexcept { return registry_; }
    [[nodiscard]] host::ProjectSession* hostSession() noexcept { return session_.get(); }
    [[nodiscard]] host::ProjectSessionStateSnapshot state() const;
    [[nodiscard]] host::DecodedProjectSnapshotResult snapshotResult() const;
    [[nodiscard]] document::Snapshot snapshot() const;

    [[nodiscard]] SessionCommandResult execute(commands::Transaction transaction);
    [[nodiscard]] SessionCommandResult undo();
    [[nodiscard]] SessionCommandResult redo();
    [[nodiscard]] SessionCommandResult
    executeJsonOperation(std::string_view operation, const Arguments& arguments,
                         const std::optional<std::string>& label = {});

    [[nodiscard]] SessionSaveResult save();
    [[nodiscard]] SessionSaveResult saveAs(const std::filesystem::path& path);
    [[nodiscard]] const std::filesystem::path& displayPath() const noexcept { return displayPath_; }

    [[nodiscard]] commands::CommandStack* commandStack() noexcept;
    [[nodiscard]] document::Document* document() noexcept;
    [[nodiscard]] host::PublicationCoordinator* publicationCoordinator() noexcept {
        return binding_ ? binding_->publication : publicationCoordinator_.get();
    }
    [[nodiscard]] platform::StagedArtifactCoordinator* artifactCoordinator() noexcept {
        return binding_ ? binding_->artifacts : artifactCoordinator_.get();
    }

  private:
    friend class SessionCreateResult;
    Session(std::unique_ptr<host::ProjectSession> session,
            std::unique_ptr<host::PublicationCoordinator> publicationCoordinator,
            std::unique_ptr<platform::StagedArtifactCoordinator> artifactCoordinator,
            OperationRegistry registry, std::filesystem::path displayPath) noexcept
        : session_(std::move(session)), publicationCoordinator_(std::move(publicationCoordinator)),
          artifactCoordinator_(std::move(artifactCoordinator)), registry_(std::move(registry)),
          displayPath_(std::move(displayPath)) {}

    [[nodiscard]] static SessionCreateResult
    fromHostSession(std::unique_ptr<Session> base, host::ProjectSessionCreateResult hostResult,
                    std::filesystem::path displayPath);
    [[nodiscard]] SessionSaveResult saveTo(const std::filesystem::path& path, bool saveAs);
    [[nodiscard]] SessionCommandResult
    makeCommandResult(host::ProjectSessionCommandResult result) const;

    Session() : registry_(OperationRegistry::builtIn()) {}

    host::ProjectSessionIdentitySource identitySource_;
    std::unique_ptr<host::ProjectSession> session_;
    std::unique_ptr<host::PublicationCoordinator> publicationCoordinator_;
    std::unique_ptr<platform::StagedArtifactCoordinator> artifactCoordinator_;
    OperationRegistry registry_;
    std::filesystem::path displayPath_;
    std::optional<SessionBinding> binding_;
};

} // namespace bloom::scripting
