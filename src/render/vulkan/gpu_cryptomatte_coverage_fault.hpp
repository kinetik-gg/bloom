#pragma once

// Test-only fault-injection seam for CryptomatteCoverageV1. The production atomics and accessors
// are always defined; the setters are defined only in the test-only static library
// bloom_render_cryptomatte_coverage_test_faults, which production targets never link, so a shipping
// build cannot expose fault behavior.

#include <atomic>
#include <cstdint>

namespace bloom::render::cryptomatte_coverage_detail {

enum class CryptomatteCoverageFault : std::uint8_t {
    None,
    // The per-job native allocation (pipeline/buffer/image) is forced to fail before submit.
    FailJobAllocation,
    // vkQueueSubmit is forced to fail.
    FailSubmit,
    // poll() observes the deadline as exceeded without waiting on the real fence.
    ForceFenceTimeout,
    // poll() observes VK_ERROR_DEVICE_LOST.
    ForceDeviceLost,
};

// Production-owned fault selector read by the cryptomatte-coverage implementation.
[[nodiscard]] std::atomic<std::uint8_t>& cryptomatteCoverageFault() noexcept;

// Production-owned forced X workgroup-count limit (0 = use the physical limit). Clamped to the
// physical value by the implementation, so a test can force a 2D dispatch tail on real hardware.
[[nodiscard]] std::atomic<std::uint32_t>& cryptomatteCoverageForcedMaxWorkGroupCountX() noexcept;

// Production accessors (defined in the Vulkan translation unit).
[[nodiscard]] bool cryptomatteCoverageQuarantineOccupied() noexcept;
[[nodiscard]] bool cryptomatteCoverageRetireQuarantineForOwner() noexcept;
// Observability: live native per-job resource sets (pipeline/command/fence) across all instances,
// bounded by the single process-wide in-flight reservation.
[[nodiscard]] std::uint32_t cryptomatteCoverageLiveResourceSets() noexcept;
// Observability: live published/retained resident output images process-wide, bounded by the
// resident pool. Includes Ready results and foreign-destruction-retained results.
[[nodiscard]] std::uint32_t cryptomatteCoverageLiveResidents() noexcept;
// Observability: residents retained for owner-thread destruction after a foreign-thread
// destruction.
[[nodiscard]] std::uint32_t cryptomatteCoverageRetainedResidents() noexcept;
// Owner-only recovery: destroys every retained resident owned by the calling thread. Returns the
// number reclaimed.
[[nodiscard]] std::uint32_t cryptomatteCoverageRetireRetainedForOwner() noexcept;

// Defined only in the test-only library.
void setCryptomatteCoverageFaultForTest(CryptomatteCoverageFault fault) noexcept;
void setCryptomatteCoverageForcedMaxWorkGroupCountXForTest(std::uint32_t maxGroupsX) noexcept;
[[nodiscard]] bool cryptomatteCoverageQuarantineOccupiedForTest() noexcept;
[[nodiscard]] bool retireCryptomatteCoverageQuarantineForOwnerForTest() noexcept;
[[nodiscard]] std::uint32_t cryptomatteCoverageLiveResourceSetsForTest() noexcept;
[[nodiscard]] std::uint32_t cryptomatteCoverageLiveResidentsForTest() noexcept;
[[nodiscard]] std::uint32_t cryptomatteCoverageRetainedResidentsForTest() noexcept;
[[nodiscard]] std::uint32_t retireCryptomatteCoverageRetainedForOwnerForTest() noexcept;

} // namespace bloom::render::cryptomatte_coverage_detail
