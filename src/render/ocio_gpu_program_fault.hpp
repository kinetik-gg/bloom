#pragma once

// Test-only fault-injection seam for the OCIO GPU program resource lifecycle. Production code
// never sets an override. The functions have no Vulkan types so a test can include this header
// without the private Vulkan surface.

#include <cstdint>

namespace bloom::render::ocio_program_detail {

enum class UploadFenceOverride : std::uint8_t { None, NotReady, CancelAfterSubmit };

void setUploadFenceOverrideForTest(UploadFenceOverride override) noexcept;
[[nodiscard]] bool uploadQuarantineOccupiedForTest() noexcept;
void retireUploadQuarantineForTest() noexcept;

} // namespace bloom::render::ocio_program_detail
