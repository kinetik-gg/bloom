#pragma once

#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/save_publication.hpp>
#include <bloom/host/session_open.hpp>
#include <bloom/host/session_save.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

// Asynchronous session Save/Open over the blocking-I/O lane (task A1, issue #68). Both handles run
// the SAME code the synchronous session_save.cpp/session_open.cpp entry points run -- the
// session-free "middles" (executeSessionSaveMiddle() / project::openProjectArchive()) plus, on
// completion, the exact shared "accept"/"install" tails (acceptSessionSavePublication() /
// installOpenedArchiveResult()) -- as a bloom::runtime::TaskScheduler TaskExecutor::BlockingIo
// task.
//
// ---------------------------------------------------------------------------------------------
// Threading rules (both AsyncSessionSave and AsyncSessionOpen)
// ---------------------------------------------------------------------------------------------
// - begin*(): authoring thread only. Runs admission (captureSaveInput()/admitOpenIntent()) on the
//   calling thread synchronously; a refusal is a typed begin-failure and no task is submitted.
// - isReady() / tryComplete() / handle destruction: authoring thread only. tryComplete() applies
//   its session-dependent tail (acceptSavepoint() / installDecodedReplacement()) against whatever
//   ProjectSession& is passed in AT THAT CALL -- which may legitimately differ in state from
//   begin-time; the existing intent/generation checks inside acceptSavepoint()/install*() decide,
//   not this module.
// - requestCancellation(): may be called from ANY thread at any time (including from a destructor
//   racing with an authoring-thread caller, as long as the handle itself is not used concurrently
//   from two threads -- it is not internally synchronized beyond the scheduler's own submission
//   state, exactly as bloom::runtime::TaskHandle documents for itself).
// - `session`, `coordinator`, `artifacts` (Save) and `session` (Open) are caller-owned references
//   captured by the worker closure; they must outlive the whole asynchronous operation (until
//   tryComplete() returns a result or the handle is destroyed and the scheduler reaches
//   quiescence for that task), not merely the begin*() call -- unlike the synchronous entry points,
//   whose composed calls all run and return before begin*() returns.
// - `request.preAdmitted` (Save only) is the one exception to "caller keeps ownership": when
//   non-null, beginSessionSave() MOVES *request.preAdmitted into the handle's own owned storage
//   before it returns (PublicationAdmission is move-only RAII; owning it in the closure means an
//   unstarted or rejected task abandons it correctly on destruction, exactly like a caller-side
//   admission that never registers). After beginSessionSave() returns, `*request.preAdmitted` is
//   moved-from and the caller has no further lifetime obligation toward it -- unlike the
//   synchronous saveProjectSession(), whose `SessionSaveRequest::preAdmitted` pointer must merely
//   outlive that one same-frame call, an async worker consumes the pointee at an unbounded later
//   time on another thread, so a borrowed pointer would dangle against the caller's natural
//   "admit, then let the admission go out of scope" pattern.
//
// ---------------------------------------------------------------------------------------------
// Cancellation bridge
// ---------------------------------------------------------------------------------------------
// Each handle owns one std::atomic_bool (inside a small heap-allocated shared state also reachable
// from the worker closure) that IS the `const std::atomic_bool* cancellationFlag` the synchronous
// executor primitives already accept (SavePublicationRequest::cancellationFlag).
// requestCancellation() writes that atomic directly (release order), so a cancellation requested
// WHILE executeSessionSaveMiddle()/executeSavePublication() is already running on the worker thread
// is observed at its very next between-stage check -- no different in kind from the synchronous
// flow handing it the same kind of flag. In addition, at worker entry (before doing any real work)
// the worker checks bloom::runtime::TaskContext::isCancellationRequested() (the scheduler's own
// CancellationToken, which handle.cancel() -- also invoked by requestCancellation() -- and any
// other scheduler-level cancellation source such as owner cancellation or beginShutdown() all set)
// and, if set, also writes it into the same atomic before proceeding. This entry check is the ONLY
// point where a cancellation source other than this handle's own requestCancellation() can reach
// the executor flag: once the worker is running, only requestCancellation()'s direct write is
// observed (the worker does not re-poll the scheduler token mid-flight).
// project::openProjectArchive() (the Open middle) has no internal checkpoints of its own at all --
// it is a single non-cancellable call
// -- so AsyncSessionOpen's bridge only ever has an effect at that one entry checkpoint:
// cancellation requested before the worker starts skips calling openProjectArchive() entirely
// (SessionOpenStage::NotOpened / CancelledBeforeOpening); cancellation requested after the worker
// has already begun that call is not observed until it returns (openProjectArchive() takes no
// cancellation parameter and this task's non-goals forbid changing it or the scheduler/runtime
// modules).
//
// ---------------------------------------------------------------------------------------------
// Readiness
// ---------------------------------------------------------------------------------------------
// isReady() calls bloom::runtime::TaskScheduler::snapshot(handle.id()) and checks
// bloom::runtime::isTerminal(snapshot->state) -- the same non-blocking, non-consuming observability
// the scheduler already publishes to any consumer (its own tests read task lifecycle the identical
// way). This avoids a second, module-owned readiness channel and any polling loop internal to this
// module; the caller is the one who polls (in a bounded spin), exactly as the task's test plan
// requires.
namespace bloom::host {

namespace detail {

struct AsyncSessionSaveSharedState final {
    // The executor-side cancellation flag bridged from the scheduler's own CancellationToken (see
    // the cancellation-bridge documentation above).
    std::atomic_bool cancellationFlag{false};
    // Populated exactly once by the worker before it returns (TaskResult<void>::succeeded() is
    // always what the worker itself reports at the scheduler level; the REAL outcome lives here).
    // The worker's write happens-before the scheduler ever reports the task terminal (the same
    // internal synchronization TaskHandle::tryTakeResult()/TaskScheduler::snapshot() rely on for
    // every other typed task result), so reading this after isReady() observes a terminal state, or
    // after tryTakeResult() itself returns a value, is safe without additional synchronization
    // here.
    std::optional<SavePublicationResult> publication;
    // The caller's SessionSaveRequest::preAdmitted, MOVED here on the authoring thread by
    // beginSessionSave() before task submission (see the file-level threading-rules "preAdmitted"
    // note). Held in the shared state -- rather than captured by value directly in the worker
    // closure -- because PublicationAdmission is move-only and std::function (which
    // bloom::runtime::TaskFunction is built on) requires its target to stay copy-constructible;
    // std::shared_ptr<AsyncSessionSaveSharedState> itself is trivially copyable regardless of what
    // it points to. Written once, before submission (so before the worker thread could ever
    // observe `shared`); read/consumed at most once, by the worker.
    std::optional<PublicationAdmission> preAdmitted;
};

struct AsyncSessionOpenSharedState final {
    std::atomic_bool cancellationFlag{false};
    // Set by the worker instead of `opened` when cancellation was observed at entry (see the
    // cancellation-bridge documentation above): the worker never called
    // project::openProjectArchive().
    bool cancelledBeforeOpening = false;
    std::optional<project::OpenArchiveResult> opened;
};

} // namespace detail

// ================================================================================================
// Save
// ================================================================================================

enum class AsyncSessionSaveBeginStage : std::uint8_t { Capture, Submission };

class AsyncSessionSaveResult;

class AsyncSessionSave final {
  public:
    AsyncSessionSave(AsyncSessionSave&&) noexcept = default;
    AsyncSessionSave& operator=(AsyncSessionSave&&) noexcept = default;
    AsyncSessionSave(const AsyncSessionSave&) = delete;
    AsyncSessionSave& operator=(const AsyncSessionSave&) = delete;
    // Typed abandonment (see docs/architecture/project-session.md's late-completion staleness
    // rules, extended here to an in-flight worker): requests cancellation, then drops the handle
    // (and, once the scheduler reclaims it, the worker's result) without touching any session. A
    // save whose executeSavePublication() had ALREADY entered its non-cancellable platform
    // publication lease before this destructor runs may still have durably published remotely --
    // that publication is real and stays real, but the caller who abandons the handle here forfeits
    // ever recording the resulting savepoint against a session (nothing calls acceptSavepoint() for
    // it), exactly as a late/stale completion the caller never observes would be under the
    // synchronous contract.
    ~AsyncSessionSave();

