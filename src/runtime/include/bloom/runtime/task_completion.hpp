#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>

namespace bloom::runtime {
namespace detail {
struct TaskCompletionState;
class TaskCompletionSubscriptions;
} // namespace detail

enum class TaskCompletionSubscriptionState : std::uint8_t { Open, Closed, SerialExhausted };

// A neutral wake only: subscribers still own and consume their typed task handles. The serial
// advances once after each winning terminal publishes its mailbox, history and final accounting.
// No callback, task payload or scheduler lifetime is retained. Waiting belongs on an adapter
// thread, never the UI thread. close() is concurrent/idempotent; move/destruction must not race
// calls on the subscription object. A stop token interrupts an otherwise idle wait.
class TaskCompletionSubscription final {
  public:
    TaskCompletionSubscription() = default;
    TaskCompletionSubscription(const TaskCompletionSubscription&) = delete;
    TaskCompletionSubscription& operator=(const TaskCompletionSubscription&) = delete;
    TaskCompletionSubscription(TaskCompletionSubscription&&) noexcept;
    TaskCompletionSubscription& operator=(TaskCompletionSubscription&&) noexcept;
    ~TaskCompletionSubscription();

    [[nodiscard]] std::uint64_t serial() const noexcept;
    [[nodiscard]] TaskCompletionSubscriptionState state() const noexcept;
    // nullopt means closed, serial exhausted, or stop requested. Read state() to distinguish it.
    [[nodiscard]] std::optional<std::uint64_t> waitForChange(std::uint64_t observed,
                                                             const std::stop_token& stop) const;
    void close() noexcept;

  private:
    friend class detail::TaskCompletionSubscriptions;
    explicit TaskCompletionSubscription(std::shared_ptr<detail::TaskCompletionState>) noexcept;
    std::shared_ptr<detail::TaskCompletionState> state_;
};

enum class TaskCompletionSubscribeStatus : std::uint8_t {
    Accepted,
    CapacityReached,
    AllocationFailure,
    ResourceFailure,
    Closed,
    SerialExhausted
};

struct TaskCompletionSubscribeResult final {
    TaskCompletionSubscribeStatus status = TaskCompletionSubscribeStatus::Closed;
    TaskCompletionSubscription subscription;
};

inline constexpr std::size_t kTaskCompletionSubscriptionCapacity = 16;

} // namespace bloom::runtime
