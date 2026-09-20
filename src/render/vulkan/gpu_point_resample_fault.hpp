#pragma once

// Test-only fault-injection seam for PointResampleV1. The production atomics and quarantine
// accessors are always defined; the setters are defined only in the test-only static library
// bloom_render_point_resample_test_faults, which production targets never link, so a shipping build
// cannot expose fault behavior.

#include <atomic>
#include <cstdint>

namespace bloom::render::point_resample_detail {

enum class PointResampleFault : std::uint8_t {
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

// Production-owned fault selector read by the point-resample implementation.
[[nodiscard]] std::atomic<std::uint8_t>& pointResampleFault() noexcept;

// Production-owned forced X workgroup-count limit (0 = use the physical limit). Clamped to the
// physical value by the implementation, so a test can force a 2D dispatch tail on real hardware.
[[nodiscard]] std::atomic<std::uint32_t>& pointResampleForcedMaxWorkGroupCountX() noexcept;

// Production quarantine accessors (defined in the Vulkan translation unit).
[[nodiscard]] bool pointResampleQuarantineOccupied() noexcept;
[[nodiscard]] bool pointResampleRetireQuarantineForOwner() noexcept;
// Observability: the number of live native per-job resource sets across all instances. Bounded by
// the single process-wide reservation.
[[nodiscard]] std::uint32_t pointResampleLiveResourceSets() noexcept;

// Defined only in the test-only library.
void setPointResampleFaultForTest(PointResampleFault fault) noexcept;
void setPointResampleForcedMaxWorkGroupCountXForTest(std::uint32_t maxGroupsX) noexcept;
[[nodiscard]] bool pointResampleQuarantineOccupiedForTest() noexcept;
[[nodiscard]] bool retirePointResampleQuarantineForOwnerForTest() noexcept;
[[nodiscard]] std::uint32_t pointResampleLiveResourceSetsForTest() noexcept;

} // namespace bloom::render::point_resample_detail
