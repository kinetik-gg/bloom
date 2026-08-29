#pragma once

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/result.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/id.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/project.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/project/manifest_requirements.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/round_trip_state.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Design decision 1 (task A1, issue #68): ProjectSession's round-trip state is an owning
// std::shared_ptr<const project::RoundTripState> (nullable), not a std::optional<RoundTripState>.
// A replacement install (installDecodedReplacement()) swaps the session's shared_ptr; any capture
// already handed out by captureSaveInput() (SessionSaveInput::roundTripView()) holds its own
// std::shared_ptr copy, so it keeps the OLD state alive independently of the session even across a
// session-thread replacement -- this is what makes an async worker's captured view safe against a
// concurrent Save As/Open install on the authoring thread (see session_async_io.hpp's "replacement
// survives capture" contract). Public request/content types (DecodedProjectSessionRequest::
// roundTrip, DecodedReplacementContent::roundTrip_) deliberately keep their existing
// std::optional<project::RoundTripState> by-value shape -- ProjectSession itself wraps the value
// into the shared owning pointer on install/create, so no other module needs to know about the
// shared_ptr.

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
    // REQUIRED for a decoded session: the canonical writer (project::CanonicalDocumentV1) takes
    // color settings as explicit caller input by contract -- bloom::document::Project does not
    // itself own color settings. A decoded project always has a real opened value to carry
    // forward here, unlike a brand-new project (see createNew()), which always synthesizes the
    // immutable Bloom Neutral v1 built-in default (color_settings.hpp's
    // makeBloomNeutralColorSettingsV1()) rather than accepting caller input.
    document::ColorSettings colorSettings;
    DecodedProjectEditability editability = DecodedProjectEditability::Editable;
    std::optional<ProjectDisplayPath> displayPath;
    std::optional<document::IdAllocatorHighWater> persistedAllocatorHighWater;
    // Present only for a same-major newer-minor (schema {1, minor > 0}) decoded document (see
    // docs/architecture/project-format.md, "Versions, Migrations, And Preservation"); absent for
    // an exact-{1,0} decoded document. project::RoundTripState is move-only, which makes this
    // whole request move-only. A request with roundTrip present and schemaMinor == 0 is a typed
    // create failure (see createDecoded()).
    std::optional<project::RoundTripState> roundTrip = std::nullopt;
    std::uint32_t schemaMinor = 0;
    // Verbatim from the opened manifest's requirement set; empty for a new project using only
    // foundation types and no extension-owned truth. The save chain recomputes node-type coverage
    // at save time (project-format.md, "Manifest Shape"), so this is stored, never re-derived.
    std::vector<project::ManifestRequirement> retainedRequirements{};
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

// Typed statuses for ProjectSession::captureSaveInput(). ColorSettingsUnavailable is not part of
// docs/architecture/project-session.md's normative text. Historically it existed because a
// session installed through createNew() had no color settings and no qualified build-profile
// default to synthesize one; createNew() now always installs the immutable Bloom Neutral v1
// built-in color settings (see color_settings.hpp's makeBloomNeutralColorSettingsV1() and
// bloom_neutral_profile.hpp's kBloomNeutralV1ConfigDigest, issue #60), so a createNew() session
// no longer reaches this status in practice. The enumerator and its captureSaveInput() gate stay
// in place -- colorSettings_ remains std::optional on ProjectSession, and any future construction
// path that installs decoded content without color settings would still be refused here rather
// than silently saved with an absent/invalid value.
enum class SessionSaveInputStatus : std::uint8_t {
    Captured,
    ReadOnly,
    InvalidSession,
    ColorSettingsUnavailable,
    PathRequired,
    StaleIntent,
};

// One immutable capture of everything a synchronous Save needs from a ProjectSession, per
// docs/architecture/project-session.md's "Save Inputs And Intent". Every member here is a self-
// contained OWNING copy (snapshot/colorSettings/schemaMinor/retainedRequirements are by-value;
// roundTripView() is a std::shared_ptr<const project::RoundTripState> copy -- see
// project_session.hpp's design-decision-1 comment above): a SessionSaveInput is safe to move off
// the authoring thread and outlive the ProjectSession it was captured from, including across a
// later replacement install on that session. This IS the contract's "immutable round-trip view"
// (project-session.md, "Save Inputs And Intent"), reused verbatim by both the synchronous save flow
// (session_save.cpp) and the async save worker (session_async_io.cpp).
class SessionSaveInput final {
  public:
    [[nodiscard]] const document::Snapshot& snapshot() const& noexcept { return snapshot_; }
    const document::Snapshot& snapshot() const&& = delete;
    [[nodiscard]] document::Revision revision() const noexcept { return snapshot_.revision(); }
    [[nodiscard]] const document::ColorSettings& colorSettings() const& noexcept {
        return colorSettings_;
    }
    const document::ColorSettings& colorSettings() const&& = delete;
    // The owning shared view itself (nullable); copying it keeps the pointee alive independently
    // of this SessionSaveInput and of the originating ProjectSession.
    [[nodiscard]] const std::shared_ptr<const project::RoundTripState>&
    roundTripView() const& noexcept {
        return roundTrip_;
    }
    const std::shared_ptr<const project::RoundTripState>& roundTripView() const&& = delete;
    // Derived raw pointer for project::CanonicalDocumentV1::roundTrip; valid for as long as this
    // SessionSaveInput (or a roundTripView() copy taken from it) stays alive -- no longer tied to
    // the originating ProjectSession's own lifetime.
    [[nodiscard]] const project::RoundTripState* roundTrip() const noexcept {
        return roundTrip_.get();
    }
    [[nodiscard]] std::uint32_t schemaMinor() const noexcept { return schemaMinor_; }
    [[nodiscard]] const std::vector<project::ManifestRequirement>&
    retainedRequirements() const& noexcept {
        return retainedRequirements_;
    }
    const std::vector<project::ManifestRequirement>& retainedRequirements() const&& = delete;
    // Absent for a Save As replacement intent before its publication is accepted.
    [[nodiscard]] const std::optional<ProjectDisplayPath>& displayPath() const& noexcept {
        return displayPath_;
    }
    const std::optional<ProjectDisplayPath>& displayPath() const&& = delete;
    [[nodiscard]] SessionPathIntentCapture pathIntent() const noexcept { return pathIntent_; }
    [[nodiscard]] SessionResultAcceptanceCapture resultAcceptance() const noexcept {
        return resultAcceptance_;
    }

    friend class ProjectSession;

  private:
    SessionSaveInput(document::Snapshot snapshot, document::ColorSettings colorSettings,
                     std::shared_ptr<const project::RoundTripState> roundTrip,
                     std::uint32_t schemaMinor,
                     std::vector<project::ManifestRequirement> retainedRequirements,
                     std::optional<ProjectDisplayPath> displayPath,
                     SessionPathIntentCapture pathIntent,
                     SessionResultAcceptanceCapture resultAcceptance) noexcept
        : snapshot_(std::move(snapshot)), colorSettings_(std::move(colorSettings)),
          roundTrip_(std::move(roundTrip)), schemaMinor_(schemaMinor),
          retainedRequirements_(std::move(retainedRequirements)),
          displayPath_(std::move(displayPath)), pathIntent_(pathIntent),
          resultAcceptance_(resultAcceptance) {}

    document::Snapshot snapshot_;
    document::ColorSettings colorSettings_;
    std::shared_ptr<const project::RoundTripState> roundTrip_;
    std::uint32_t schemaMinor_ = 0;
    std::vector<project::ManifestRequirement> retainedRequirements_;
    std::optional<ProjectDisplayPath> displayPath_;
    SessionPathIntentCapture pathIntent_;
    SessionResultAcceptanceCapture resultAcceptance_;
};

class [[nodiscard]] SessionSaveInputResult final {
  public:
    SessionSaveInputResult(SessionSaveInputResult&&) noexcept = default;
    SessionSaveInputResult& operator=(SessionSaveInputResult&&) noexcept = default;
    SessionSaveInputResult(const SessionSaveInputResult&) = delete;
    SessionSaveInputResult& operator=(const SessionSaveInputResult&) = delete;
    ~SessionSaveInputResult() = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status_ == SessionSaveInputStatus::Captured && input_.has_value();
    }
    [[nodiscard]] SessionSaveInputStatus status() const noexcept { return status_; }
    [[nodiscard]] const SessionSaveInput* value() const& noexcept {
        return input_.has_value() ? &*input_ : nullptr;
    }
    [[nodiscard]] const SessionSaveInput* value() const&& = delete;

    // Moves the captured input out; valid only when status() == Captured (terminates otherwise,
    // mirroring ProjectSessionCreateResult::takeSession()'s established pattern). Additive: the
    // synchronous save flow never needed this (it kept `*this` alive for the call's duration and
    // used value() alone), but the async save path (session_async_io.cpp) builds a self-contained
    // owning capture that must outlive this result and the whole begin() call stack -- see this
    // task's frozen design decision 2.
    [[nodiscard]] SessionSaveInput takeValue() && noexcept {
        if (!input_.has_value()) {
            std::terminate();
        }
        return std::move(*input_);
    }

  private:
    friend class ProjectSession;

    explicit SessionSaveInputResult(const SessionSaveInputStatus status) noexcept
        : status_(status) {}
    explicit SessionSaveInputResult(SessionSaveInput input) noexcept
        : status_(SessionSaveInputStatus::Captured), input_(std::move(input)) {}

    SessionSaveInputStatus status_ = SessionSaveInputStatus::InvalidSession;
    std::optional<SessionSaveInput> input_;
};