    // Non-blocking, non-consuming poll; see the file-level "Readiness" documentation.
    [[nodiscard]] bool isReady() const noexcept;
    // May be called from any thread, at any time, including after tryComplete() has already
    // returned a result (a no-op then). See the file-level cancellation-bridge documentation.
    void requestCancellation() noexcept;
    // Returns nullopt while the worker has not yet reached a scheduler-terminal state. Once it has,
    // returns the full result exactly once (subsequent calls return nullopt) -- applying
    // acceptSessionSavePublication() (the same tail saveProjectSession() itself runs) against
    // `session`, which may legitimately differ in state from begin-time (see the file-level
    // threading-rules documentation).
    [[nodiscard]] std::optional<SessionSaveResult> tryComplete(ProjectSession& session);

  private:
    friend class AsyncSessionSaveResult;
    friend AsyncSessionSaveResult beginSessionSave(ProjectSession&, runtime::TaskScheduler&,
                                                   PublicationCoordinator&,
                                                   platform::StagedArtifactCoordinator&,
                                                   const SessionSaveRequest&,
                                                   project::ProjectIoOperationMemory);

    AsyncSessionSave(runtime::TaskScheduler& scheduler, runtime::TaskHandle<void> handle,
                     std::shared_ptr<detail::AsyncSessionSaveSharedState> shared,
                     SessionPathIntentCapture intent, document::Revision revision,
                     std::filesystem::path targetPath) noexcept;

