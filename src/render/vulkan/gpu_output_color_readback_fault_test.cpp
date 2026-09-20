#include "gpu_output_color_readback_fault.hpp"

// Test-only setters. This translation unit is compiled into
// bloom_render_output_color_readback_test_faults, which is linked only into the combined
// output-colour readback fault tests; production targets never link it.
namespace bloom::render::output_color_readback_detail {

void setReadbackFaultForTest(const ReadbackFault fault) noexcept {
    readbackFault().store(static_cast<std::uint8_t>(fault));
}

bool outputColorQuarantineOccupiedForTest() noexcept { return outputColorQuarantineOccupied(); }

bool retireOutputColorQuarantineForTest() noexcept { return outputColorRetireQuarantineForOwner(); }

} // namespace bloom::render::output_color_readback_detail
