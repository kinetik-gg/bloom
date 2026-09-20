#include "ocio_gpu_program_fault.hpp"

// Test-only setters. This translation unit is compiled into bloom_render_ocio_test_faults, which is
// linked only into the OCIO GPU program fault tests; production targets never link it.
namespace bloom::render::ocio_program_detail {

void setUploadFenceOverrideForTest(const UploadFenceOverride override) noexcept {
    ocioFaultNotReady().store(override == UploadFenceOverride::NotReady);
    ocioFaultCancelAfterSubmit().store(override == UploadFenceOverride::CancelAfterSubmit);
}

bool uploadQuarantineOccupiedForTest() noexcept { return uploadQuarantineOccupied(); }
void retireUploadQuarantineForTest() noexcept { retireUploadQuarantine(); }

} // namespace bloom::render::ocio_program_detail
