#include <bloom/host/copy_async_io.hpp>

#include <exception>
#include <new>
#include <span>
#include <utility>

namespace bloom::host {

namespace {

// A distinct placeholder TaskOwner from session_async_io.cpp's asyncSessionIoOwner() (id 1):
// Save Copy has no ProjectSession tie at all, so it deliberately does not share an owner identity
// with session-scoped Save/Open async work -- a future scheduler-level cancelOwner() targeting one
// session's async I/O should not also reach an unrelated in-flight Save Copy, and vice versa. Both
// remain fixed Application-kind placeholders for the same reason session_async_io.cpp's own comment
// documents: neither ProjectSession nor TaskScheduler exposes a per-session TaskOwnerId today, and
// this task's non-goals forbid jobs/task-bridge changes.
[[nodiscard]] runtime::TaskOwner asyncCopyPublicationOwner() noexcept {
    return {.kind = runtime::TaskOwnerKind::Application, .id = runtime::TaskOwnerId::fromRaw(2)};
}

} // namespace

AsyncCopyPublication::AsyncCopyPublication(
    runtime::TaskScheduler& scheduler, runtime::TaskHandle<void> handle,
    std::shared_ptr<detail::AsyncCopyPublicationSharedState> shared) noexcept
    : scheduler_(&scheduler), handle_(std::move(handle)), shared_(std::move(shared)) {}

AsyncCopyPublication::~AsyncCopyPublication() {
    if (shared_ && !completed_) {
        requestCancellation();
    }
}

bool AsyncCopyPublication::isReady() const noexcept {
    if (!shared_ || completed_ || scheduler_ == nullptr) {
        return false;
    }
    const auto snapshot = scheduler_->snapshot(handle_.id());
    return snapshot.has_value() && runtime::isTerminal(snapshot->state);
}

void AsyncCopyPublication::requestCancellation() noexcept {
    if (shared_) {
        shared_->cancellationFlag.store(true, std::memory_order_release);
    }
    if (handle_.isValid()) {
        handle_.cancel();
    }
}

std::optional<CopyPublicationResult> AsyncCopyPublication::tryComplete() {
    if (!shared_ || completed_) {
        return std::nullopt;
    }
    const auto taken = handle_.tryTakeResult();
    if (!taken.has_value()) {
        return std::nullopt;
    }
    completed_ = true;
    if (!shared_->publication.has_value()) {
        // Same scheduler-level "never populated" edge session_async_io.cpp's
        // AsyncSessionSave::tryComplete() documents: no CopyPublicationResult was ever produced, so
        // this is reported the same typed way an unexpected pipeline failure before publish() would
        // be.
        return CopyPublicationResult::failure(
            CopyPublicationFailure(CopyPublicationStage::None, CopyPublicationUnexpectedFailure{}));
    }
    return std::move(shared_->publication);
}

AsyncCopyPublicationResult::AsyncCopyPublicationResult(
    const runtime::TaskSubmissionStatus status) noexcept
    : submissionFailure_(status) {}

AsyncCopyPublicationResult::AsyncCopyPublicationResult(AsyncCopyPublication handle) noexcept
    : handle_(std::move(handle)) {}

AsyncCopyPublication AsyncCopyPublicationResult::takeHandle() && noexcept {
    if (!handle_.has_value()) {
        std::terminate();
    }
    return std::move(*handle_);
}

AsyncCopyPublicationResult beginCopyPublication(runtime::TaskScheduler& scheduler,
                                                PublicationCoordinator& coordinator,
                                                platform::StagedArtifactCoordinator& artifacts,
                                                AsyncCopyPublicationRequest request,
                                                project::ProjectIoOperationMemory operation) {
    auto shared = std::make_shared<detail::AsyncCopyPublicationSharedState>();

    // Ownership transfer, not a borrow -- see this file's own "preAdmitted" threading-rules note
    // for why (identical to beginSessionSave()'s treatment of SessionSaveRequest::preAdmitted).
    if (request.preAdmitted != nullptr) {
        shared->preAdmitted.emplace(std::move(*request.preAdmitted));
    }

    runtime::TaskRequest taskRequest("Save a copy (async)", asyncCopyPublicationOwner(),
                                     runtime::TaskPriority::Foreground,
                                     runtime::TaskExecutor::BlockingIo);

    auto submission = scheduler.submit<void>(
        std::move(taskRequest),
        [&coordinator, &artifacts, bytesOwned = std::move(request.bytes),
         targetPath = request.targetPath, overwritePolicy = request.overwritePolicy,
         expectedTarget = request.expectedTarget, limits = request.limits,
         operation = std::move(operation),
         shared](runtime::TaskContext& context) -> runtime::TaskResult<void> {
            // Entry-time cancellation bridge -- see this file's own cancellation-bridge
            // documentation (identical to session_async_io.hpp's) for exactly what this covers.
            if (context.isCancellationRequested()) {
                shared->cancellationFlag.store(true, std::memory_order_release);
            }
            auto* const preAdmitted =
                shared->preAdmitted.has_value() ? &*shared->preAdmitted : nullptr;
            const CopyPublicationRequest executorRequest{
                .targetPath = targetPath,
                .overwritePolicy = overwritePolicy,
                .expectedTarget = expectedTarget,
                .sourceBytes = std::span<const std::byte>(bytesOwned),
                .limits = limits,
                .preAdmitted = preAdmitted,
                .cancellationFlag = &shared->cancellationFlag,
            };
            auto publication =
                executeCopyPublication(coordinator, artifacts, executorRequest, operation);
            shared->publication.emplace(std::move(publication));
            return runtime::TaskResult<void>::succeeded();
        });

    if (!submission.accepted()) {
        return AsyncCopyPublicationResult(submission.status);
    }

    return AsyncCopyPublicationResult(
        AsyncCopyPublication(scheduler, std::move(submission.handle), std::move(shared)));
}

} // namespace bloom::host
