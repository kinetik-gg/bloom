#pragma once

#include "root_retirements.hpp"
#include "runtime.hpp"
#include "task_ticket.hpp"

#include <bloom/host/gpu_export_provider.hpp>
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
    NativeHost(const std::string& project, std::shared_ptr<PythonRootRetirements>);
    explicit NativeHost(std::shared_ptr<Session> session,
                        std::shared_ptr<host::GpuExportProvider> provider,
                        std::shared_ptr<PythonRootRetirements>);
    ~NativeHost();
    NativeHost(const NativeHost&) = delete;
    NativeHost& operator=(const NativeHost&) = delete;

    [[nodiscard]] nb::dict snapshot() const;
    [[nodiscard]] nb::list schemas() const;
    [[nodiscard]] nb::dict transact(const nb::list& operations, const std::string& label,
                                    std::uint64_t revision);
    [[nodiscard]] nb::dict history(bool redo);
    void save(const std::string& path);
    // Project Settings > Colour (Session::setColorSettings): not undoable, marks the project dirty.
    [[nodiscard]] nanobind::dict setColorSettings(const std::string& config,
                                                  std::optional<std::string> workingSpace);
    [[nodiscard]] nb::dict render(std::uint64_t composition, std::uint64_t first,
                                  std::uint64_t last, bool range, const std::string& preset,
                                  const std::string& destination, std::uint64_t cancellationTask);
    [[nodiscard]] nb::list events();
    [[nodiscard]] nb::list tasks() const;
    [[nodiscard]] std::unique_ptr<TaskTicket> trackTask(std::string name) {
        auto call = retirement_->acquire();
        return std::make_unique<TaskTicket>(call.scheduler(), std::move(name));
    }
    [[nodiscard]] bool cancel(std::uint64_t id);
    [[nodiscard]] nb::dict context() const;
    // The same values, in the language-neutral shape contextual arguments are filled from.
    [[nodiscard]] OperationContext operationContext() const;

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
        auto call = retirement_->tryAcquire();
        return call ? call->scheduler().snapshots() : std::vector<runtime::TaskSnapshot>{};
    }

  private:
    [[nodiscard]] std::unique_lock<std::mutex> lockSession() const;
    mutable std::mutex sessionMutex_;
    std::shared_ptr<Session> session_;
    std::unique_ptr<Facade> facade_;
    std::thread::id owner_ = std::this_thread::get_id();
    std::shared_ptr<PythonRootRetirements> rootRetirements_;
    std::shared_ptr<PythonHostRetirement> retirement_;
    std::shared_ptr<host::GpuExportProvider> gpuExportProvider_;
    bool ownsGpuExportProvider_ = false;
    mutable std::mutex eventMutex_;
    std::deque<commands::CommandEvent> pendingEvents_;
    bool eventsDropped_ = false;
    Subscription subscription_;
    std::atomic_bool cancelled_ = false;
    std::atomic_uint64_t cancellationEpoch_ = 0;
    friend struct PythonRootRetirementsTestAccess;
};

[[nodiscard]] nb::dict projectSnapshot(const document::Snapshot& snapshot);
[[nodiscard]] nb::dict commandResult(const commands::CommandResult& result);
[[nodiscard]] Value scriptValue(nb::handle value, std::size_t depth = 0);
void bindHost(nb::module_& module);
[[nodiscard]] std::shared_ptr<PythonRootRetirements> moduleRootRetirements();
// GIL held: close admission, finish Python producers and release runtime-owned context refs.
void beginPythonRootShutdown(const std::shared_ptr<PythonRootRetirements>&);
void preparePythonRootShutdown(const std::shared_ptr<PythonRootRetirements>&) noexcept;
// GIL held on entry; actual waits/joins release it. Never returns while retirement is incomplete.
void finishPythonRootShutdown(const std::shared_ptr<PythonRootRetirements>&) noexcept;

} // namespace bloom::scripting::python
