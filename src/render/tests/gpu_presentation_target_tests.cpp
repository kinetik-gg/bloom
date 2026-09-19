// Device-free presentation-target unit tests: the format-refusal table and the conservative
// wait-result classification used by the swapchain retirement poll. No device, surface, or driver
// is touched; the native Wayland lifecycle lives in the UI test.

#include <bloom/render/gpu_presentation_target.hpp>

#include "../vulkan/gpu_presentation_target_private.hpp"

#include <iostream>
#include <source_location>
#include <string>
#include <vector>

namespace {

using bloom::render::GpuPresentationTargetCode;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (!condition) {
            ++failures_;
            std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
        }
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] bool selectFormat(const std::vector<VkSurfaceFormatKHR>& formats, VkFormat& format,
                                VkColorSpaceKHR& colorSpace, std::string& error) {
    return bloom::render::presentation_detail::selectFormat(formats, format, colorSpace, error);
}

void testFormatTable(Expectations& expectations) {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    std::string error;

    const auto accepts = [&](const VkFormat wanted, const VkColorSpaceKHR space,
                             const VkFormat expected) {
        std::vector<VkSurfaceFormatKHR> formats{{wanted, space}};
        error.clear();
        expectations.expect(selectFormat(formats, format, colorSpace, error) &&
                                format == expected &&
                                colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
                            "accepted encoded UNORM format");
    };
    accepts(VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, VK_FORMAT_B8G8R8A8_UNORM);
    accepts(VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, VK_FORMAT_R8G8B8A8_UNORM);

    const auto rejects = [&](const VkFormat wanted, const VkColorSpaceKHR space) {
        std::vector<VkSurfaceFormatKHR> formats{{wanted, space}};
        error.clear();
        expectations.expect(!selectFormat(formats, format, colorSpace, error),
                            "refused an unsupported encoded format/color-space pairing");
    };
    rejects(VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    rejects(VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    rejects(VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT);
    rejects(VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT);
    rejects(VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);

    // The IMPLICIT-UNDEFINED single-format surface is the one defined default.
    std::vector<VkSurfaceFormatKHR> implicit{
        {VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    error.clear();
    expectations.expect(selectFormat(implicit, format, colorSpace, error) &&
                            format == VK_FORMAT_B8G8R8A8_UNORM &&
                            colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
                        "the IMPLICIT-UNDEFINED surface resolves to BGRA8 UNORM");

    // An empty list is refused, not substituted.
    std::vector<VkSurfaceFormatKHR> none;
    error.clear();
    expectations.expect(!selectFormat(none, format, colorSpace, error),
                        "an empty format list is refused");
}

void testWaitResultClassification(Expectations& expectations) {
    using bloom::render::presentation_detail::presentationCodeFromWaitResult;
    expectations.expect(presentationCodeFromWaitResult(VK_SUCCESS) == GpuPresentationTargetCode::Ok,
                        "VK_SUCCESS proves completion");
    expectations.expect(presentationCodeFromWaitResult(VK_ERROR_DEVICE_LOST) ==
                            GpuPresentationTargetCode::DeviceLost,
                        "VK_ERROR_DEVICE_LOST is terminal");
    expectations.expect(presentationCodeFromWaitResult(VK_TIMEOUT) ==
                            GpuPresentationTargetCode::RetirePending,
                        "VK_TIMEOUT is unproven, never retired");
    expectations.expect(presentationCodeFromWaitResult(VK_NOT_READY) ==
                            GpuPresentationTargetCode::RetirePending,
                        "VK_NOT_READY is unproven, never retired");
    expectations.expect(presentationCodeFromWaitResult(VK_ERROR_UNKNOWN) ==
                            GpuPresentationTargetCode::RetirePending,
                        "an unknown result is unproven, never retired");
}

} // namespace

int main() {
    Expectations expectations;
    testFormatTable(expectations);
    testWaitResultClassification(expectations);
    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " presentation target unit expectation(s) failed\n";
        return 1;
    }
    std::cout << "presentation target unit tests passed\n";
    return 0;
}
