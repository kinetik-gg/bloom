#pragma once

// Shared by the import progress tests (#377): a gate that holds the import worker at one progress
// report until the test releases it, and a log of every report the worker made.

#include <bloom/commands/asset_operations.hpp>

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <vector>

namespace bloom::ui::test {

using ImportReportPredicate = std::function<bool(const commands::ImportProgress&)>;

// The report that starts reading item `index`.
inline ImportReportPredicate itemStart(const std::size_t index) {
    return [index](const commands::ImportProgress& state) {
        return state.phase == commands::ImportProgress::Phase::Reading &&
               state.completed == index && !state.current.empty() && state.framesRead == 0;
    };
}

class ImportGate final {
  public:
    explicit ImportGate(ImportReportPredicate at) : at_(std::move(at)) {}
    ~ImportGate() { release(); }
    // Runs on the import worker for every report.
    void operator()(const commands::ImportProgress& state) {
        {
            const std::lock_guard lock(mutex_);
            log_.push_back(state);
        }
        if (!at_(state))
            return;
        std::unique_lock lock(mutex_);
        reached_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });
    }
    [[nodiscard]] bool reached() {
        const std::lock_guard lock(mutex_);
        return reached_;
    }
    void release() {
        const std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }
    [[nodiscard]] std::vector<commands::ImportProgress> log() {
        const std::lock_guard lock(mutex_);
        return log_;
    }

  private:
    ImportReportPredicate at_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<commands::ImportProgress> log_;
    bool reached_ = false;
    bool released_ = false;
};

} // namespace bloom::ui::test
