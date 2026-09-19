#include <bloom/render/gpu_present_image.hpp>
#include <bloom/render/gpu_presentation_target.hpp>

#include <memory>
#include <string>

// Portable CPU-unavailable stub for the present-image module. It defines the free entry point the
// native GpuPresentationTarget::presentImage member calls, so a build without Vulkan dependencies
// links with the exact same API and every resident present reports a typed PresentationUnavailable
// while the CPU display path stays active. It includes no Vulkan header: the private types are only
// forward-declared, because the stub never touches a device. The public GpuPresentationTarget stub
// reports the same code directly, so this definition is only linked, never called.

namespace bloom::render {

namespace vulkan_detail {
struct DeviceAllocatorState;
} // namespace vulkan_detail

namespace presentation_detail {
struct SwapchainResources;
} // namespace presentation_detail

namespace present_image_detail {

struct PresentImagePipeline;

GpuPresentationTargetCode renderResidentIntoAcquired(
    presentation_detail::SwapchainResources&,
    const std::shared_ptr<vulkan_detail::DeviceAllocatorState>&,
    std::shared_ptr<PresentImagePipeline>&, const std::shared_ptr<const GpuDisplayImage>&,
    const GpuPresentImageParams&, const GpuPresentOverlay&, std::string& message) {
    message = "Bloom was built without Vulkan dependencies; the CPU display path remains active";
    return GpuPresentationTargetCode::PresentationUnavailable;
}

} // namespace present_image_detail
} // namespace bloom::render
