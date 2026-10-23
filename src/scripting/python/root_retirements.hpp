#pragma once

#include <bloom/host/gpu_export_provider.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>

namespace bloom::scripting::python {
class PythonRootRetirements;
struct PythonRootRetirementsTestAccess;

struct PythonRootRetirementSnapshot final {
    std::size_t roots = 0;
    std::size_t participants = 0;
    std::size_t activeCalls = 0;
    bool closing = false;
};

// One scheduler participant per Host. The registry owns the scheduler from inception; the Host
// and calls retain this lightweight record, never a Host/Session/Python object in the registry.
class PythonHostRetirement final : public std::enable_shared_from_this<PythonHostRetirement> {
  public:
    class Call final {
      public:
        Call(Call&&) noexcept;
        Call& operator=(Call&&) = delete;
        Call(const Call&) = delete;
        Call& operator=(const Call&) = delete;
        ~Call();
        [[nodiscard]] runtime::TaskScheduler& scheduler() const noexcept;
        [[nodiscard]] bool closing() const noexcept;

      private:
        Call(std::shared_ptr<PythonHostRetirement>, runtime::TaskScheduler*) noexcept;
        std::shared_ptr<PythonHostRetirement> participant_;
        runtime::TaskScheduler* scheduler_ = nullptr;
        friend class PythonHostRetirement;
    };
    [[nodiscard]] Call acquire();
    [[nodiscard]] std::optional<Call> tryAcquire() noexcept;
    [[nodiscard]] bool closing() const noexcept;
    // GC handoff: no allocation, wait or join. Must precede dropping Host's provider reference.
    void release(const std::shared_ptr<host::GpuExportProvider>&) noexcept;
    ~PythonHostRetirement();

  private:
    PythonHostRetirement();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void finishCall() noexcept;
    friend class PythonRootRetirements;
};

class PythonRootRetirements final : public std::enable_shared_from_this<PythonRootRetirements> {
  public:
    [[nodiscard]] static std::shared_ptr<PythonRootRetirements> create();
    ~PythonRootRetirements();
    // Inert owned-provider factory: one interpreter gate protects driver initialization across
    // provider generations; it never serializes pixel evaluation or closes a borrowed app root.
    [[nodiscard]] std::shared_ptr<host::GpuExportProvider>
    createOwnedProvider(runtime::GpuProcessFrameEvaluatorOptions options = {});
    // The one GPU export provider (and so the one native device) every Python Host of this
    // interpreter shares, created on first use. Its root lives for the interpreter: Hosts come and
    // go as members, and only beginShutdown() -- the module exit hook -- retires it, once and
    // serially, with no work in flight. Several devices torn down concurrently on different owner
    // threads deadlock inside at least one vendor driver. Throws std::runtime_error once shutdown
    // has begun.
    [[nodiscard]] std::shared_ptr<host::GpuExportProvider> sharedProvider();
    // Off GIL/nonUI: scheduler construction and every failure cleanup may join worker threads.
    [[nodiscard]] std::shared_ptr<PythonHostRetirement>
    registerHost(const std::shared_ptr<host::GpuExportProvider>&, bool ownsProvider);
    // Reject new calls/registrations and signal all owned roots and local schedulers. Borrowed
    // application providers/host owners are never closed or collected by this registry.
    void beginShutdown() noexcept;
    // Off GIL/nonUI, outside caller locks. Complete records are destroyed only after active calls,
    // scheduler work, native retirement and owned host actual exit are proven. Never abandons an
    // incomplete object on allocation failure or timeout.
    [[nodiscard]] bool collectCompleted() noexcept;
    [[nodiscard]] bool shutdownAndWait(std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] PythonRootRetirementSnapshot snapshot() const noexcept;

  private:
    PythonRootRetirements();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void release(const std::shared_ptr<PythonHostRetirement>&,
                 const std::shared_ptr<host::GpuExportProvider>&) noexcept;
    friend class PythonHostRetirement;
    friend struct PythonRootRetirementsTestAccess;
};
} // namespace bloom::scripting::python
