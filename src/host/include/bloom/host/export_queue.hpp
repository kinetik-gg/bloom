#pragma once

#include <bloom/document/export_queue.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/host/export_verification.hpp>
#include <bloom/runtime/task_types.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace bloom::output {
class ExportResourceLedgerV1;
} // namespace bloom::output
namespace bloom::platform {
class StagedArtifactCoordinator;
} // namespace bloom::platform
namespace bloom::runtime {
class SnapshotCompiler;
class TaskScheduler;
} // namespace bloom::runtime

// The export queue's runtime contract (ADR 0033, proposed). Qt-free; owned by src/host.
//
// Two kinds of state meet here and stay separate:
// - Project truth: the jobs, render settings and outputs (document::ExportQueue). The panel and
//   scripting change them only through undoable commands (src/commands, ADR 0033 "Commands") on
//   the ordinary command path. This service reads them from the session's published snapshots;
//   it never mutates the document.
// - Runtime state: run status, progress, ETA, logs, verification and the run link to the task
//   system. Owned here, published as immutable snapshots, never saved.
//
// Threading: the owner (desktop: the UI thread's coalesced completion drain; headless: the client
// thread) calls pump() and the session commands. They never block. Evaluation, encoding,
// verification and publication run as scheduler tasks. snapshot(), events() and the subscription
// are safe on any thread. Neither the snapshot nor the events carry callbacks, Qt types, or
// anything that keeps a scheduler, provider or document alive.
//
// Jobs vs Export: every task a run submits is owned by TaskOwner{TaskOwnerKind::Export, owner} and
// grouped under one TaskGroupId per run (ExportRunLink). The Jobs panel shows the run as one row
// whose progress it reads from this snapshot (jobForTaskOwner()), not from the tasks, so there is
// one progress source. Cancel, pause and retry belong to the Export panel; the Jobs row offers
// only "Show in Export".
namespace bloom::host {

class GpuExportProvider;
class ProjectSession;
class PublicationCoordinator;
class SequenceExportRetirementOwnerV1;

using document::ExportJobId;
using document::ExportOutputId;

// ---------------------------------------------------------------------------------------------
// Lifecycle

// The job state machine (ADR 0033, "Job lifecycle"):
//
//   Ready --Start--> Waiting --turn--> Preparing --> Rendering --> Verifying --> Done
//                                                                       |------> DoneWithWarnings
//   Rendering --Pause--> Pausing --frame boundary--> Paused --Resume--> Rendering
//   Waiting --Stop--> Ready                     (leaves the run order; nothing ran)
//   Preparing|Rendering|Pausing|Paused|Verifying --Stop--> Stopping --> Stopped
//   Preparing|Rendering|Verifying --error--> Failed
//   Done|DoneWithWarnings|Failed|Stopped --Retry--> Waiting
//
// Ready is also the state of every job after a project opens: run state is never saved.
enum class ExportJobState : std::uint8_t {
    Ready = 1,
    Waiting = 2,
    Preparing = 3,
    Rendering = 4,
    Pausing = 5,
    Paused = 6,
    Verifying = 7,
    Stopping = 8,
    Done = 9,
    DoneWithWarnings = 10,
    Failed = 11,
    Stopped = 12,
};

// Started and not yet terminal (includes Waiting).
[[nodiscard]] constexpr bool isActive(const ExportJobState state) noexcept {
    return state >= ExportJobState::Waiting && state <= ExportJobState::Stopping;
}
// Holds a captured document snapshot and run resources (excludes Waiting).
[[nodiscard]] constexpr bool isRunning(const ExportJobState state) noexcept {
    return state >= ExportJobState::Preparing && state <= ExportJobState::Stopping;
}
[[nodiscard]] constexpr bool isTerminal(const ExportJobState state) noexcept {
    return state >= ExportJobState::Done;
}

enum class ExportOutputState : std::uint8_t {
    Pending = 1,
    Preparing = 2,
    Rendering = 3,
    Verifying = 4,
    Published = 5,
    PublishedWithWarnings = 6,
    Failed = 7,
    Stopped = 8,
    // Disabled output, or an output after a failed one in the same job.
    Skipped = 9,
};

// How the current frames are being evaluated (ADR 0033, "GPU fair admission").
enum class ExportDeviceRoute : std::uint8_t {
    None = 0,
    Gpu = 1,
    // Admission is waiting because the Viewer has priority; it ages and is admitted within the
    // starvation bound.
    WaitingForViewer = 2,
    // The export GPU generation is being replaced after an unproven phase retirement.
    RecoveringGpu = 3,
    // Explicit CPU reference fallback (no device, or recovery failed). Recorded in provenance.
    CpuReference = 4,
};

enum class ExportRunnerState : std::uint8_t {
    Idle = 1,
    Running = 2,
    // beginShutdown() was called: new starts are refused and active jobs are stopping.
    ShuttingDown = 3,
};

struct ExportResolvedRange final {
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    friend bool operator==(const ExportResolvedRange&, const ExportResolvedRange&) = default;
};

struct ExportJobProgress final {
    // Over all enabled outputs of the job: frames completed / frames to render.
    std::uint64_t framesCompleted = 0;
    std::uint64_t framesTotal = 0;
    // Index of the output being rendered among the enabled outputs, and their count.
    std::uint32_t outputIndex = 0;
    std::uint32_t outputCount = 0;
    std::optional<std::uint64_t> currentFrame;
    // 0..1; absent while indeterminate (Preparing, Verifying, an unknown total).
    std::optional<double> fraction;
    // Excludes paused time.
    std::chrono::milliseconds elapsed{0};
    // Smoothed over recent frame completions; absent until enough frames finished to estimate.
    std::optional<std::chrono::milliseconds> remaining;
    ExportDeviceRoute route = ExportDeviceRoute::None;
};

// Bounded per-run log. Structured first; `message` is the human wording of `code`.
struct ExportLogEntry final {
    std::uint64_t sequence = 0;
    std::chrono::milliseconds sinceStart{0};
    runtime::DiagnosticSeverity severity = runtime::DiagnosticSeverity::Information;
    std::string code;
    std::optional<ExportOutputId> output;
    std::optional<std::uint64_t> frame;
    std::string message;
};

// The link from the Jobs (task system) panel to an export run.
struct ExportRunLink final {
    // Monotonic per service; a Retry starts a new run.
    std::uint64_t run = 0;
    runtime::TaskOwnerId owner;
    runtime::TaskGroupId group;
    friend bool operator==(const ExportRunLink&, const ExportRunLink&) = default;
};

enum class ExportFailureCode : std::uint8_t {
    CompositionMissing = 1,
    CompileFailed = 2,
    PreflightRefused = 3,
    // The analysis did not fit the preset's declared preservation (AnalysisPolicy check).
    AnalysisRefused = 4,
    EvaluationFailed = 5,
    // GPU admission or a resource budget refused and could not recover in time.
    ResourceRefused = 6,
    EncoderUnavailable = 7,
    EncoderFailed = 8,
    WriteFailed = 9,
    DestinationFull = 10,
    VerificationFailed = 11,
    PublicationConflict = 12,
    DeadlineExceeded = 13,
    Internal = 14,
};

struct ExportFailure final {
    ExportFailureCode code = ExportFailureCode::Internal;
    std::optional<ExportOutputId> output;
    std::optional<std::uint64_t> frame;
    // The underlying stable diagnostic code (task, attempt, provider), for support and scripts.
    std::string diagnosticCode;
    // Human-readable summary, ready for display.
    std::string reason;
};

enum class ExportStopCause : std::uint8_t {
    User = 1,
    // The job's record left the document (removed by undo, script or another edit).
    JobRemoved = 2,
    ProjectClosing = 3,
    ApplicationQuit = 4,
};

// ---------------------------------------------------------------------------------------------
// Commands and availability

// Every command the panel, menus and scripting can issue. ADR 0033's contract table lists each
// one's availability rule; exportCommandKind() says whether it changes project truth.
enum class ExportCommand : std::uint8_t {
    // Queue level.
    AddJob = 0,
    StartAll = 1,
    StopAll = 2,
    RemoveFinished = 3,
    // Job level.
    Start = 4,
    Pause = 5,
    Resume = 6,
    Stop = 7,
    Retry = 8,
    EditJob = 9, // name, range, enabled
    RemoveJob = 10,
    DuplicateJob = 11,
    MoveJob = 12,
    AddOutput = 13,
    // Output level.
    EditOutput = 14, // settings, destination, enabled
    ApplyPreset = 15,
    RemoveOutput = 16,
    RevealOutput = 17,
};
inline constexpr std::size_t kExportCommandCount = 18;

enum class ExportCommandKind : std::uint8_t {
    // An undoable document command: one undo entry, dirties the project, saved.
    ProjectTruth = 1,
    // Acts on runtime state only: not undoable, never dirties the project, never saved.
    Session = 2,
};

[[nodiscard]] constexpr ExportCommandKind exportCommandKind(const ExportCommand command) noexcept {
    switch (command) {
    case ExportCommand::AddJob:
    case ExportCommand::RemoveFinished:
    case ExportCommand::EditJob:
    case ExportCommand::RemoveJob:
    case ExportCommand::DuplicateJob:
    case ExportCommand::MoveJob:
    case ExportCommand::AddOutput:
    case ExportCommand::EditOutput:
    case ExportCommand::ApplyPreset:
    case ExportCommand::RemoveOutput:
        return ExportCommandKind::ProjectTruth;
    case ExportCommand::StartAll:
    case ExportCommand::StopAll:
    case ExportCommand::Start:
    case ExportCommand::Pause:
    case ExportCommand::Resume:
    case ExportCommand::Stop:
    case ExportCommand::Retry:
    case ExportCommand::RevealOutput:
        return ExportCommandKind::Session;
    }
    return ExportCommandKind::Session;
}

enum class ExportUnavailableCode : std::uint8_t {
    None = 0,
    // The command does not apply at this level (e.g. Pause on an output).
    NotApplicable = 1,
    ShuttingDown = 2,
    NoProject = 3,
    ReadOnlyProject = 4,
    UnknownJob = 5,
    UnknownOutput = 6,
    // Start/Retry: the job is already Waiting or running.
    AlreadyActive = 7,
    // Stop: nothing to stop.
    NotActive = 8,
    // Pause: only Rendering pauses (Preparing and Verifying are short and not resumable).
    NotRendering = 9,
    NotPaused = 10,
    // Retry: only a terminal job can run again.
    NotFinished = 11,
    NoEnabledOutputs = 12,
    // A blocking preflight issue or an unavailable preset; the reason is the issue's reason.
    PreflightBlocked = 13,
    // Edits are locked while the job runs; it keeps the settings it started with.
    LockedWhileRunning = 14,
    // A job keeps at least one output.
    LastOutput = 15,
    QueueFull = 16,
    OutputLimitReached = 17,
    NothingToStart = 18,
    NothingToStop = 19,
    NothingFinished = 20,
    // Reveal: nothing has been published at this destination in this session.
    NotPublished = 21,
    CompositionMissing = 22,
};

struct ExportCommandAvailability final {
    bool available = false;
    ExportUnavailableCode code = ExportUnavailableCode::NotApplicable;
    // Human-readable, ready for a tooltip; empty when available. Never empty when unavailable.
    std::string reason;
    [[nodiscard]] explicit operator bool() const noexcept { return available; }
};

// Availability of every command at one level. Commands that do not apply at that level answer
// NotApplicable, so a panel can index any command without a special case.
struct ExportCommandAvailabilitySet final {
    std::array<ExportCommandAvailability, kExportCommandCount> entries{};
    [[nodiscard]] const ExportCommandAvailability& operator[](const ExportCommand command) const {
        return entries[static_cast<std::size_t>(command)];
    }
};

// What a session command did.
struct ExportCommandResult final {
    bool accepted = false;
    // When refused, the availability that refused it (same code and reason as the snapshot).
    ExportCommandAvailability availability;
    // The snapshot revision that first shows the effect.
    std::uint64_t revision = 0;
};

// ---------------------------------------------------------------------------------------------
// Snapshot

struct ExportOutputView final {
    ExportOutputId id;
    // The project-truth record as of documentRevision.
    document::ExportOutput record;
    ExportOutputState state = ExportOutputState::Pending;
    // The destination with project-relative paths resolved and the first file name expanded.
    std::filesystem::path resolvedDestination;
    // The record's settings differ from the catalogue preset named by record.presetId, or that
    // preset is not on this computer.
    bool presetModified = false;
    ExportAvailability availability;
    std::vector<ExportPreflightIssue> preflight;
    // Present once the output reached a terminal output state in the current or last run.
    std::optional<ExportOutputVerification> verification;
    ExportCommandAvailabilitySet commands;
};

struct ExportJobView final {
    ExportJobId id;
    // Monotonic per job; bumps whenever anything in this view changes. Equal revisions mean equal
    // views, so the panel can skip a row.
    std::uint64_t revision = 0;
    document::ExportJob record;
    // record.name, or the composition's name when that is empty.
    std::string displayName;
    // The frames Start would render now (or the running job renders); absent when the range
    // cannot resolve (missing composition, custom range outside it).
    std::optional<ExportResolvedRange> resolvedRange;
    ExportJobState state = ExportJobState::Ready;
    ExportJobProgress progress;
    std::optional<ExportRunLink> run;
    std::optional<ExportFailure> failure;
    std::optional<ExportStopCause> stopCause;
    // The record changed (undo, script) after the run captured it; the run keeps what it captured.
    bool recordChangedSinceStart = false;
    std::vector<ExportLogEntry> log;
    std::uint64_t droppedLogEntries = 0;
    std::vector<ExportOutputView> outputs;
    ExportCommandAvailabilitySet commands;
};

struct ExportQueueSnapshot final {
    // Monotonic per service; bumps on any change below. Never reused, never decreases.
    std::uint64_t revision = 0;
    // The document revision the project-truth parts were read from.
    std::optional<std::uint64_t> documentRevision;
    std::uint64_t catalogueRevision = 0;
    ExportRunnerState runner = ExportRunnerState::Idle;
    std::optional<ExportJobId> activeJob;
    // Run order.
    std::vector<ExportJobView> jobs;
    // Queue-level commands (AddJob, StartAll, StopAll, RemoveFinished).
    ExportCommandAvailabilitySet commands;
};

// ---------------------------------------------------------------------------------------------
// Terminal events

// Typed terminal transitions so a panel, a sound or an OS-notification service can react without
// polling. The engine names what happened; it has no notion of sounds or notifications. Each
// event carries the outputs' verification, which is also in the snapshot.
struct ExportJobSucceeded final {
    ExportJobId job;
    std::uint64_t run = 0;
    // True for DoneWithWarnings.
    bool withWarnings = false;
    std::vector<ExportOutputVerification> outputs;
};

struct ExportJobFailed final {
    ExportJobId job;
    std::uint64_t run = 0;
    ExportFailure reason;
    std::vector<ExportOutputVerification> outputs;
};

struct ExportJobCancelled final {
    ExportJobId job;
    std::uint64_t run = 0;
    ExportStopCause cause = ExportStopCause::User;
    // Outputs that finished before the stop keep their results; for a sequence, frames already
    // published stay published and are counted here.
    std::vector<ExportOutputVerification> outputs;
};

// The runner went idle: every started job is terminal. Counts cover the jobs that reached a
// terminal state since the runner last left Idle.
struct ExportQueueDrained final {
    std::uint32_t succeeded = 0;
    std::uint32_t succeededWithWarnings = 0;
    std::uint32_t failed = 0;
    std::uint32_t cancelled = 0;
};

using ExportQueueEventPayload =
    std::variant<ExportJobSucceeded, ExportJobFailed, ExportJobCancelled, ExportQueueDrained>;

struct ExportQueueEvent final {
    // Monotonic per service, starting at 1.
    std::uint64_t sequence = 0;
    // The first snapshot revision that shows this transition.
    std::uint64_t revision = 0;
    ExportQueueEventPayload payload;
};

struct ExportQueueEventBatch final {
    std::vector<ExportQueueEvent> events;
    // Events after the requested sequence were dropped from the bounded ring before being read.
    // Nothing durable is lost: the snapshot still holds every job's state and verification.
    bool gap = false;
    std::uint64_t lastSequence = 0;
};

// ---------------------------------------------------------------------------------------------
// Subscription

enum class ExportQueueSubscriptionState : std::uint8_t { Open, Closed };

// A neutral wake, the same shape as runtime::TaskCompletionSubscription: the serial is the
// snapshot revision. An adapter thread waits; the UI marshals one coalesced queued drain that reads
// snapshot() and events(). No callback, payload or service lifetime is retained, so a panel can
// close at any time. close() is concurrent and idempotent; service shutdown wakes waiters.
class ExportQueueSubscription final {
  public:
    ExportQueueSubscription() = default;
    ExportQueueSubscription(const ExportQueueSubscription&) = delete;
    ExportQueueSubscription& operator=(const ExportQueueSubscription&) = delete;
    ExportQueueSubscription(ExportQueueSubscription&&) noexcept;
    ExportQueueSubscription& operator=(ExportQueueSubscription&&) noexcept;
    ~ExportQueueSubscription();