// Owns the three project::ProjectIoMemoryReservation charges an opened archive accrued for its
// resident document/colorSettings/roundTrip/requirements state (see project::OpenedArchive's own
// class comment: "Moving storage between stages transfers its charge"). A DecodedReplacementContent
// carries this by value into ProjectSession::installDecodedReplacement(); on a successful install
// ProjectSession keeps it for as long as the installed content stays resident (charge follows
// residency -- see the task's frozen design decision 1), releasing it only when the session is
// destroyed or replaced by a later installation. Deliberately opaque: nothing outside this module
// inspects its members, it is only ever constructed once (from a real OpenedArchive, in
// session_open.cpp) and moved along verbatim.
class DecodedReplacementReservations final {
  public:
    DecodedReplacementReservations(
        project::ProjectIoMemoryReservation manifestReservation,
        project::ProjectIoMemoryReservation decodeReservation,
        project::ProjectIoMemoryReservation reconstructionReservation) noexcept
        : manifestReservation_(std::move(manifestReservation)),
          decodeReservation_(std::move(decodeReservation)),
          reconstructionReservation_(std::move(reconstructionReservation)) {}
    DecodedReplacementReservations(DecodedReplacementReservations&&) noexcept = default;
    DecodedReplacementReservations& operator=(DecodedReplacementReservations&&) noexcept = default;
    DecodedReplacementReservations(const DecodedReplacementReservations&) = delete;
    DecodedReplacementReservations& operator=(const DecodedReplacementReservations&) = delete;
    ~DecodedReplacementReservations() = default;

