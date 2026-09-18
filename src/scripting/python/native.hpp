#pragma once

#include "runtime.hpp"
#include "task_ticket.hpp"

#include <bloom/scripting/facade.hpp>
#include <nanobind/nanobind.h>

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace bloom::scripting::python {

namespace nb = nanobind;

// Owned by the client runtime. Only immutable DTOs cross the Python boundary.
class NativeHost final {
  public:
    explicit NativeHost(const std::string& project = {});
    explicit NativeHost(std::shared_ptr<Session> session);
    ~NativeHost() = default;
    NativeHost(const NativeHost&) = delete;
    NativeHost& operator=(const NativeHost&) = delete;

    [[nodiscard]] nb::dict snapshot() const;
    [[nodiscard]] nb::list schemas() const;
    [[nodiscard]] nb::dict transact(const nb::list& operations, const std::string& label,
                                    std::uint64_t revision);
    [[nodiscard]] nb::dict history(bool redo);
    void save(const std::string& path);
    [[nodiscard]] nb::dict render(std::uint64_t composition, std::uint64_t first,
                                  std::uint64_t last, bool range, const std::string& preset,
                                  const std::string& destination, std::uint64_t cancellationTask);
    [[nodiscard]] nb::list events();
    [[nodiscard]] nb::list tasks() const;
    [[nodiscard]] std::unique_ptr<TaskTicket> trackTask(std::string name) {
        return std::make_unique<TaskTicket>(scheduler_, std::move(name));
    }
    [[nodiscard]] bool cancel(std::uint64_t id);
    [[nodiscard]] nb::dict context() const;

    std::function<PythonContext()> contextProvider;
    std::function<bool()> cancelProvider;
    bool headless = true;
    void prepareExecution() noexcept { cancelled_.store(false); }
    void requestCancellation();
    [[nodiscard]] std::uint64_t cancellationEpoch() const noexcept {
        return cancellationEpoch_.load();
    }
    [[nodiscard]] bool cancellationRequested() const noexcept {
        return cancelled_.load() || (cancelProvider && cancelProvider());
    }
    [[nodiscard]] std::vector<runtime::TaskSnapshot> taskSnapshots() const {
        return scheduler_.snapshots();
    }

  private:
    [[nodiscard]] std::unique_lock<std::mutex> lockSession() const;
    mutable std::mutex sessionMutex_;
    std::shared_ptr<Session> session_;
    std::unique_ptr<Facade> facade_;
    std::thread::id owner_ = std::this_thread::get_id();
    runtime::TaskScheduler scheduler_;
    mutable std::mutex eventMutex_;
    std::deque<commands::CommandEvent> pendingEvents_;
    bool eventsDropped_ = false;
    Subscription subscription_;
    std::atomic_bool cancelled_ = false;
    std::atomic_uint64_t cancellationEpoch_ = 0;
};

[[nodiscard]] nb::dict projectSnapshot(const document::Snapshot& snapshot);
[[nodiscard]] nb::dict commandResult(const commands::CommandResult& result);
[[nodiscard]] Value scriptValue(nb::handle value, std::size_t depth = 0);
void bindHost(nb::module_& module);

} // namespace bloom::scripting::python
