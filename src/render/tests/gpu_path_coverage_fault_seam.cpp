// Test-only fault seam definitions for GpuPathCoverage. This translation unit is compiled only into
// the test executable, never into bloom_render, so a shipping build exposes no fault setter. It
// forwards to the production-owned atomics and reservation accessors.

#include "gpu_path_coverage_fault.hpp"

#include <cstdint>

namespace bloom::render::path_coverage_detail {

void setPathCoverageFaultForTest(const PathCoverageFault fault) noexcept {
    pathCoverageFault().store(static_cast<std::uint8_t>(fault));
}

void setPathCoverageMaxWorkGroupCountXForTest(const std::uint32_t value) noexcept {
    pathCoverageMaxWorkGroupCountXOverride().store(value);
}

void setPathCoverageMaxWorkGroupCountYForTest(const std::uint32_t value) noexcept {
    pathCoverageMaxWorkGroupCountYOverride().store(value);
}

bool pathCoverageQuarantineOccupiedForTest() noexcept { return pathCoverageQuarantineOccupied(); }

bool retirePathCoverageQuarantineForTest() noexcept {
    return retirePathCoverageQuarantineForOwner();
}

} // namespace bloom::render::path_coverage_detail