    runtime::TaskScheduler* scheduler_ = nullptr;
    runtime::TaskHandle<void> handle_;
    std::shared_ptr<detail::AsyncSessionSaveSharedState> shared_;
    SessionPathIntentCapture intent_;
    document::Revision revision_;
    std::filesystem::path targetPath_;
    bool completed_ = false;
};

// [[nodiscard]] begin-outcome: either a live handle (Submission stage, operator bool() true) or a
// typed begin-failure at Capture stage (session.captureSaveInput()'s own refusal, or a wrapped
// exception from building the owning input -- see SessionSaveResourceExhausted/
// SessionSaveUnexpectedFailure in session_save.hpp) or Submission stage (the scheduler's own typed
// submission refusal). Capture refusals surface HERE, on the authoring thread, exactly as the
// synchronous saveProjectSession()'s Capture stage does -- no task is ever submitted for them.
class [[nodiscard]] AsyncSessionSaveResult final {
  public:
    AsyncSessionSaveResult(AsyncSessionSaveResult&&) noexcept = default;
    AsyncSessionSaveResult& operator=(AsyncSessionSaveResult&&) noexcept = default;
    AsyncSessionSaveResult(const AsyncSessionSaveResult&) = delete;
    AsyncSessionSaveResult& operator=(const AsyncSessionSaveResult&) = delete;
    ~AsyncSessionSaveResult() = default;

    [[nodiscard]] explicit operator bool() const noexcept { return handle_.has_value(); }
    [[nodiscard]] AsyncSessionSaveBeginStage stage() const noexcept { return stage_; }
    // Valid only when stage() == Capture and !*this.
    [[nodiscard]] const SessionSaveCaptureOutcome* captureFailure() const& noexcept {
        return stage_ == AsyncSessionSaveBeginStage::Capture ? &captureFailure_ : nullptr;
    }
    [[nodiscard]] const SessionSaveCaptureOutcome* captureFailure() const&& = delete;
    // Valid only when stage() == Submission and !*this.
    [[nodiscard]] std::optional<runtime::TaskSubmissionStatus> submissionFailure() const noexcept {
        return submissionFailure_;
    }
    // Moves the handle out; valid only when *this is true (terminates otherwise, mirroring
    // ProjectSessionCreateResult::takeSession()'s established pattern).
    [[nodiscard]] AsyncSessionSave takeHandle() && noexcept;