    [[nodiscard]] std::uint64_t serial() const noexcept;
    [[nodiscard]] ExportQueueSubscriptionState state() const noexcept;
    // Blocks the calling (adapter) thread until the serial differs from `observed`. nullopt
    // means closed or stop requested. Never call it on the UI thread.
    [[nodiscard]] std::optional<std::uint64_t> waitForChange(std::uint64_t observed,
                                                             const std::stop_token& stop) const;
    void close() noexcept;

  private:
    struct State;
    friend class ExportQueueService;
    explicit ExportQueueSubscription(std::shared_ptr<State>) noexcept;
    std::shared_ptr<State> state_;
};

enum class ExportQueueSubscribeStatus : std::uint8_t { Accepted, CapacityReached, Closed };

struct ExportQueueSubscribeResult final {
    ExportQueueSubscribeStatus status = ExportQueueSubscribeStatus::Closed;
    ExportQueueSubscription subscription;
};

inline constexpr std::size_t kExportQueueSubscriptionCapacity = 8;

// ---------------------------------------------------------------------------------------------
// Service

struct ExportQueueServiceLimits final {
    std::size_t logEntriesPerRun = 256;
    std::size_t retainedEvents = 256;
    // Frames that must finish before an ETA is published.
    std::uint32_t etaMinimumFrames = 3;
};

// Borrowed collaborators. All must outlive the service; the composition root passes the same
// application-wide instances the rest of the application uses (one publication truth, one GPU
// export provider, one processing root).
struct ExportQueueServiceDependencies final {
    ProjectSession& session;
    runtime::TaskScheduler& scheduler;
    const runtime::SnapshotCompiler& compiler;
    PublicationCoordinator& publications;
    platform::StagedArtifactCoordinator& artifacts;
    std::shared_ptr<output::ExportResourceLedgerV1> ledger;
    std::shared_ptr<GpuExportProvider> gpu;
    std::shared_ptr<SequenceExportRetirementOwnerV1> retirement;
    std::shared_ptr<ExportPresetCatalogue> presets;
    ExportQueueServiceLimits limits;
};

class ExportQueueService final {
  public:
    explicit ExportQueueService(ExportQueueServiceDependencies dependencies);
    ~ExportQueueService();
    ExportQueueService(const ExportQueueService&) = delete;
    ExportQueueService& operator=(const ExportQueueService&) = delete;

