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
class QualifiedDisplayProcessorProvider;
class SnapshotCompiler;
class TaskScheduler;
} // namespace bloom::runtime

// The export queue's runtime contract (ADR 0033). Qt-free; owned by src/host.
//
// Two kinds of state meet here and stay separate:
// - Project truth: the jobs, render settings and outputs (document::ExportQueue). The panel and
//   scripting change them only through undoable commands (src/commands) on the ordinary command
//   path. This service reads them from the session's published snapshots and never edits them,
//   except for the undoable asset import an output explicitly asks for (importIntoAssets).
// - Runtime state: run status, progress, ETA, logs, preflight, replace confirmations, verification,
//   import results and the run link to the task system. Owned here, never saved.
//
// Threading guarantee. Every member the panel calls -- snapshot(), events(), log(),
// availability(), canStart(), the session commands, pump(), and the catalogue's defaultPreset()
// and availabilityFor() -- is non-blocking and performs no I/O on the calling thread. Destination
// facts (exists, writable, free space, existing-file fingerprint) are sampled on a blocking-I/O
// worker and cached; evaluation, encoding, verification, publication and import preparation run as
// scheduler tasks.
//
// Change notification. Only pump() publishes a new snapshot, so the revision visible to the UI
// thread advances in exactly one place and the UI never sees half-applied state. A session command
// records its intent and then runs pump() before returning, so its effect is published atomically
// by that pump. In the desktop application one application-lifetime Qt adapter (not a panel; panels
// are replaceable and may be closed or duplicated) calls pump() from
// TaskUiBridge::resultsAvailable, the coalesced completion drain every task consumer already uses,
// and re-emits a "queue changed" signal the Export panels and the Jobs row read. Every export task,
// preflight sample and import preparation is a scheduler task, so its completion wakes that drain.
// ExportQueueSubscription is for worker threads (scripting and MCP waiters); the UI thread never
// calls waitForChange().
//
// Jobs vs Export: every task a run submits is owned by TaskOwner{TaskOwnerKind::Export, owner} and
// grouped under one TaskGroupId per run (ExportRunLink). The Jobs panel shows the run as one row
// whose progress it reads from this snapshot (jobForTaskOwner()), not from the tasks, so there is
// one progress source. Cancel, pause and retry belong to the Export panel; the Jobs row offers
// only "Show in Export".
//
// Platforms: export runs on Linux in this version. On macOS (#371) and Windows (#370) the queue is
// fully editable and Start, Start All and Retry are unavailable with PlatformNotSupported.
namespace bloom::host {

class ExportRunBackend;
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
// Ready is also the state of every job after a project opens: run state is never saved. A job is
// terminal only after every requested asset import has resolved.
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

// How the current frames are being evaluated (ADR 0033, "GPU admission with strict Viewer
// priority").
enum class ExportDeviceRoute : std::uint8_t {
    None = 0,
    Gpu = 1,
    // The Viewer is active (a frame request, playback or RAM preview) and there is no spare GPU
    // capacity. There is no minimum export rate while the Viewer is active; full speed returns when
    // the Viewer is idle. Time spent here does not count toward the export's deadlines.
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
    // Smoothed over recent frame completions; absent until enough frames finished to estimate, and
    // while the route is WaitingForViewer (no honest estimate exists then).
    std::optional<std::chrono::milliseconds> remaining;
    ExportDeviceRoute route = ExportDeviceRoute::None;
};

// One entry of a run's bounded log. Structured first; `message` is the human wording of `code`.
struct ExportLogEntry final {
    std::uint64_t sequence = 0;
    std::chrono::milliseconds sinceStart{0};
    runtime::DiagnosticSeverity severity = runtime::DiagnosticSeverity::Information;
    std::string code;
    std::optional<ExportOutputId> output;
    std::optional<std::uint64_t> frame;
    std::string message;
};

struct ExportLogBatch final {
    std::vector<ExportLogEntry> entries;
    // Entries after the requested sequence were dropped from the bounded log before being read.
    bool gap = false;
    std::uint64_t lastSequence = 0;
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
    // GPU admission or a resource budget refused and could not recover.
    ResourceRefused = 6,
    EncoderUnavailable = 7,
    EncoderFailed = 8,
    WriteFailed = 9,
    DestinationFull = 10,
    VerificationFailed = 11,
    // The destination changed after it was checked (or after replacement was confirmed).
    PublicationConflict = 12,
    DeadlineExceeded = 13,
    Internal = 14,
};

struct ExportFailure final {
    ExportFailureCode code = ExportFailureCode::Internal;
    std::optional<ExportOutputId> output;
    std::optional<std::uint64_t> frame;
    // The underlying stable diagnostic code (task, attempt, provider), for support and scripts.
    // Never shown as the failure text; stage, payload and facet codes go to the job's log.
    std::string diagnosticCode;
    // The plain sentence that explains this failure ("Bloom couldn't check frame 12 before
    // encoding, so the export stopped. ..."), ready for display. No stage or code numbers.
    std::string reason;
    // A short plain headline for the failure code (exportFailureSummary(code)), for a toast,
    // notification or row title; `reason` is the detail sentence.
    std::string summary;
};

// The headline for a failure code, e.g. "The destination ran out of space."
[[nodiscard]] std::string_view exportFailureSummary(ExportFailureCode code) noexcept;

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
    EditJob = 9, // name, range, enabled, label colour
    RemoveJob = 10,
    DuplicateJob = 11,
    MoveJob = 12,
    AddOutput = 13,
    // Output level.
    EditOutput = 14, // settings, destination, enabled, import into Assets
    ApplyPreset = 15,
    RemoveOutput = 16,
    RevealOutput = 17,
    ConfirmReplace = 18,
};
inline constexpr std::size_t kExportCommandCount = 19;

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
    case ExportCommand::ConfirmReplace:
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
    // Export runs on Linux in this version (macOS #371, Windows #370).
    PlatformNotSupported = 23,
    // An existing destination under AskBeforeReplacing has not been confirmed.
    NeedsReplaceConfirmation = 24,
    // ConfirmReplace: the destination does not exist, is already confirmed, or the policy does
    // not ask.
    NothingToConfirm = 25,
    // The worker has not sampled the destination yet ("Checking destination…").
    DestinationCheckPending = 26,
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
    // The snapshot revision published by the pump() the command ran before returning.
    std::uint64_t revision = 0;
};

