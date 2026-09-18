#pragma once

#include <bloom/runtime/task_scheduler.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bloom::scripting {

class Tasks final {
  public:
    explicit Tasks(runtime::TaskScheduler& scheduler) noexcept : scheduler_(scheduler) {}

    [[nodiscard]] runtime::TaskSubmission<void>
    submit(std::string name, runtime::TaskFunction<void> function,
           runtime::TaskExecutor executor = runtime::TaskExecutor::Cpu) {
        return scheduler_.submit<void>(
            runtime::TaskRequest(std::move(name),
                                 {.kind = runtime::TaskOwnerKind::Application,
                                  .id = runtime::TaskOwnerId::fromRaw(1)},
                                 runtime::TaskPriority::Foreground, executor),
            std::move(function));
    }
    [[nodiscard]] bool cancel(runtime::TaskId id) noexcept { return scheduler_.cancel(id); }
    [[nodiscard]] std::vector<runtime::TaskSnapshot> snapshots() const {
        return scheduler_.snapshots();
    }

  private:
    runtime::TaskScheduler& scheduler_;
};

} // namespace bloom::scripting
