#include "gpu_point_resample_fault.hpp"

// Test-only setters. This translation unit is compiled into
// bloom_render_point_resample_test_faults, which is linked only into the point-resample fault
// tests; production targets never link it.
namespace bloom::render::point_resample_detail {

void setPointResampleFaultForTest(const PointResampleFault fault) noexcept {
    pointResampleFault().store(static_cast<std::uint8_t>(fault));
}

void setPointResampleForcedMaxWorkGroupCountXForTest(const std::uint32_t maxGroupsX) noexcept {
    pointResampleForcedMaxWorkGroupCountX().store(maxGroupsX);
}

bool pointResampleQuarantineOccupiedForTest() noexcept { return pointResampleQuarantineOccupied(); }

bool retirePointResampleQuarantineForOwnerForTest() noexcept {
    return pointResampleRetireQuarantineForOwner();
}

std::uint32_t pointResampleLiveResourceSetsForTest() noexcept {
    return pointResampleLiveResourceSets();
}

std::uint32_t pointResampleLiveResidentsForTest() noexcept { return pointResampleLiveResidents(); }

std::uint32_t pointResampleRetainedResidentsForTest() noexcept {
    return pointResampleRetainedResidents();
}

std::uint32_t retirePointResampleRetainedForOwnerForTest() noexcept {
    return pointResampleRetireRetainedForOwner();
}

} // namespace bloom::render::point_resample_detail
