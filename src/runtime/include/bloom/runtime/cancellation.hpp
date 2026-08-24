#pragma once

#include <atomic>
#include <memory>

namespace bloom::runtime {

namespace detail {

struct CancellationState {
    std::atomic_bool requested = false;
    std::atomic_bool completed = false;
};

} // namespace detail

class CancellationToken final {
  public:
    CancellationToken() = default;

    [[nodiscard]] bool isCancellationRequested() const noexcept;

  private:
    friend class TaskContext;
    CancellationToken(std::shared_ptr<const detail::CancellationState> taskState,
                      std::shared_ptr<const detail::CancellationState> groupState) noexcept;

    std::shared_ptr<const detail::CancellationState> taskState_;
    std::shared_ptr<const detail::CancellationState> groupState_;
};

} // namespace bloom::runtime