// ---------------------------------------------------------------------------------------------
// Destinations (pure)

// The facts a destination expands against. Supplied by the caller so resolution is pure: no clock,
// filesystem or session is read.
struct ExportDestinationContext final {
    // The project file's folder; absent for an unsaved project (a project-relative destination
    // then resolves to nothing, with a DestinationMissing issue).
    std::optional<std::filesystem::path> projectFolder;
    std::string compositionName;
    // The frames the job renders; absent when the range cannot resolve.
    std::optional<ExportResolvedRange> range;
    // `{date}` as local calendar date. A run freezes it when it starts.
    std::uint16_t year = 1970;
    std::uint8_t month = 1;
    std::uint8_t day = 1;
};

struct ExportResolvedDestination final {
    // The expanded destination before sequence naming (a sequence's `<base>`/`<ext>` source);
    // equal to firstPath for a single file.
    std::filesystem::path basePath;
    // The first (or only) file the output writes, absolute.
    std::filesystem::path firstPath;
    // The last file of a sequence; equal to firstPath for a single file.
    std::filesystem::path lastPath;
    std::uint64_t fileCount = 0;
    // Blocking issues (NamingInvalid, DestinationMissing) when the tokens cannot resolve.
    std::vector<ExportPreflightIssue> issues;
};

// Expands an output's destination for live preview and for the run. Tokens, in the path and in a
// sequence's naming pattern: `{name}` composition name, `{job}` the job's display name, `{format}`
// the format's short name ("mp4", "exr", "prores"), `{date}` YYYY-MM-DD, `<base>`/`<ext>` the
// destination's stem and extension, and for sequences exactly one `####` or `{frame}` padded to
// the naming's frame padding (widened when the range needs more). Token values are sanitized for
// file names; an unknown token, a path separator from a value, or a changed extension is a
// NamingInvalid issue. Pure: non-blocking, no I/O, any thread.
[[nodiscard]] ExportResolvedDestination resolveDestination(const document::ExportJob& job,
                                                           const document::ExportOutput& output,
                                                           const ExportDestinationContext& context);