  private:
    friend AsyncSessionSaveResult beginSessionSave(ProjectSession&, runtime::TaskScheduler&,
                                                   PublicationCoordinator&,
                                                   platform::StagedArtifactCoordinator&,
                                                   const SessionSaveRequest&,
                                                   project::ProjectIoOperationMemory);

    explicit AsyncSessionSaveResult(SessionSaveCaptureOutcome outcome) noexcept;
    explicit AsyncSessionSaveResult(runtime::TaskSubmissionStatus status) noexcept;
    explicit AsyncSessionSaveResult(AsyncSessionSave handle) noexcept;

    AsyncSessionSaveBeginStage stage_ = AsyncSessionSaveBeginStage::Capture;
    SessionSaveCaptureOutcome captureFailure_;
    std::optional<runtime::TaskSubmissionStatus> submissionFailure_;
    std::optional<AsyncSessionSave> handle_;
};

// begin*(): runs session.captureSaveInput(request.intent) on the calling (authoring) thread; a
// refusal is a typed begin-failure, no task submitted. On success it builds a
// SessionSaveOwningInput (design decision 2) and submits the save middle
// (executeSessionSaveMiddle()) as a TaskExecutor::BlockingIo task, bridging cancellation as
// documented above. `session`, `coordinator`, and `artifacts` must outlive the whole asynchronous
// operation -- see the file-level threading-rules documentation. `request.preAdmitted`, when
// non-null, is CONSUMED here (moved into the handle's own owned storage) before this function
// returns; the caller has no lifetime obligation toward `*request.preAdmitted` afterward -- see the
// threading-rules documentation's `preAdmitted` note.
[[nodiscard]] AsyncSessionSaveResult
beginSessionSave(ProjectSession& session, runtime::TaskScheduler& scheduler,
                 PublicationCoordinator& coordinator,
                 platform::StagedArtifactCoordinator& artifacts, const SessionSaveRequest& request,
                 project::ProjectIoOperationMemory operation);

// ================================================================================================
// Open
// ================================================================================================

enum class AsyncSessionOpenBeginStage : std::uint8_t { Admission, Submission };

class AsyncSessionOpenResult;

class AsyncSessionOpen final {
  public:
    AsyncSessionOpen(AsyncSessionOpen&&) noexcept = default;
    AsyncSessionOpen& operator=(AsyncSessionOpen&&) noexcept = default;
    AsyncSessionOpen(const AsyncSessionOpen&) = delete;
    AsyncSessionOpen& operator=(const AsyncSessionOpen&) = delete;
    // Same typed-abandonment contract as ~AsyncSessionSave() above: requests cancellation, then
    // drops the handle without touching any session. Open never publishes anything remotely, so
    // there is no "forfeited savepoint" analogue here -- an abandoned Open simply never installs.
    ~AsyncSessionOpen();

    [[nodiscard]] bool isReady() const noexcept;
    void requestCancellation() noexcept;
    // Returns nullopt while running. Once the worker has reached a scheduler-terminal state,
    // returns the full result exactly once, applying installOpenedArchiveResult() (the O2 install
    // gates, which already refuse edit-during-open as RevisionChanged) against `session`.
    [[nodiscard]] std::optional<SessionOpenResult> tryComplete(ProjectSession& session);

