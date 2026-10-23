#pragma once
#include "root_retirements.hpp"

#include <bloom/runtime/packed_host_retirement.hpp>

#include <atomic>
#include <list>
#include <mutex>

namespace bloom::scripting::python {
struct PythonProviderRetirement final {
    std::weak_ptr<host::GpuExportProvider> provider;
    std::shared_ptr<host::GpuExportProvider> closingProvider;
    std::shared_ptr<runtime::PackedHostRetirementOwner> hostOwner;
    std::size_t memberships = 0;
    bool ownsProvider = false;
    // The interpreter's shared provider: zero members does not close it; shutdown does.
    bool processLifetime = false;
    bool closing = false;
    bool collected = false;
};
struct PythonHostRetirement::Impl final {
    mutable std::mutex mutex;
    std::weak_ptr<PythonRootRetirements> registry;
    std::shared_ptr<PythonProviderRetirement> root;
    std::unique_ptr<runtime::TaskScheduler> scheduler;
    std::atomic_bool closing = false;
    std::size_t activeCalls = 0;
    bool starting = true;
    bool released = false;
    bool collecting = false;
    bool collected = false;
};
struct PythonRootRetirements::Impl final {
    mutable std::mutex mutex;
    std::list<std::shared_ptr<PythonHostRetirement>> participants;
    std::list<std::shared_ptr<PythonProviderRetirement>> roots;
    bool closing = false;
    bool collecting = false;
    std::shared_ptr<std::timed_mutex> bootstrapGate = std::make_shared<std::timed_mutex>();
    // The strong reference that keeps the shared provider alive between Hosts; released at
    // shutdown, after which its root's closingProvider carries it through retirement.
    std::shared_ptr<host::GpuExportProvider> sharedProvider;
    // Private construction-failure seam, empty for all production module/interpreter roots.
    void (*beforeRegistration)(void*) = nullptr;
    void* testContext = nullptr;
};
} // namespace bloom::scripting::python
