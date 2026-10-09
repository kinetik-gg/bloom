#pragma once

// Numeric admission defaults delivered by an explicit composition root. Tasks owns this neutral
// view: no renderer, ledger, settings, UI callback or admission credit crosses this boundary.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
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
        return snapshot_.load(std::memory_order_acquire);
    }
    // The composition root publishes only after its quota locks are released. Failed publication
    // removes default readiness rather than retaining a stale growth allowance. Actual phase
    // admission and explicit finite overrides remain independent of this default ceiling.
    [[nodiscard]] bool publish(TaskBudgetSnapshot snapshot) noexcept {
        try {
            auto next = std::make_shared<const TaskBudgetSnapshot>(snapshot);
            auto previous = load();
            do {
                if (previous && previous->sampleSerial > next->sampleSerial)
                    return false;
            } while (!snapshot_.compare_exchange_weak(previous, next, std::memory_order_acq_rel,
                                                      std::memory_order_acquire));
            return true;
        } catch (...) {
            snapshot_.store(nullptr, std::memory_order_release);
            return false;
        }
    }

  private:
    std::atomic<std::shared_ptr<const TaskBudgetSnapshot>> snapshot_;
};
} // namespace bloom::runtime
