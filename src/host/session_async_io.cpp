#include <bloom/host/session_async_io.hpp>

#include <exception>
#include <new>
#include <span>
#include <utility>

namespace bloom::host {

namespace {

// Neither ProjectSession nor bloom::runtime::TaskScheduler exposes a per-session TaskOwnerId today
// (docs/architecture/project-session.md: "The runtime Project task owner uses a fresh TaskOwnerId
// for each installed session" -- that identity does not yet exist on ProjectSession's public
// surface). This task's non-goals forbid jobs/task-bridge changes, so both async workers below
// submit under this fixed placeholder Application-kind owner rather than inventing new
// session-owned identity plumbing. A future slice that wires ProjectSession to a real per-session
// TaskOwnerId should replace this with that identity so scheduler-level owner cancellation
// (TaskScheduler::cancelOwner()) can target one session's async I/O specifically.
[[nodiscard]] runtime::TaskOwner asyncSessionIoOwner() noexcept {
    return {.kind = runtime::TaskOwnerKind::Application, .id = runtime::TaskOwnerId::fromRaw(1)};
}

} // namespace

// ================================================================================================
// Save
// ================================================================================================

AsyncSessionSave::AsyncSessionSave(runtime::TaskScheduler& scheduler,
                                   runtime::TaskHandle<void> handle,
                                   std::shared_ptr<detail::AsyncSessionSaveSharedState> shared,
                                   const SessionPathIntentCapture intent,
                                   const document::Revision revision,
                                   std::filesystem::path targetPath) noexcept
    : scheduler_(&scheduler), handle_(std::move(handle)), shared_(std::move(shared)),
      intent_(intent), revision_(revision), targetPath_(std::move(targetPath)) {}

AsyncSessionSave::~AsyncSessionSave() {
    if (shared_ && !completed_) {
        requestCancellation();
    }
}

bool AsyncSessionSave::isReady() const noexcept {
    if (!shared_ || completed_ || scheduler_ == nullptr) {
        return false;
    }
    const auto snapshot = scheduler_->snapshot(handle_.id());
    return snapshot.has_value() && runtime::isTerminal(snapshot->state);
}

void AsyncSessionSave::requestCancellation() noexcept {
    if (shared_) {
        shared_->cancellationFlag.store(true, std::memory_order_release);
    }
    if (handle_.isValid()) {
        handle_.cancel();
    }
}

std::optional<SessionSaveResult> AsyncSessionSave::tryComplete(ProjectSession& session) {
    if (!shared_ || completed_) {
        return std::nullopt;
    }
    const auto taken = handle_.tryTakeResult();
    if (!taken.has_value()) {
        return std::nullopt;
    }
    completed_ = true;
    if (!shared_->publication.has_value()) {
        // The scheduler-level task never populated its shared result (an exception the runtime's
        // own TypedTaskWork wrapper caught before our worker body ran to completion, or a
        // scheduler-level Cancelled state for a task that never started) -- no middle ever ran, so
        // the session is untouched. Reported the same typed way the synchronous flow reports an
        // unexpected pipeline failure before publish() ever runs.
        return SessionSaveResult::publicationFailure(
            SavePublicationFailure(SavePublicationStage::None, SavePublicationUnexpectedFailure{}),
            std::nullopt);
    }
    return acceptSessionSavePublication(session, std::move(*shared_->publication), intent_,
                                        revision_, targetPath_);
}

AsyncSessionSaveResult::AsyncSessionSaveResult(SessionSaveCaptureOutcome outcome) noexcept
    : stage_(AsyncSessionSaveBeginStage::Capture), captureFailure_(outcome) {}

AsyncSessionSaveResult::AsyncSessionSaveResult(const runtime::TaskSubmissionStatus status) noexcept
    : stage_(AsyncSessionSaveBeginStage::Submission), submissionFailure_(status) {}

AsyncSessionSaveResult::AsyncSessionSaveResult(AsyncSessionSave handle) noexcept
    : stage_(AsyncSessionSaveBeginStage::Submission), handle_(std::move(handle)) {}

AsyncSessionSave AsyncSessionSaveResult::takeHandle() && noexcept {
    if (!handle_.has_value()) {
        std::terminate();
    }
    return std::move(*handle_);
}

AsyncSessionSaveResult beginSessionSave(ProjectSession& session, runtime::TaskScheduler& scheduler,
                                        PublicationCoordinator& coordinator,
                                        platform::StagedArtifactCoordinator& artifacts,
                                        const SessionSaveRequest& request,
                                        project::ProjectIoOperationMemory operation) {
    std::optional<SessionSaveOwningInput> owning;
    try {
        SessionSaveInputResult captured = session.captureSaveInput(request.intent);
        if (!captured) {
            return AsyncSessionSaveResult(SessionSaveCaptureOutcome{captured.status()});
        }
        owning.emplace(SessionSaveOwningInput{
            .capturedInput = std::move(captured).takeValue(),
            .targetPath = request.targetPath,
            .overwritePolicy = request.overwritePolicy,
            .expectedTarget = request.expectedTarget,
            .limits = request.limits,
        });
    } catch (const std::bad_alloc&) {
        return AsyncSessionSaveResult(SessionSaveCaptureOutcome{SessionSaveResourceExhausted{}});
    } catch (...) {
        return AsyncSessionSaveResult(SessionSaveCaptureOutcome{SessionSaveUnexpectedFailure{}});
    }

    const auto intent = owning->capturedInput.pathIntent();
    const auto revision = owning->capturedInput.revision();
    auto targetPath = owning->targetPath;
    auto shared = std::make_shared<detail::AsyncSessionSaveSharedState>();

    // Ownership transfer, not a borrow (see session_async_io.hpp's threading-rules "preAdmitted"
    // note): the synchronous flow's `preAdmitted` pointer is same-frame-safe
    // (executeSavePublication() runs and returns before saveProjectSession() does), but the async
    // worker consumes it at an unbounded later time on another thread -- a borrowed pointer would
    // dangle against the caller's natural "admit, then let the admission go out of scope" pattern.
    // Move the admission out of the caller's object HERE, on the authoring thread, into `shared`
    // (see AsyncSessionSaveSharedState::preAdmitted's own comment for why it lives there rather
    // than in the worker closure's captures directly). PublicationAdmission is move-only RAII that
    // abandons itself on destruction, which is exactly correct if the task is never started
    // (submission rejected) or never reaches registration.
    if (request.preAdmitted != nullptr) {
        shared->preAdmitted.emplace(std::move(*request.preAdmitted));
    }

    runtime::TaskRequest taskRequest("Save project (async)", asyncSessionIoOwner(),
                                     runtime::TaskPriority::Foreground,
                                     runtime::TaskExecutor::BlockingIo);

    auto submission = scheduler.submit<void>(
        std::move(taskRequest),
        [&coordinator, &artifacts, ownedInput = std::move(*owning),
         operation = std::move(operation),
         shared](runtime::TaskContext& context) -> runtime::TaskResult<void> {
            // Entry-time cancellation bridge -- see session_async_io.hpp's cancellation-bridge
            // documentation for exactly what this covers.
            if (context.isCancellationRequested()) {
                shared->cancellationFlag.store(true, std::memory_order_release);
            }
            // `shared` is captured by value (a copy of the shared_ptr), but its pointee is not
            // const-qualified by that -- mutating *shared through it (here, and via
            // shared->publication.emplace() below) needs no `mutable` on this lambda.
            auto* const preAdmitted =
                shared->preAdmitted.has_value() ? &*shared->preAdmitted : nullptr;
            auto publication =
                executeSessionSaveMiddle(coordinator, artifacts, ownedInput, preAdmitted,
                                         &shared->cancellationFlag, operation);
            shared->publication.emplace(std::move(publication));
            return runtime::TaskResult<void>::succeeded();
        });

    if (!submission.accepted()) {
        return AsyncSessionSaveResult(submission.status);
    }

    return AsyncSessionSaveResult(AsyncSessionSave(scheduler, std::move(submission.handle),
                                                   std::move(shared), intent, revision,
                                                   std::move(targetPath)));
}

// ================================================================================================
// Open
// ================================================================================================

AsyncSessionOpen::AsyncSessionOpen(runtime::TaskScheduler& scheduler,
                                   runtime::TaskHandle<void> handle,
                                   std::shared_ptr<detail::AsyncSessionOpenSharedState> shared,
                                   const OpenIntentCapture intent,
                                   std::optional<ProjectDisplayPath> displayPath) noexcept
    : scheduler_(&scheduler), handle_(std::move(handle)), shared_(std::move(shared)),
      intent_(intent), displayPath_(std::move(displayPath)) {}

AsyncSessionOpen::~AsyncSessionOpen() {
    if (shared_ && !completed_) {
        requestCancellation();
    }
}

bool AsyncSessionOpen::isReady() const noexcept {
    if (!shared_ || completed_ || scheduler_ == nullptr) {
        return false;
    }
    const auto snapshot = scheduler_->snapshot(handle_.id());
    return snapshot.has_value() && runtime::isTerminal(snapshot->state);
}

void AsyncSessionOpen::requestCancellation() noexcept {
    if (shared_) {
        shared_->cancellationFlag.store(true, std::memory_order_release);
    }
    if (handle_.isValid()) {
        handle_.cancel();
    }
}

std::optional<SessionOpenResult> AsyncSessionOpen::tryComplete(ProjectSession& session) {
    if (!shared_ || completed_) {
        return std::nullopt;
    }
    const auto taken = handle_.tryTakeResult();
    if (!taken.has_value()) {
        return std::nullopt;
    }
    completed_ = true;
    if (shared_->cancelledBeforeOpening) {
        return SessionOpenResult::notOpened(SessionOpenNotOpenedReason::CancelledBeforeOpening);
    }
    if (!shared_->opened.has_value()) {
        // Same scheduler-level "never populated" edge as AsyncSessionSave::tryComplete() -- no
        // install*() call is ever made, session untouched.
        return SessionOpenResult::notOpened(SessionOpenNotOpenedReason::WorkerUnexpectedFailure);
    }
    return installOpenedArchiveResult(session, intent_, std::move(*shared_->opened),
                                      std::move(displayPath_));
}

AsyncSessionOpenResult::AsyncSessionOpenResult(const OpenIntentAdmissionStatus status) noexcept
    : stage_(AsyncSessionOpenBeginStage::Admission), admissionFailure_(status) {}

AsyncSessionOpenResult::AsyncSessionOpenResult(const runtime::TaskSubmissionStatus status) noexcept
    : stage_(AsyncSessionOpenBeginStage::Submission), submissionFailure_(status) {}

AsyncSessionOpenResult::AsyncSessionOpenResult(AsyncSessionOpen handle) noexcept
    : stage_(AsyncSessionOpenBeginStage::Submission), handle_(std::move(handle)) {}

AsyncSessionOpen AsyncSessionOpenResult::takeHandle() && noexcept {
    if (!handle_.has_value()) {
        std::terminate();
    }
    return std::move(*handle_);
}

AsyncSessionOpenResult beginSessionOpen(ProjectSession& session, runtime::TaskScheduler& scheduler,
                                        std::vector<std::byte> bytes,
                                        std::optional<ProjectDisplayPath> displayPath,
                                        const project::SaveArchiveLimits& limits,
                                        project::ProjectIoOperationMemory operation) {
    const auto admission = session.admitOpenIntent();
    if (!admission) {
        return AsyncSessionOpenResult(admission.status());
    }
    const auto intent = admission.capture();
    const auto limitsCopy = limits;
    auto shared = std::make_shared<detail::AsyncSessionOpenSharedState>();

    runtime::TaskRequest taskRequest("Open project (async)", asyncSessionIoOwner(),
                                     runtime::TaskPriority::Foreground,
                                     runtime::TaskExecutor::BlockingIo);

    auto submission = scheduler.submit<void>(
        std::move(taskRequest),
        [bytesOwned = std::move(bytes), limitsCopy, operation = std::move(operation),
         shared](runtime::TaskContext& context) -> runtime::TaskResult<void> {
            // Entry-time-only cancellation bridge -- project::openProjectArchive() has no internal
            // checkpoints of its own to observe a mid-flight cancellation; see
            // session_async_io.hpp's cancellation-bridge documentation.
            if (context.isCancellationRequested() ||
                shared->cancellationFlag.load(std::memory_order_acquire)) {
                shared->cancellationFlag.store(true, std::memory_order_release);
                shared->cancelledBeforeOpening = true;
                return runtime::TaskResult<void>::succeeded();
            }
            auto opened = project::openProjectArchive(std::span<const std::byte>(bytesOwned),
                                                      limitsCopy, operation);
            shared->opened.emplace(std::move(opened));
            return runtime::TaskResult<void>::succeeded();
        });

    if (!submission.accepted()) {
        return AsyncSessionOpenResult(submission.status);
    }

    return AsyncSessionOpenResult(AsyncSessionOpen(scheduler, std::move(submission.handle),
                                                   std::move(shared), intent,
                                                   std::move(displayPath)));
}

} // namespace bloom::host
