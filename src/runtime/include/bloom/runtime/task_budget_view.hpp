#pragma once

// Numeric admission defaults delivered by an explicit composition root. Tasks owns this neutral
// view: no renderer, ledger, settings, UI callback or admission credit crosses this boundary.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace bloom::runtime {
struct TaskBudgetSnapshot final {
    using Clock = std::chrono::steady_clock;
    std::uint64_t sampleSerial = 0;
    Clock::time_point sampledAt{};
    bool ready = false;
    std::size_t assignedPreviewBytes = 0;
    std::size_t assignedOperationBytes = 0;
    std::size_t previewRequestBytes = 0;
    std::size_t aggregateGpuBytes = 0;

    [[nodiscard]] bool fresh(const Clock::time_point now = Clock::now()) const noexcept {
        return ready && now >= sampledAt && now - sampledAt <= std::chrono::seconds(10);
    }
};

class TaskBudgetView final {
  public:
    [[nodiscard]] std::shared_ptr<const TaskBudgetSnapshot> load() const noexcept {
        const std::lock_guard lock(mutex_);
        return snapshot_;
    }
    // The composition root publishes only after its quota locks are released. Failed publication
    // removes default readiness rather than retaining a stale growth allowance. Actual phase
    // admission and explicit finite overrides remain independent of this default ceiling.
    [[nodiscard]] bool publish(TaskBudgetSnapshot snapshot) noexcept {
        try {
            auto next = std::make_shared<const TaskBudgetSnapshot>(snapshot);
            {
                const std::lock_guard lock(mutex_);
                if (snapshot_ && snapshot_->sampleSerial > next->sampleSerial)
                    return false;
                snapshot_.swap(next);
            }
            // The displaced snapshot is released after unlocking; allocation also precedes locking.
            return true;
        } catch (...) {
            std::shared_ptr<const TaskBudgetSnapshot> previous;
            {
                const std::lock_guard lock(mutex_);
                snapshot_.swap(previous);
            }
            return false;
        }
    }

  private:
    // A short shared_ptr copy/swap avoids depending on the C++20 atomic specialization,
    // which is unavailable in supported Apple libc++ versions. Immutable values stay coherent.
    mutable std::mutex mutex_;
    std::shared_ptr<const TaskBudgetSnapshot> snapshot_;
};
} // namespace bloom::runtime
