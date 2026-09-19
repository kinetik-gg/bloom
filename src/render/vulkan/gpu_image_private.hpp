#ifndef BLOOM_RENDER_VULKAN_GPU_IMAGE_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_IMAGE_PRIVATE_HPP

// Private to src/render/vulkan. Defines the opaque impl behind
// bloom::render::GpuImage and the resident-image creation/retirement helpers
// the SolidV1 operation (and later transform/sourceover inputs) reuse.
// Vulkan-Hpp typed RAII and VMA stay here; the public header exposes no native
// type.

#include <bloom/render/gpu_image.hpp>

#include "gpu_device_private.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace bloom::render {

// Resident RGBA32F image state. Co-owns the device allocator generation so the
// allocator cannot be destroyed while the image lives. Destruction is
// owner-thread-only.
struct GpuImageImpl final {
    GpuImageImpl() = default;
    GpuImageImpl(const GpuImageImpl&) = delete;
    GpuImageImpl& operator=(const GpuImageImpl&) = delete;
    ~GpuImageImpl();

    [[nodiscard]] bool onOwnerThread() const noexcept;
    // Releases the view then the VMA image. Never called while a submission
    // references the image; callers only release after the producing fence has
    // signalled.
    void destroy() noexcept;

    std::shared_ptr<vulkan_detail::DeviceAllocatorState> state;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::optional<ImageWindow> dataWindow;
    std::optional<ImageWindow> displayWindow;
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    std::uint32_t generation = 0;
    bool deviceLost = false;
    // Set while this image is referenced by a readback submission whose completion
    // is not yet proved; the owner then retains (does not destroy) the image.
    bool submissionUnretired = false;
};

// Device-limit/format facts needed before creating a resident image. Checked
// explicitly; no blanket operation flag.
struct SolidImageSupport final {
    bool supported = false;
    std::uint64_t maxImageBytes = 0;
    std::string reason;
};

[[nodiscard]] SolidImageSupport querySolidImageSupport(vulkan_detail::DeviceAllocatorState& state,
                                                       std::uint32_t width,
                                                       std::uint32_t height) noexcept;

// Creates a device-local RGBA32F storage image with a color view. `usage`
// always includes STORAGE and SAMPLED (future transform/sourceover inputs) plus
// TRANSFER_SRC for the test-only readback path. Returns false and leaves `out`
// untouched on failure.
[[nodiscard]] bool createResidentImage(vulkan_detail::DeviceAllocatorState& state,
                                       std::uint32_t width, std::uint32_t height,
                                       GpuImageImpl& out);

} // namespace bloom::render

#endif // BLOOM_RENDER_VULKAN_GPU_IMAGE_PRIVATE_HPP
