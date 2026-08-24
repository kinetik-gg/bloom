#include <bloom/project/project_io_memory.hpp>

#include <algorithm>
#include <exception>
#include <mutex>
#include <new>
#include <utility>

namespace bloom::project::detail {

struct ProjectIoSharedMemoryState final {
    explicit ProjectIoSharedMemoryState(const std::uint64_t value) noexcept : limitBytes(value) {}

    std::mutex mutex;
    std::uint64_t currentBytes = 0;
    std::uint64_t peakBytes = 0;
    std::uint64_t limitBytes = 0;
};

struct ProjectIoOperationMemoryState final {
    ProjectIoOperationMemoryState(std::shared_ptr<ProjectIoSharedMemoryState> sharedState,
                                  const std::uint64_t value,
                                  const std::uint64_t singleValue) noexcept
        : shared(std::move(sharedState)), limitBytes(value),
          singleAllocationLimitBytes(singleValue) {}

    std::shared_ptr<ProjectIoSharedMemoryState> shared;
    std::mutex mutex;
    std::uint64_t currentBytes = 0;
    std::uint64_t peakBytes = 0;
    std::uint64_t limitBytes = 0;
    std::uint64_t singleAllocationLimitBytes = 0;
};

} // namespace bloom::project::detail

namespace {

void releaseReservation(
    const std::shared_ptr<bloom::project::detail::ProjectIoOperationMemoryState>& operation,
    const std::uint64_t bytes) noexcept {
    if (operation == nullptr || bytes == 0) {
        return;
    }
    const auto& shared = operation->shared;
    std::scoped_lock lock(operation->mutex, shared->mutex);
    if (bytes > operation->currentBytes || bytes > shared->currentBytes) {
        std::terminate();
    }
    operation->currentBytes -= bytes;
    shared->currentBytes -= bytes;
}

} // namespace

namespace bloom::project {

std::optional<ProjectIoMemoryCoordinator>
ProjectIoMemoryCoordinator::create(const std::uint64_t limitBytes) noexcept {
    if (limitBytes == 0) {
        return std::nullopt;
    }
    try {
        return ProjectIoMemoryCoordinator(
            std::make_shared<detail::ProjectIoSharedMemoryState>(limitBytes));
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
}

std::optional<ProjectIoOperationMemory> ProjectIoMemoryCoordinator::createOperation(
    const std::uint64_t limitBytes, const std::uint64_t singleAllocationLimitBytes) const noexcept {
    if (state_ == nullptr || limitBytes == 0 || limitBytes > state_->limitBytes ||
        singleAllocationLimitBytes == 0 || singleAllocationLimitBytes > limitBytes) {
        return std::nullopt;
    }
    try {
        return ProjectIoOperationMemory(std::make_shared<detail::ProjectIoOperationMemoryState>(
            state_, limitBytes, singleAllocationLimitBytes));
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
}

ProjectIoMemorySnapshot ProjectIoMemoryCoordinator::snapshot() const {
    std::lock_guard lock(state_->mutex);
    return {.currentBytes = state_->currentBytes,
            .peakBytes = state_->peakBytes,
            .limitBytes = state_->limitBytes};
}

ProjectIoMemoryCoordinator::ProjectIoMemoryCoordinator(
    std::shared_ptr<detail::ProjectIoSharedMemoryState> state) noexcept
    : state_(std::move(state)) {}

ProjectIoMemoryReservation::ProjectIoMemoryReservation(
    std::shared_ptr<detail::ProjectIoOperationMemoryState> operation,
    const std::uint64_t bytes) noexcept
    : operation_(std::move(operation)), bytes_(bytes) {}

ProjectIoMemoryReservation::ProjectIoMemoryReservation(ProjectIoMemoryReservation&& other) noexcept
    : operation_(std::move(other.operation_)), bytes_(std::exchange(other.bytes_, 0)) {}

ProjectIoMemoryReservation&
ProjectIoMemoryReservation::operator=(ProjectIoMemoryReservation&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    operation_ = std::move(other.operation_);
    bytes_ = std::exchange(other.bytes_, 0);
    return *this;
}

ProjectIoMemoryReservation::~ProjectIoMemoryReservation() { reset(); }

bool ProjectIoMemoryReservation::isValid() const noexcept { return operation_ != nullptr; }

void ProjectIoMemoryReservation::reset() noexcept {
    releaseReservation(operation_, bytes_);
    operation_.reset();
    bytes_ = 0;
}

ProjectIoMemoryReservationResult::ProjectIoMemoryReservationResult(
    const ProjectIoMemoryError error) noexcept
    : error_(error) {}

ProjectIoMemoryReservationResult::ProjectIoMemoryReservationResult(
    ProjectIoMemoryReservation reservation) noexcept
    : error_(ProjectIoMemoryError::None), reservation_(std::move(reservation)) {}

ProjectIoMemoryReservation ProjectIoMemoryReservationResult::takeReservation() && noexcept {
    return std::move(reservation_);
}

ProjectIoMemoryReservationResult
ProjectIoOperationMemory::reserve(const std::uint64_t bytes) const {
    if (state_ == nullptr) {
        return ProjectIoMemoryReservationResult(ProjectIoMemoryError::InvalidLimit);
    }
    if (bytes > state_->singleAllocationLimitBytes) {
        return ProjectIoMemoryReservationResult(
            ProjectIoMemoryError::SingleAllocationLimitExceeded);
    }
    const auto& shared = state_->shared;
    std::scoped_lock lock(state_->mutex, shared->mutex);
    if (bytes > state_->limitBytes - state_->currentBytes) {
        return ProjectIoMemoryReservationResult(ProjectIoMemoryError::OperationLimitExceeded);
    }
    if (bytes > shared->limitBytes - shared->currentBytes) {
        return ProjectIoMemoryReservationResult(ProjectIoMemoryError::SharedLimitExceeded);
    }

    state_->currentBytes += bytes;
    state_->peakBytes = std::max(state_->peakBytes, state_->currentBytes);
    shared->currentBytes += bytes;
    shared->peakBytes = std::max(shared->peakBytes, shared->currentBytes);
    return ProjectIoMemoryReservationResult(ProjectIoMemoryReservation(state_, bytes));
}

ProjectIoOperationMemorySnapshot ProjectIoOperationMemory::snapshot() const {
    std::lock_guard lock(state_->mutex);
    return {.currentBytes = state_->currentBytes,
            .peakBytes = state_->peakBytes,
            .limitBytes = state_->limitBytes,
            .singleAllocationLimitBytes = state_->singleAllocationLimitBytes};
}

ProjectIoOperationMemory::ProjectIoOperationMemory(
    std::shared_ptr<detail::ProjectIoOperationMemoryState> state) noexcept
    : state_(std::move(state)) {}

} // namespace bloom::project