  private:
    project::ProjectIoMemoryReservation manifestReservation_;
    project::ProjectIoMemoryReservation decodeReservation_;
    project::ProjectIoMemoryReservation reconstructionReservation_;
};

// Typed statuses for
// ProjectSession::installDecodedReplacement()/installPreservedReadOnlyReplacement(). See
// docs/architecture/project-session.md's "Session Publication" and "Open Intent" for the contract
// this composes. RevisionChanged is the contract's edit-during-Open refusal: the decoded document
// the session currently holds was edited (or undone/redone -- revisions are monotonic, so undo/redo
// count as changes) after the Open intent was admitted, so the current project is kept.
// InvalidContent covers a structurally unusable DecodedReplacementContent (a null document, an
// unrecognized DecodedProjectEditability, or a RoundTripState paired with schemaMinor == 0 --
// mirroring createDecoded()'s own request validation exactly, since a decoded install is otherwise
// the same kind of content createDecoded() accepts).
enum class SessionInstallStatus : std::uint8_t {
    Installed,
    InvalidSession,
    StaleOpenIntent,
    AcceptanceMismatch,
    RevisionChanged,
    InvalidContent,
    RuntimeIdentityExhausted,
};

// Everything installDecodedReplacement() needs to replace a session's decoded content atomically
// (see docs/architecture/project-session.md, "Session Publication"). Move-only
// (unique_ptr<Document> and RoundTripState are each move-only, which makes this whole type
// move-only). Mirrors project::OpenedArchive's shape one-to-one (see open_archive.hpp's file
// comment) plus the two host-owned concerns OpenedArchive cannot determine on its own:
// DecodedProjectEditability (this slice always installs Editable -- see session_open.cpp) and an
// optional display path.
class DecodedReplacementContent final {
  public:
    DecodedReplacementContent(std::unique_ptr<document::Document> document,
                              document::ColorSettings colorSettings,
                              std::optional<project::RoundTripState> roundTrip,
                              std::uint32_t schemaMinor,
                              std::vector<project::ManifestRequirement> requirements,
                              DecodedProjectEditability editability,
                              std::optional<ProjectDisplayPath> displayPath,
                              std::optional<DecodedReplacementReservations> reservations) noexcept
        : document_(std::move(document)), colorSettings_(std::move(colorSettings)),
          roundTrip_(std::move(roundTrip)), schemaMinor_(schemaMinor),
          requirements_(std::move(requirements)), editability_(editability),
          displayPath_(std::move(displayPath)), reservations_(std::move(reservations)) {}
    DecodedReplacementContent(DecodedReplacementContent&&) noexcept = default;
    DecodedReplacementContent& operator=(DecodedReplacementContent&&) noexcept = default;
    DecodedReplacementContent(const DecodedReplacementContent&) = delete;
    DecodedReplacementContent& operator=(const DecodedReplacementContent&) = delete;
    ~DecodedReplacementContent() = default;

