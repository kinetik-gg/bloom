#pragma once

#include <bloom/host/copy_publication.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

// Asynchronous Save Copy over the blocking-I/O lane -- the sessionless sibling of
// bloom/host/session_async_io.hpp's AsyncSessionSave, over the SAME bloom::runtime::TaskScheduler
// TaskExecutor::BlockingIo lane, wrapping bloom::host::executeCopyPublication()
// (copy_publication.hpp) exactly as AsyncSessionSave wraps
// executeSessionSaveMiddle()/executeSavePublication().
//
// A deliberate sibling file rather than an addition to session_async_io.{hpp,cpp}: Save Copy has
// no ProjectSession dependency at all (docs/architecture/project-session.md: "Save Copy ... never
// [proposes] ProjectSession path/dirty changes"), so keeping it out of the session-titled module
// avoids implying a session tie that does not exist, and leaves session_async_io.{hpp,cpp} and its
// existing tests completely untouched (this task's scope guard: "defects in merged modules get
// documented, not fixed").
//
// ---------------------------------------------------------------------------------------------
// Threading rules
// ---------------------------------------------------------------------------------------------
// - beginCopyPublication(): authoring thread only. There is no session-level capture or admission
//   step to run synchronously here (unlike beginSessionSave()'s captureSaveInput() or
//   beginSessionOpen()'s admitOpenIntent()) -- Save Copy's only "capture" is the caller already
//   handing over the bytes to publish by value. Submission can still be refused by the scheduler
//   itself (a typed runtime::TaskSubmissionStatus), which is this wrapper's only begin-time failure
//   mode.
// - isReady() / tryComplete() / handle destruction: authoring thread only. tryComplete() takes NO
//   session and simply returns the typed CopyPublicationResult -- Save Copy never changes
//   clean/path/dirty state, so there is no acceptance tail to apply against anything (contrast
//   AsyncSessionSave::tryComplete(ProjectSession&), which applies acceptSessionSavePublication()).
// - requestCancellation(): may be called from ANY thread at any time -- identical contract to
//   AsyncSessionSave::requestCancellation(); see session_async_io.hpp's cancellation-bridge
//   documentation, which applies here unchanged (this wrapper bridges the identical
//   scheduler-CancellationToken-plus-owned-atomic pattern to executeCopyPublication()'s own
//   `cancellationFlag` seam).
// - `coordinator`, `artifacts` are caller-owned references captured by the worker closure; they
//   must outlive the whole asynchronous operation, exactly as session_async_io.hpp documents for
//   AsyncSessionSave's identical captures.
// - `request.preAdmitted`, when non-null, is CONSUMED here (moved into the handle's own owned
//   storage) before beginCopyPublication() returns -- the identical cross-thread ownership-transfer
//   lesson beginSessionSave() applies to SessionSaveRequest::preAdmitted (see
//   session_async_io.hpp's own "preAdmitted" threading-rules note for the full rationale: an async
//   worker consumes the pointee at an unbounded later time on another thread, so a borrowed pointer
//   would dangle against the caller's natural "admit, then let the admission go out of scope"
//   pattern).
namespace bloom::host {

namespace detail {

struct AsyncCopyPublicationSharedState final {
    std::atomic_bool cancellationFlag{false};
    // Populated exactly once by the worker before it returns; see
    // session_async_io.hpp's AsyncSessionSaveSharedState::publication for the identical
    // happens-before argument (the worker's write happens-before the scheduler ever reports the
    // task terminal).
    std::optional<CopyPublicationResult> publication;
    // The caller's AsyncCopyPublicationRequest::preAdmitted, MOVED here on the authoring thread by
    // beginCopyPublication() before task submission -- see the file-level "preAdmitted" threading
    // note above.
    std::optional<PublicationAdmission> preAdmitted;
};

} // namespace detail

// The async-level request: bytes-by-value (the handle takes ownership -- mirrors
// AsyncSessionOpen's own owned-bytes parameter), rather than CopyPublicationRequest's non-owning
// span (which is only ever safe for a same-frame synchronous call). `preAdmitted`, when non-null,
// follows the ownership-transfer contract documented at the top of this file.
struct AsyncCopyPublicationRequest final {
    std::filesystem::path targetPath;
    platform::ArtifactOverwritePolicy overwritePolicy =
        platform::ArtifactOverwritePolicy::CreateOrReplace;
    std::optional<platform::ArtifactTargetObservation> expectedTarget;
    std::vector<std::byte> bytes;
    project::SaveArchiveLimits limits;
    PublicationAdmission* preAdmitted = nullptr;
};

class AsyncCopyPublicationResult;

class AsyncCopyPublication final {
  public:
    AsyncCopyPublication(AsyncCopyPublication&&) noexcept = default;
    AsyncCopyPublication& operator=(AsyncCopyPublication&&) noexcept = default;
    AsyncCopyPublication(const AsyncCopyPublication&) = delete;
    AsyncCopyPublication& operator=(const AsyncCopyPublication&) = delete;
    // Typed abandonment, identical in spirit to ~AsyncSessionSave(): requests cancellation, then
    // drops the handle without touching anything. A copy whose executeCopyPublication() had
    // ALREADY entered its non-cancellable platform publication lease before this destructor runs
    // may still have durably published remotely -- that publication is real and stays real, but
    // the caller who abandons the handle here simply never observes the result (there is no
    // session bookkeeping to forfeit, since Save Copy never had any to begin with).
    ~AsyncCopyPublication();

