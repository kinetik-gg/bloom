#pragma once

// Test-only fault-injection seam for the combined output-colour readback. The production atomic and
// the production quarantine accessors are always defined; the setters are defined only in the
// test-only static library bloom_render_output_color_readback_test_faults, which production targets
// never link, so a shipping build cannot expose fault behavior.

#include <atomic>
#include <cstdint>

namespace bloom::render::output_color_readback_detail {

enum class ReadbackFault : std::uint8_t {
    None,
    // The staging VMA allocation is forced to fail before submit.
    FailStagingAllocation,
    // vkQueueSubmit is forced to fail.
    FailSubmit,
    // poll() observes the deadline as exceeded without waiting on the real fence.
    ForceFenceTimeout,
};

// Production-owned atomic read by the readback implementation.
[[nodiscard]] std::atomic<std::uint8_t>& readbackFault() noexcept;

// Production quarantine accessors (defined in the Vulkan translation unit, so this header needs no
// Vulkan include).
[[nodiscard]] bool outputColorQuarantineOccupied() noexcept;
[[nodiscard]] bool outputColorRetireQuarantineForOwner() noexcept;

// Defined only in the test-only library.
void setReadbackFaultForTest(ReadbackFault fault) noexcept;
[[nodiscard]] bool outputColorQuarantineOccupiedForTest() noexcept;
[[nodiscard]] bool retireOutputColorQuarantineForTest() noexcept;

} // namespace bloom::render::output_color_readback_detail