  private:
    friend class ProjectSession;

    std::unique_ptr<document::Document> document_;
    document::ColorSettings colorSettings_;
    std::optional<project::RoundTripState> roundTrip_;
    std::uint32_t schemaMinor_ = 0;
    std::vector<project::ManifestRequirement> requirements_;
    DecodedProjectEditability editability_ = DecodedProjectEditability::Editable;
    std::optional<ProjectDisplayPath> displayPath_;
    std::optional<DecodedReplacementReservations> reservations_;
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

    // Design decision 2 (task U1, issue #72): the narrow UI-only "projection binding" seam. A UI
    // projection (src/ui's CompositionSession::rebind()) needs raw, non-owning access to this
    // session's CURRENTLY installed live document/command-stack pair so it can atomically rebind
    // its own internal pointers after New/Open replaces session content -- without owning, copying,
    // or otherwise duplicating that state. Both pointers are null for preserved-read-only or
    // otherwise non-decoded content (mirrors decodedSnapshot()'s own NoDecodedDocument case).
    // Mutation of the document stays exclusively with execute()/undo()/redo(); this accessor exists
    // only so a UI projection can observe and bind to the CURRENT objects, never to bypass the
    // command stack.
    [[nodiscard]] std::pair<document::Document*, commands::CommandStack*>
    liveDocumentAndStack() noexcept;
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

    // Captures everything a synchronous Save needs (see docs/architecture/project-session.md,
    // "Save Inputs And Intent") without mutating the session. `intent` is normally either
    // capturePlainSavePathIntent() or an advancePathIntentForSaveAs() capture. See
    // SessionSaveInput's class comment for the returned round-trip pointer's lifetime contract.
    [[nodiscard]] SessionSaveInputResult captureSaveInput(SessionPathIntentCapture intent) const;