// ---------------------------------------------------------------------------------------------
// Copy as command (pure)

enum class ExportScriptFormat : std::uint8_t {
    // A bloom-cli JSON transaction of bloom.export.* operations followed by an export.start call.
    CliJson = 1,
    // The same through the Python package.
    Python = 2,
};

// Emits a script that recreates `job` (its render settings, every output and its settings) as a
// new job and starts it. Ids are not copied; the script uses the ids the transaction returns.
// Pure: non-blocking, no I/O, any thread.
[[nodiscard]] std::string exportJobScript(const document::ExportJob& job,
                                          ExportScriptFormat format);

// ---------------------------------------------------------------------------------------------
// Snapshot

struct ExportOutputView final {
    ExportOutputId id;
    // The project-truth record as of documentRevision.
    document::ExportOutput record;
    ExportOutputState state = ExportOutputState::Pending;
    // resolveDestination() against the current project, composition and today's date (the run's
    // frozen date while it runs).
    ExportResolvedDestination destination;
    // The record's settings differ from the catalogue preset named by record.presetId, or that
    // preset is not on this computer.
    bool presetModified = false;
    ExportAvailability availability;
    // Preflight from the cached worker sample plus pure checks; includes DestinationCheckPending
    // until the first sample lands.
    std::vector<ExportPreflightIssue> preflight;
    // The artist confirmed replacing the existing destination as it was sampled. Cleared when the
    // destination, its settings or the sampled file change; consumed by the run that replaces it.
    bool replaceConfirmed = false;
    // Size estimate from format, profile or bit rate, extent and frame count; absent when the
    // format's size cannot be estimated honestly (e.g. compressed EXR).
    std::optional<std::uint64_t> estimatedBytes;
    // Free space at the destination's volume, sampled on a worker; absent until sampled or when
    // the platform cannot report it.
    std::optional<std::uint64_t> freeBytesAtDestination;
    // Present once the output reached a terminal output state in the current or last run.
    std::optional<ExportOutputVerification> verification;
    // "Import into Assets after render" for the current or last run.
    ExportAssetImportResult import;
    ExportCommandAvailabilitySet commands;
};

struct ExportJobView final {
    ExportJobId id;
    // A transient job (submitTransient()): runtime only, never saved in the project, removed from
    // the snapshot after acknowledgeTransient() once it is terminal.
    bool transient = false;
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
    // The log itself is read with ExportQueueService::log(); the snapshot carries only its size,
    // its newest sequence and its newest entry.
    std::uint64_t logEntryCount = 0;
    std::uint64_t lastLogSequence = 0;
    std::optional<ExportLogEntry> latestLogEntry;
    // Job-level preflight (ADR 0033 section 7): issues whose target is the Job, its Composition
    // or its Range (composition missing, range outside the composition, no enabled output). Each
    // output's own issues (destination, naming, encoder, colour) stay on ExportOutputView.
    std::vector<ExportPreflightIssue> preflight;
    std::vector<ExportOutputView> outputs;
    ExportCommandAvailabilitySet commands;
};

