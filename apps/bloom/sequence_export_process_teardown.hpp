#pragma once

#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <chrono>
#include <memory>
#include <thread>
#include <utility>

namespace bloom::app {
// Declared before UI export clients. On normal shutdown the coordinator has already proved
// quiescence; on an early return or exception this post-event-loop guard preserves the owner and
// borrowed task dependencies until both admitted tasks and export-specific retirement finish.
class SequenceExportProcessTeardown final {
  public:
    SequenceExportProcessTeardown(runtime::TaskScheduler& scheduler,
                                  std::shared_ptr<host::SequenceExportRetirementOwnerV1> retirement)
        : scheduler_(scheduler), retirement_(std::move(retirement)) {}
    ~SequenceExportProcessTeardown() {
        scheduler_.beginShutdown();
        retirement_->beginShutdown();
        while (!scheduler_.isQuiescent() || !retirement_->retirementComplete())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        while (!retirement_->collect())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

  private:
    runtime::TaskScheduler& scheduler_;
    std::shared_ptr<host::SequenceExportRetirementOwnerV1> retirement_;
};
} // namespace bloom::app
