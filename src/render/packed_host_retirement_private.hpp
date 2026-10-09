#pragma once

#include <bloom/render/packed_host_retirement.hpp>

#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace bloom::render {
namespace detail {
enum class PackedHostWorkerState { Dormant, Starting, Running, Failed };
struct PackedHostRetirementState final {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::weak_ptr<PackedHostRetirementOwner> owner;
    std::promise<void> exitPromise;
    std::shared_future<void> exit = exitPromise.get_future().share();
    PackedHostRetirementNode* first = nullptr;
    PackedHostRetirementNode* last = nullptr;
    std::size_t allocations = 0;
    std::size_t queued = 0;
    PackedHostWorkerState worker = PackedHostWorkerState::Dormant;
    bool closing = false;
    bool collected = false;
    bool collecting = false;
    bool ackRegistered = false;
    bool ackRegistrationFailed = false;
    // Private test seams remain empty for ordinary roots and perform no allocation when invoked.
    void (*beforeStart)(void*) = nullptr;
    void (*beforeFree)(void*) noexcept = nullptr;
    void (*beforeAckRegistration)(void*) noexcept = nullptr;
    void (*beforeCollect)(void*) noexcept = nullptr;
    void* testContext = nullptr;
};
} // namespace detail
struct PackedHostRetirementOwner::Impl final {
    std::shared_ptr<detail::PackedHostRetirementState> state =
        std::make_shared<detail::PackedHostRetirementState>();
    std::thread worker;
};
} // namespace bloom::render
