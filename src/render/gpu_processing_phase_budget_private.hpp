#pragma once

#include <bloom/render/gpu_processing_phase_budget.hpp>

#include <limits>
#include <mutex>

namespace bloom::render::detail {
struct GpuProcessingPhaseBudgetState final {
    mutable std::mutex mutex;
    GpuProcessingPhaseSnapshot counters;
    std::uint64_t nextId = 1;
    std::shared_ptr<PackedHostRetirementState> retirement;
    std::chrono::steady_clock::time_point hostValidUntil =
        std::chrono::steady_clock::time_point::max();
};
[[nodiscard]] inline std::uint64_t remaining(const std::uint64_t cap,
                                             const std::uint64_t reserved) noexcept {
    return cap > reserved ? cap - reserved : 0;
}
inline void incrementSaturating(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}
} // namespace bloom::render::detail
