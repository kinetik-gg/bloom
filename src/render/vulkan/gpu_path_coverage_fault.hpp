#pragma once

// Private test-only fault-injection seam for GpuPathCoverage. The production-owned atomics are
// always defined in the Vulkan translation unit (and inert in the CPU stub), so a shipping build
// carries only the readable atomic and never a fault path unless a test sets it. The setters are
// declared here and defined in the same translation units; this header is never installed or
// included by a public render header.

#include <atomic>
#include <cstdint>

namespace bloom::render::path_coverage_detail {

enum class PathCoverageFault : std::uint8_t {
    None,
    // vkQueueSubmit is forced to fail before any submission is considered in flight.
    FailSubmit,
    // poll() treats the fence as unretired without waiting on the real fence, so the submission is
    // quarantined deterministically.
    ForceFenceTimeout,
    // poll() reports device loss, proving the exact submission is released without quarantine.
    ForceDeviceLost,
};

// Production-owned atomic read by the producer.
[[nodiscard]] std::atomic<std::uint8_t>& pathCoverageFault() noexcept;

// Test-only override of the effective compute workgroup count limits; 0 uses the real device value.
[[nodiscard]] std::atomic<std::uint32_t>& pathCoverageMaxWorkGroupCountXOverride() noexcept;
[[nodiscard]] std::atomic<std::uint32_t>& pathCoverageMaxWorkGroupCountYOverride() noexcept;

// Test/observability seams for the bounded reservation.
[[nodiscard]] bool pathCoverageQuarantineOccupied() noexcept;
[[nodiscard]] bool retirePathCoverageQuarantineForOwner() noexcept;

// Defined in the Vulkan translation unit and the CPU stub; tests call them.
void setPathCoverageFaultForTest(PathCoverageFault fault) noexcept;
void setPathCoverageMaxWorkGroupCountXForTest(std::uint32_t value) noexcept;
void setPathCoverageMaxWorkGroupCountYForTest(std::uint32_t value) noexcept;
[[nodiscard]] bool pathCoverageQuarantineOccupiedForTest() noexcept;
[[nodiscard]] bool retirePathCoverageQuarantineForTest() noexcept;

} // namespace bloom::render::path_coverage_detail
