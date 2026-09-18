#pragma once

#include <bloom/scripting/tasks.hpp>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

namespace bloom::scripting::python {

// The scheduler observes native progress/cancellation only. Python runs on its dedicated worker,
// never in a TaskScheduler callable, completion callback or render kernel.
class TaskTicket final {
  public:
    explicit TaskTicket(runtime::TaskScheduler& scheduler, std::string name);
    ~TaskTicket();
    TaskTicket(const TaskTicket&) = delete;
    TaskTicket& operator=(const TaskTicket&) = delete;
    [[nodiscard]] std::uint64_t id() const noexcept { return handle_.id().value(); }
    [[nodiscard]] bool cancelled() const noexcept {
        return state_->cancelled.load() || handle_.cancellationRequested();
    }
    void cancel() const noexcept {
        state_->cancelled.store(true);
        handle_.cancel();
    }
    void progress(std::uint64_t completed, std::optional<std::uint64_t> total, std::string phase);
    void finish(std::string diagnostic);

  private:
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        runtime::TaskProgress progress;
        bool finished = false;
        std::atomic_bool cancelled = false;
        std::string diagnostic;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
    runtime::TaskHandle<void> handle_;
};

} // namespace bloom::scripting::python
