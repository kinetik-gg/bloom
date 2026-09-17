#include "task_ticket.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace bloom::scripting::python {

TaskTicket::TaskTicket(runtime::TaskScheduler& scheduler, std::string name) {
    Tasks tasks(scheduler);
    auto submission = tasks.submit(
        std::move(name),
        [state = state_](runtime::TaskContext& context) {
            std::unique_lock lock(state->mutex);
            while (!state->finished) {
                context.reportProgress(state->progress);
                if (context.isCancellationRequested()) {
                    state->cancelled.store(true);
                    return runtime::TaskResult<void>::cancelled();
                }
                state->changed.wait_for(lock, std::chrono::milliseconds(10));
            }
            context.reportProgress(state->progress);
            if (!state->diagnostic.empty()) {
                return runtime::TaskResult<void>::failed(
                    {.code = "bloom.scripting.python-task-failed",
                     .severity = runtime::DiagnosticSeverity::Error,
                     .summary = "Python task failed",
                     .detail = state->diagnostic,
                     .suggestedAction = "Inspect the Script output."});
            }
            return runtime::TaskResult<void>::succeeded();
        },
        runtime::TaskExecutor::BlockingIo);
    if (!submission.accepted()) {
        throw std::runtime_error("The host task queue is full or shutting down");
    }
    handle_ = std::move(submission.handle);
}

TaskTicket::~TaskTicket() {
    const std::lock_guard lock(state_->mutex);
    if (!state_->finished) {
        cancel();
    }
}

void TaskTicket::progress(const std::uint64_t completed, const std::optional<std::uint64_t> total,
                          std::string phase) {
    if (total && completed > *total) {
        throw std::invalid_argument("Task progress exceeds its total");
    }
    const std::lock_guard lock(state_->mutex);
    state_->progress = {
        .phase = std::move(phase), .subphase = {}, .completed = completed, .total = total};
    state_->changed.notify_all();
}

void TaskTicket::finish(std::string diagnostic) {
    const std::lock_guard lock(state_->mutex);
    state_->diagnostic = std::move(diagnostic);
    state_->finished = true;
    state_->changed.notify_all();
}

} // namespace bloom::scripting::python