struct ExportQueueSnapshot final {
    // Monotonic per service; advanced only by pump(). Never reused, never decreases.
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
// polling. The engine names what happened; it has no notion of sounds or notifications. Each job
// event carries its outputs' verification and, separately, their asset import results: an import
// failure never turns a succeeded export into a failure.
struct ExportJobSucceeded final {
    ExportJobId job;
    std::uint64_t run = 0;
    // True for DoneWithWarnings.
    bool withWarnings = false;
    std::vector<ExportOutputVerification> outputs;
    std::vector<ExportAssetImportResult> imports;
};

struct ExportJobFailed final {
    ExportJobId job;
    std::uint64_t run = 0;
    ExportFailure reason;
    std::vector<ExportOutputVerification> outputs;
    // Outputs that published before the failure still import when they asked to.
    std::vector<ExportAssetImportResult> imports;
};

struct ExportJobCancelled final {
    ExportJobId job;
    std::uint64_t run = 0;
    ExportStopCause cause = ExportStopCause::User;
    // Outputs that finished before the stop keep their results; for a sequence, frames already
    // published stay published and are counted here.
    std::vector<ExportOutputVerification> outputs;
    std::vector<ExportAssetImportResult> imports;
};

// The runner went idle: every started job is terminal. Counts cover the jobs that reached a
// terminal state since the runner last left Idle.
struct ExportQueueDrained final {
    std::uint32_t succeeded = 0;
    std::uint32_t succeededWithWarnings = 0;
    std::uint32_t failed = 0;
    std::uint32_t cancelled = 0;
    std::uint32_t importFailures = 0;
};

using ExportQueueEventPayload =
    std::variant<ExportJobSucceeded, ExportJobFailed, ExportJobCancelled, ExportQueueDrained>;

struct ExportQueueEvent final {
    // Monotonic per service, starting at 1.
    std::uint64_t sequence = 0;
    // The snapshot revision whose pump() published this transition.
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
// Subscription (worker threads)

enum class ExportQueueSubscriptionState : std::uint8_t { Open, Closed };

// A neutral wake for worker threads (scripting and MCP waiters), the same shape as
// runtime::TaskCompletionSubscription: the serial is the snapshot revision, advanced only by
// pump(). The desktop UI does not use it; it reacts to the completion drain that runs pump(). No
// callback, payload or service lifetime is retained. close() is concurrent and idempotent;
// service shutdown wakes waiters.
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
    // Blocks the calling worker thread until the serial differs from `observed`. nullopt means
    // closed or stop requested. Never call it on the UI thread.
    [[nodiscard]] std::optional<std::uint64_t> waitForChange(std::uint64_t observed,
                                                             const std::stop_token& stop) const;
    void close() noexcept;

    // Opaque implementation state, shared with the service that wakes it.
    struct State;

  private:
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
// Transient jobs (ADR 0033 section 13; the render, export and capture tools of scripting and
// MCP). A transient job is one composition, one range (a single frame for a still or capture),
// one output and a destination. It is never saved in the project and never enters undo; it is a
// runtime job with the same preflight, verification, GPU priority, replacement policy (an
// existing destination is never replaced silently: pass Replace explicitly, or the run refuses)
// and PlatformNotSupported behaviour as any other job. Its tasks are owned and grouped like any
// run, it stops with stop(job), and it leaves the snapshot once it is terminal and the caller has
// acknowledged its result.

struct ExportTransientJobRequest final {
    document::CompositionId composition;
    document::ExportRange range;
    document::ExportOutputSettings settings;
    document::ExportDestination destination;
    // Shown as the job's name; empty shows the composition's name.
    std::string name;
    std::string presetId;
};

struct ExportTransientSubmitResult final {
    bool accepted = false;
    // When refused, why (the same codes and reasons the queue's Start uses).
    ExportCommandAvailability availability;
    std::optional<ExportJobId> job;
    std::optional<ExportOutputId> output;
    std::uint64_t revision = 0;
};

// ---------------------------------------------------------------------------------------------
// Service

struct ExportQueueServiceLimits final {
    std::size_t logEntriesPerRun = 1024;
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
    // Optional application-wide qualified display processor (the Viewer's); PNG reuses it.
    runtime::QualifiedDisplayProcessorProvider* displayProcessor = nullptr;
    // Caller-owned scratch folder for publication's bridging files (never a destination).
    std::filesystem::path scratchDirectory;
    // The runner seam. Null uses makeProductionExportRunBackend(); tests pass a fake.
    std::shared_ptr<ExportRunBackend> backend;
};

class ExportQueueService final {
  public:
    explicit ExportQueueService(ExportQueueServiceDependencies dependencies);
    ~ExportQueueService();
    ExportQueueService(const ExportQueueService&) = delete;
    ExportQueueService& operator=(const ExportQueueService&) = delete;

