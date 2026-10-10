#pragma once

#include <bloom/render/packed_host_retirement.hpp>

#include <chrono>
#include <stdexcept>
#include <thread>

namespace bloom::render::test {
// Fixture thread only. Keep this before roots and all storage aliases so the real worker can
// finish after those aliases die, including failure unwinding; never substitute enqueue for free.
struct FinalHostRetirementScope final {
    std::shared_ptr<PackedHostRetirementOwner> owner = PackedHostRetirementOwner::create();
    FinalHostRetirementScope() {
        if (!owner->prepare())
            throw std::runtime_error("final host fixture retirement worker must prepare");
    }
    ~FinalHostRetirementScope() {
        owner->beginShutdown();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!owner->exitAcknowledged() && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        if (!owner->exitAcknowledged() || !owner->collect())
            std::terminate();
    }
};
} // namespace bloom::render::test
