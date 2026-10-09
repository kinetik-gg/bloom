#pragma once

#include <bloom/render/packed_host_retirement.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>

namespace bloom::app {
// Declared before every consumer of the root. On an early return/exception those consumers
// are destroyed first; the scheduler and borrowed evaluators remain alive until this guard ends.
// This runs only during process teardown, after any active application event loop has returned.
class PackedHostProcessTeardown final {
  public:
    PackedHostProcessTeardown(bloom::runtime::TaskScheduler& scheduler,
                              std::shared_ptr<bloom::render::PackedHostRetirementOwner> owner)
        : scheduler_(scheduler), owner_(std::move(owner)) {}
    ~PackedHostProcessTeardown() {
        scheduler_.beginShutdown();
        owner_->beginShutdown();
        auto diagnosticAt = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!scheduler_.isQuiescent() || !owner_->exitAcknowledged()) {
            if (std::chrono::steady_clock::now() >= diagnosticAt) {
                const auto state = owner_->snapshot();
                std::cerr << "Bloom teardown is still awaiting packed host retirement: "
                          << state.allocations << " allocations, " << state.queuedAllocations
                          << " queued. Resources remain owned until actual exit.\n";
                diagnosticAt = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // No active UI event processing here. The OS join is not assumed universally nonblocking.
        while (!owner_->collect())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

  private:
    bloom::runtime::TaskScheduler& scheduler_;
    std::shared_ptr<bloom::render::PackedHostRetirementOwner> owner_;
};
} // namespace bloom::app