  private:
    friend class AsyncSessionOpenResult;
    friend AsyncSessionOpenResult beginSessionOpen(ProjectSession&, runtime::TaskScheduler&,
                                                   std::vector<std::byte>,
                                                   std::optional<ProjectDisplayPath>,
                                                   const project::SaveArchiveLimits&,
                                                   project::ProjectIoOperationMemory);

    AsyncSessionOpen(runtime::TaskScheduler& scheduler, runtime::TaskHandle<void> handle,
                     std::shared_ptr<detail::AsyncSessionOpenSharedState> shared,
                     OpenIntentCapture intent,
                     std::optional<ProjectDisplayPath> displayPath) noexcept;

    runtime::TaskScheduler* scheduler_ = nullptr;
    runtime::TaskHandle<void> handle_;
    std::shared_ptr<detail::AsyncSessionOpenSharedState> shared_;
    OpenIntentCapture intent_;
    std::optional<ProjectDisplayPath> displayPath_;
    bool completed_ = false;
};

class [[nodiscard]] AsyncSessionOpenResult final {
  public:
    AsyncSessionOpenResult(AsyncSessionOpenResult&&) noexcept = default;
    AsyncSessionOpenResult& operator=(AsyncSessionOpenResult&&) noexcept = default;
    AsyncSessionOpenResult(const AsyncSessionOpenResult&) = delete;
    AsyncSessionOpenResult& operator=(const AsyncSessionOpenResult&) = delete;
    ~AsyncSessionOpenResult() = default;

    [[nodiscard]] explicit operator bool() const noexcept { return handle_.has_value(); }
    [[nodiscard]] AsyncSessionOpenBeginStage stage() const noexcept { return stage_; }
    // Valid only when stage() == Admission and !*this: session.admitOpenIntent()'s own refusal.
    [[nodiscard]] std::optional<OpenIntentAdmissionStatus> admissionFailure() const noexcept {
        return admissionFailure_;
    }
    // Valid only when stage() == Submission and !*this: the scheduler's own typed submission
    // refusal.
    [[nodiscard]] std::optional<runtime::TaskSubmissionStatus> submissionFailure() const noexcept {
        return submissionFailure_;
    }
    [[nodiscard]] AsyncSessionOpen takeHandle() && noexcept;

  private:
    friend AsyncSessionOpenResult beginSessionOpen(ProjectSession&, runtime::TaskScheduler&,
                                                   std::vector<std::byte>,
                                                   std::optional<ProjectDisplayPath>,
                                                   const project::SaveArchiveLimits&,
                                                   project::ProjectIoOperationMemory);

    explicit AsyncSessionOpenResult(OpenIntentAdmissionStatus status) noexcept;
    explicit AsyncSessionOpenResult(runtime::TaskSubmissionStatus status) noexcept;
    explicit AsyncSessionOpenResult(AsyncSessionOpen handle) noexcept;

    AsyncSessionOpenBeginStage stage_ = AsyncSessionOpenBeginStage::Admission;
    std::optional<OpenIntentAdmissionStatus> admissionFailure_;
    std::optional<runtime::TaskSubmissionStatus> submissionFailure_;
    std::optional<AsyncSessionOpen> handle_;
};

// begin*(): runs session.admitOpenIntent() on the calling (authoring) thread; a refusal is a typed
// begin-failure, no task submitted. The handle takes ownership of `bytes` (moved in, by value --
// the caller hands over the archive buffer; nothing else may mutate it afterward). On success,
// submits the open middle (project::openProjectArchive() over the owned bytes) as a
// TaskExecutor::BlockingIo task. `session` must outlive the whole asynchronous operation -- see the
// file-level threading-rules documentation.
[[nodiscard]] AsyncSessionOpenResult beginSessionOpen(ProjectSession& session,
                                                      runtime::TaskScheduler& scheduler,
                                                      std::vector<std::byte> bytes,
                                                      std::optional<ProjectDisplayPath> displayPath,
                                                      const project::SaveArchiveLimits& limits,
                                                      project::ProjectIoOperationMemory operation);

} // namespace bloom::host
