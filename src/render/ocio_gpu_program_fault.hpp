#pragma once

// Test-only fault-injection seam for the OCIO GPU program resource lifecycle. The setters are
// defined in a separate test-only static library (bloom_render_ocio_test_faults) that production
// targets never link, so a shipping build cannot expose fault behavior. The accessors below are
// plain (no Vulkan types) and default to "no fault".

#include <atomic>
#include <cstdint>

namespace bloom::render::ocio_program_detail {

enum class UploadFenceOverride : std::uint8_t { None, NotReady, CancelAfterSubmit };

[[nodiscard]] std::atomic<bool>& ocioFaultNotReady() noexcept;
[[nodiscard]] std::atomic<bool>& ocioFaultCancelAfterSubmit() noexcept;
[[nodiscard]] std::atomic<bool>& ocioFaultJobFenceUnknown() noexcept;
[[nodiscard]] std::atomic<bool>& ocioFaultLutRgbaFormat() noexcept;
[[nodiscard]] std::atomic<std::uint32_t>& ocioFaultPublicationAfter() noexcept;

// Production quarantine helpers the test wrappers forward to.
[[nodiscard]] bool uploadQuarantineOccupied() noexcept;
void retireUploadQuarantine() noexcept;

// Defined only in the test-only library.
void setUploadFenceOverrideForTest(UploadFenceOverride override) noexcept;
// After real submit, observe unknown completion until cleared; never fabricates retirement/loss.
void setJobFenceUnknownForTest(bool unknown) noexcept;
// Use the supported RGBA alternative for actual LUT upload and pixel-oracle tests.
void setLutRgbaFormatForTest(bool forced) noexcept;
// UINT32_MAX disables the controlled rejection; otherwise reject after this many promotions.
void setPublicationFailureAfterForTest(std::uint32_t promoted) noexcept;
[[nodiscard]] bool uploadQuarantineOccupiedForTest() noexcept;
void retireUploadQuarantineForTest() noexcept;

} // namespace bloom::render::ocio_program_detail