    // Atomically installs replacement session content from a successful Open (see
    // docs/architecture/project-session.md's "Session Publication" and "Open Intent"). Acceptance
    // is checked IN ORDER, all before any mutation -- see the .cpp file for the exact gate order
    // and why it differs from a literal isDesiredOpenIntent() call. Not noexcept: constructing the
    // fresh CommandStack the new content needs may throw std::bad_alloc; every allocation this
    // method performs happens before its first mutation of session state, so a thrown exception
    // leaves the session completely untouched (strong exception guarantee) -- see the .cpp file.
    [[nodiscard]] SessionInstallStatus installDecodedReplacement(OpenIntentCapture intent,
                                                                 DecodedReplacementContent content);
    // Same acceptance gates as installDecodedReplacement(); on success the session's content
    // becomes preserved-read-only at `displayPath`, mirroring createPreservedReadOnly()'s internal
    // state (no document, no command stack, no color
    // settings/round-trip/requirements/reservations).
    [[nodiscard]] SessionInstallStatus
    installPreservedReadOnlyReplacement(OpenIntentCapture intent, ProjectDisplayPath displayPath);

  private:
    friend class ProjectSessionCreateResult;
    friend class ProjectSessionTestAccess;

    // Not noexcept: wrapping a present `roundTrip` into the session's owning
    // std::shared_ptr<const project::RoundTripState> (design decision 1) allocates a control block,
    // which may throw std::bad_alloc. Both callers (createNew()/createDecoded()) already wrap their
    // entire construction in a try/catch for exactly this reason.
    ProjectSession(ProjectSessionId projectSessionId, std::unique_ptr<document::Document> document,
                   std::unique_ptr<commands::CommandStack> commandStack,
                   DecodedProjectEditability editability, document::Revision cleanRevision,
                   std::optional<ProjectDisplayPath> displayPath,
                   std::optional<document::ColorSettings> colorSettings,
                   std::optional<project::RoundTripState> roundTrip, std::uint32_t schemaMinor,
                   std::vector<project::ManifestRequirement> retainedRequirements);
    ProjectSession(ProjectSessionId projectSessionId,
                   ProjectDisplayPath preservedDisplayPath) noexcept;

    [[nodiscard]] ProjectSessionCommandResult unavailableCommandResult() const noexcept;
    [[nodiscard]] SessionResultAcceptanceAdvanceStatus
    advanceResultAcceptanceForInstalledReplacement() noexcept;
    // Shared install acceptance gates for installDecodedReplacement()/
    // installPreservedReadOnlyReplacement(): std::nullopt means every gate passed (proceed);
    // otherwise the returned status is the exact typed refusal to report. See the .cpp file for
    // the gate order and its documented deviation from isDesiredOpenIntent()'s bundled boolean.
    [[nodiscard]] std::optional<SessionInstallStatus>
    checkInstallAcceptanceGates(OpenIntentCapture intent) const noexcept;
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
    // Present for a createNew() session (the immutable Bloom Neutral v1 built-in, installed
    // unconditionally -- see project_session.cpp's createNew()) and always present for a
    // createDecoded() session (required by that request). Stays std::optional -- see
    // SessionSaveInputStatus::ColorSettingsUnavailable's comment above -- because
    // captureSaveInput() must still refuse cleanly rather than save an absent value if some
    // future construction path is added that does not supply one.
    std::optional<document::ColorSettings> colorSettings_;
    // Owning, nullable, shared (design decision 1 -- see the file-level comment above): swapped
    // wholesale on a replacement install; a shared_ptr copy already handed out by
    // captureSaveInput() (SessionSaveInput::roundTripView()) keeps the OLD pointee alive
    // independently of this member's own later reassignment.
    std::shared_ptr<const project::RoundTripState> roundTrip_;
    std::uint32_t schemaMinor_ = 0;
    std::vector<project::ManifestRequirement> retainedRequirements_;
    // Present only for content installed via installDecodedReplacement() from a real opened
    // archive (see DecodedReplacementReservations' class comment: charge follows residency).
    // Absent for createNew()/createDecoded() sessions and for preserved-read-only content.
    std::optional<DecodedReplacementReservations> decodedContentReservations_;
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
