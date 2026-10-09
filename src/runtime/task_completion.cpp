#include <bloom/runtime/task_completion.hpp>

#include "task_completion_internal.hpp"

#include <limits>
#include <new>
#include <utility>

namespace bloom::runtime {
TaskCompletionSubscription::TaskCompletionSubscription(
    std::shared_ptr<detail::TaskCompletionState> state) noexcept
    : state_(std::move(state)) {}

TaskCompletionSubscription::TaskCompletionSubscription(TaskCompletionSubscription&&) noexcept =
    default;

TaskCompletionSubscription&
TaskCompletionSubscription::operator=(TaskCompletionSubscription&& other) noexcept {
    if (this != &other) {
        close();
        state_ = std::move(other.state_);
    }
    return *this;
}

TaskCompletionSubscription::~TaskCompletionSubscription() { close(); }

std::uint64_t TaskCompletionSubscription::serial() const noexcept {
    if (!state_)
        return 0;
    std::lock_guard lock(state_->mutex);
    return state_->serial;
}

TaskCompletionSubscriptionState TaskCompletionSubscription::state() const noexcept {
    if (!state_)
        return TaskCompletionSubscriptionState::Closed;
    std::lock_guard lock(state_->mutex);
    return state_->state;
}

std::optional<std::uint64_t>
TaskCompletionSubscription::waitForChange(const std::uint64_t observed,
                                          const std::stop_token& stop) const {
    if (!state_)
        return std::nullopt;
    std::unique_lock lock(state_->mutex);
    const bool changed = state_->changed.wait(lock, stop, [&] {
        return state_->state != TaskCompletionSubscriptionState::Open || state_->serial != observed;
    });
    if (!changed || stop.stop_requested() || state_->state != TaskCompletionSubscriptionState::Open)
        return std::nullopt;
    return state_->serial;
}

void TaskCompletionSubscription::close() noexcept {
    if (!state_)
        return;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->state == TaskCompletionSubscriptionState::Open)
            state_->state = TaskCompletionSubscriptionState::Closed;
    }
    state_->changed.notify_all();
}

namespace detail {
TaskCompletionSubscribeResult TaskCompletionSubscriptions::subscribe() noexcept {
    std::lock_guard lock(mutex_);
    if (state_ != TaskCompletionSubscriptionState::Open)
        return {.status = state_ == TaskCompletionSubscriptionState::SerialExhausted
                              ? TaskCompletionSubscribeStatus::SerialExhausted
                              : TaskCompletionSubscribeStatus::Closed,
                .subscription = {}};
    for (auto& slot : slots_) {
        const auto existing = slot.lock();
        if (existing) {
            std::lock_guard stateLock(existing->mutex);
            if (existing->state == TaskCompletionSubscriptionState::Open)
                continue;
        }
        try {
            auto state = std::make_shared<TaskCompletionState>();
            state->serial = serial_;
            slot = state;
            return {.status = TaskCompletionSubscribeStatus::Accepted,
                    .subscription = TaskCompletionSubscription(std::move(state))};
        } catch (const std::bad_alloc&) {
            return {.status = TaskCompletionSubscribeStatus::AllocationFailure, .subscription = {}};
        } catch (...) {
            return {.status = TaskCompletionSubscribeStatus::ResourceFailure, .subscription = {}};
        }
    }
    return {.status = TaskCompletionSubscribeStatus::CapacityReached, .subscription = {}};
}

void TaskCompletionSubscriptions::publish() noexcept {
    std::lock_guard lock(mutex_);
    if (state_ != TaskCompletionSubscriptionState::Open)
        return;
    // Fail closed rather than wrapping to an old observed serial and losing a wake.
    if (serial_ == std::numeric_limits<std::uint64_t>::max()) {
        closeLocked(TaskCompletionSubscriptionState::SerialExhausted);
        return;
    }
    ++serial_;
    for (auto& slot : slots_) {
        if (const auto state = slot.lock()) {
            {
                std::lock_guard stateLock(state->mutex);
                if (state->state != TaskCompletionSubscriptionState::Open)
                    continue;
                state->serial = serial_;
            }
            state->changed.notify_all();
        }
    }
}

void TaskCompletionSubscriptions::closeLocked(
    const TaskCompletionSubscriptionState state) noexcept {
    state_ = state;
    for (auto& slot : slots_) {
        if (const auto subscriber = slot.lock()) {
            {
                std::lock_guard stateLock(subscriber->mutex);
                if (subscriber->state == TaskCompletionSubscriptionState::Open)
                    subscriber->state = state;
            }
            subscriber->changed.notify_all();
        }
    }
}

void TaskCompletionSubscriptions::close() noexcept {
    std::lock_guard lock(mutex_);
    if (state_ == TaskCompletionSubscriptionState::Open)
        closeLocked(TaskCompletionSubscriptionState::Closed);
}
} // namespace detail
} // namespace bloom::runtime