    // ---- Any thread.
    [[nodiscard]] std::shared_ptr<const ExportQueueSnapshot> snapshot() const;
    [[nodiscard]] ExportQueueEventBatch events(std::uint64_t afterSequence) const;
    [[nodiscard]] ExportQueueSubscribeResult subscribe();
    // The export job a Jobs-panel task belongs to (TaskOwnerKind::Export only).
    [[nodiscard]] std::optional<ExportJobId> jobForTaskOwner(runtime::TaskOwnerId owner) const;
    [[nodiscard]] ExportPresetCatalogue& presets() const noexcept;

    // ---- Owner thread (desktop: UI thread). Non-blocking.

    // Consumes finished tasks, reads the session's newest document snapshot, advances runs and
    // publishes a new snapshot when anything changed. Called from the completion drain.
    void pump();

    // Availability, identical to the published snapshot's sets but evaluated against the
    // newest document (useful right after a command, before the next pump).
    [[nodiscard]] ExportCommandAvailability availability(ExportCommand command) const;
    [[nodiscard]] ExportCommandAvailability availability(ExportJobId job,
                                                         ExportCommand command) const;
    [[nodiscard]] ExportCommandAvailability availability(ExportJobId job, ExportOutputId output,
                                                         ExportCommand command) const;
    [[nodiscard]] ExportCommandAvailability canStart(ExportJobId job) const {
        return availability(job, ExportCommand::Start);
    }

    // Session commands. Each refuses with the availability that refused it.
    ExportCommandResult startAll();
    ExportCommandResult start(std::span<const ExportJobId> jobs);
    ExportCommandResult pause(ExportJobId job);
    ExportCommandResult resume(ExportJobId job);
    ExportCommandResult stop(ExportJobId job);
    ExportCommandResult stopAll();
    ExportCommandResult retry(ExportJobId job);

    // Shutdown edge: refuses new starts and stops every active job with `cause`. Non-blocking;
    // the owner keeps pumping until quiescent(), then retires the GPU provider and the encoder
    // retirement owner as today.
    void beginShutdown(ExportStopCause cause);
    [[nodiscard]] bool quiescent() const noexcept;
    // Jobs that would be stopped by closing the project or quitting (Waiting, running, Paused).
    [[nodiscard]] std::size_t activeJobCount() const noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace bloom::host