    // Non-blocking, non-consuming poll; identical contract to AsyncSessionSave::isReady().
    [[nodiscard]] bool isReady() const noexcept;
    // May be called from any thread, at any time; identical contract to
    // AsyncSessionSave::requestCancellation().
    void requestCancellation() noexcept;
    // Returns nullopt while the worker has not yet reached a scheduler-terminal state. Once it has,
    // returns the full result exactly once (subsequent calls return nullopt). Takes NO session --
    // see the file-level threading-rules documentation above.
    [[nodiscard]] std::optional<CopyPublicationResult> tryComplete();

  private:
    friend class AsyncCopyPublicationResult;
    friend AsyncCopyPublicationResult beginCopyPublication(runtime::TaskScheduler&,
                                                           PublicationCoordinator&,
                                                           platform::StagedArtifactCoordinator&,
                                                           AsyncCopyPublicationRequest,
                                                           project::ProjectIoOperationMemory);

    AsyncCopyPublication(runtime::TaskScheduler& scheduler, runtime::TaskHandle<void> handle,
                         std::shared_ptr<detail::AsyncCopyPublicationSharedState> shared) noexcept;

    runtime::TaskScheduler* scheduler_ = nullptr;
    runtime::TaskHandle<void> handle_;
    std::shared_ptr<detail::AsyncCopyPublicationSharedState> shared_;
    bool completed_ = false;
};

// [[nodiscard]] begin-outcome: either a live handle (operator bool() true) or the scheduler's own
// typed submission refusal. Unlike AsyncSessionSaveResult/AsyncSessionOpenResult, there is no
// separate Capture/Admission begin-stage: Save Copy has no session-level step to run synchronously
// before submission (see the file-level threading-rules documentation above), so submission is the
// only way begin can fail.
class [[nodiscard]] AsyncCopyPublicationResult final {
  public:
    AsyncCopyPublicationResult(AsyncCopyPublicationResult&&) noexcept = default;
    AsyncCopyPublicationResult& operator=(AsyncCopyPublicationResult&&) noexcept = default;
    AsyncCopyPublicationResult(const AsyncCopyPublicationResult&) = delete;
    AsyncCopyPublicationResult& operator=(const AsyncCopyPublicationResult&) = delete;
    ~AsyncCopyPublicationResult() = default;

    [[nodiscard]] explicit operator bool() const noexcept { return handle_.has_value(); }
    // Valid only when !*this: the scheduler's own typed submission refusal.
    [[nodiscard]] std::optional<runtime::TaskSubmissionStatus> submissionFailure() const noexcept {
        return submissionFailure_;
    }
    // Moves the handle out; valid only when *this is true (terminates otherwise, mirroring
    // AsyncSessionSaveResult::takeHandle()'s established pattern).
    [[nodiscard]] AsyncCopyPublication takeHandle() && noexcept;

  private:
    friend AsyncCopyPublicationResult beginCopyPublication(runtime::TaskScheduler&,
                                                           PublicationCoordinator&,
                                                           platform::StagedArtifactCoordinator&,
                                                           AsyncCopyPublicationRequest,
                                                           project::ProjectIoOperationMemory);

    explicit AsyncCopyPublicationResult(runtime::TaskSubmissionStatus status) noexcept;
    explicit AsyncCopyPublicationResult(AsyncCopyPublication handle) noexcept;

    std::optional<runtime::TaskSubmissionStatus> submissionFailure_;
    std::optional<AsyncCopyPublication> handle_;
};

// Submits executeCopyPublication() over the owned `request.bytes` as a TaskExecutor::BlockingIo
// task, bridging cancellation exactly as beginSessionSave() does (see session_async_io.hpp's
// cancellation-bridge documentation, which applies unchanged here). `coordinator` and `artifacts`
// must outlive the whole asynchronous operation -- see the file-level threading-rules
// documentation. `request.preAdmitted`, when non-null, is CONSUMED here (moved into the handle's
// own owned storage) before this function returns.
[[nodiscard]] AsyncCopyPublicationResult
beginCopyPublication(runtime::TaskScheduler& scheduler, PublicationCoordinator& coordinator,
                     platform::StagedArtifactCoordinator& artifacts,
                     AsyncCopyPublicationRequest request,
                     project::ProjectIoOperationMemory operation);

} // namespace bloom::host