    // ---- Any thread. Non-blocking, no I/O.
    [[nodiscard]] std::shared_ptr<const ExportQueueSnapshot> snapshot() const;
    [[nodiscard]] ExportQueueEventBatch events(std::uint64_t afterSequence) const;
    // The job's current (or last) run log after `afterSequence`.
    [[nodiscard]] ExportLogBatch log(ExportJobId job, std::uint64_t afterSequence) const;
    [[nodiscard]] ExportQueueSubscribeResult subscribe();
    // The export job a Jobs-panel task belongs to (TaskOwnerKind::Export only).
    [[nodiscard]] std::optional<ExportJobId> jobForTaskOwner(runtime::TaskOwnerId owner) const;
    [[nodiscard]] ExportPresetCatalogue& presets() const noexcept;

    // ---- Owner thread (desktop: UI thread). Non-blocking, no I/O.

    // Consumes finished tasks and worker samples, reads the session's newest document snapshot,
    // advances runs, applies ready asset imports, schedules destination re-sampling for changed
    // outputs, and publishes a new snapshot when anything changed. The only publisher.
    void pump();

    // Availability, identical to the snapshot published by the last pump() (it does not read a
    // newer document; the snapshot and the answers never disagree).
    [[nodiscard]] ExportCommandAvailability availability(ExportCommand command) const;
    [[nodiscard]] ExportCommandAvailability availability(ExportJobId job,
                                                         ExportCommand command) const;
    [[nodiscard]] ExportCommandAvailability availability(ExportJobId job, ExportOutputId output,
                                                         ExportCommand command) const;
    [[nodiscard]] ExportCommandAvailability canStart(ExportJobId job) const {
        return availability(job, ExportCommand::Start);
    }

    // Session commands. Each records its intent, runs pump(), and returns; a refusal carries the
    // availability that refused it.
    ExportCommandResult startAll();
    ExportCommandResult start(std::span<const ExportJobId> jobs);
    ExportCommandResult pause(ExportJobId job);
    ExportCommandResult resume(ExportJobId job);
    ExportCommandResult stop(ExportJobId job);
    ExportCommandResult stopAll();
    ExportCommandResult retry(ExportJobId job);
    // Confirms replacing the output's existing destination exactly as last sampled (its
    // fingerprint, or for a sequence the set of existing files). The run still refuses to replace
    // anything that changed after the sample (ExternalModificationConflict). A file this session
    // itself published for the output needs no confirmation on a re-run.
    ExportCommandResult confirmReplace(ExportJobId job, ExportOutputId output);
    // Transient jobs (see above). submitTransient() validates, enqueues and pumps; the job starts
    // in queue order. acknowledgeTransient() marks a terminal transient job's result as read; it
    // leaves the snapshot on that pump. False when the job is unknown, not transient or active.
    ExportTransientSubmitResult submitTransient(ExportTransientJobRequest request);
    bool acknowledgeTransient(ExportJobId job);
    // Asks the worker to re-sample destinations now (after the artist created a folder outside
    // Bloom, for example). Non-blocking.
    void refreshPreflight();
    // While an interactive edit gesture is open, ready asset imports wait (section 8). The owner
    // reports the gesture state; imports apply on the first pump after it closes.
    void setInteractiveGestureActive(bool active);

    // Shutdown edge: refuses new starts, stops every active job with `cause` and cancels the
    // preflight samples. Non-blocking; the owner keeps pumping until quiescent(), then retires the
    // GPU provider and the encoder retirement owner as today. Once quiescent() no task this
    // service submitted is still running, so the owner may destroy it and create a new service on
    // the same scheduler and session (File > New): the new service's task owners never repeat the
    // old one's, and destroying the old service closes its subscriptions.
    void beginShutdown(ExportStopCause cause);
    [[nodiscard]] bool quiescent() const noexcept;
    // Jobs that would be stopped by closing the project or quitting (Waiting, running, Paused).
    [[nodiscard]] std::size_t activeJobCount() const noexcept;

    // Opaque implementation state (src/host only).
    struct State;

  private:
    std::unique_ptr<State> state_;
};

} // namespace bloom::host
