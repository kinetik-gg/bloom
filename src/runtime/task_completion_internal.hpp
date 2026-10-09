#pragma once

#include <bloom/runtime/task_completion.hpp>

#include <array>
#include <condition_variable>
#include <mutex>

namespace bloom::runtime::detail {
struct TaskCompletionState final {
    mutable std::mutex mutex;
    std::condition_variable_any changed;
    std::uint64_t serial = 0;
    TaskCompletionSubscriptionState state = TaskCompletionSubscriptionState::Open;
};

class TaskCompletionSubscriptions final {
  public:
    [[nodiscard]] TaskCompletionSubscribeResult subscribe() noexcept;
    void publish() noexcept;
    void close() noexcept;

  private:
    friend struct TaskSchedulerTestAccess;
    void closeLocked(TaskCompletionSubscriptionState state) noexcept;
    mutable std::mutex mutex_;
    std::array<std::weak_ptr<TaskCompletionState>, kTaskCompletionSubscriptionCapacity> slots_;
    std::uint64_t serial_ = 0;
    TaskCompletionSubscriptionState state_ = TaskCompletionSubscriptionState::Open;
};
} // namespace bloom::runtime::detail
