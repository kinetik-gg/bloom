#pragma once

// Private Vulkan state for the present-image sampler. Never included by a public consumer or the
// CPU stub. The sampler pipeline is fixed configuration: one render pass/pipeline per swapchain
// color format, fullscreen triangle, two combined-image samplers and one std140 uniform block. The
// resident display image stays device-local: only a view is created for it, never a copy.

#if !defined(VK_NO_PROTOTYPES)
#define VK_NO_PROTOTYPES 1
#endif
#if !defined(VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL)
#define VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL 0
#endif
#if !defined(VULKAN_HPP_NO_EXCEPTIONS)
#define VULKAN_HPP_NO_EXCEPTIONS 1
#endif

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_present_image.hpp>
#include <bloom/render/gpu_presentation_target.hpp>
#include <bloom/render/gpu_resident_display.hpp>

#include "gpu_device_private.hpp"
#include "gpu_presentation_target_private.hpp"

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bloom::render {

struct GpuDisplayImageImpl;

// Read-only accessor defined by the resident-display translation unit (gpu_resident_display.cpp /
// _resources.cpp). It exposes only the read-only impl inside src/render; no native handle reaches a
// public header.
[[nodiscard]] const GpuDisplayImageImpl* gpuDisplayImageImpl(const GpuDisplayImage& image) noexcept;

namespace present_image_detail {

// std140 layout of the fragment shader's BloomPresentParams block. Keep in lockstep with
// viewer_present.frag.
struct PresentImageUniforms final {
    float targetExtent[4];
    float destinationRect[4];
    float imageWindow[4];
    float sourceExtent[4];
    float backgroundColor[4];
    float checkerColorA[4];
    float checkerColorB[4];
    float checkerParams[4];
    std::uint32_t modes[4];
};
static_assert(sizeof(PresentImageUniforms) == 144, "present uniform block must be 144 bytes");

// One fixed sampler pipeline plus the swapchain-generation render targets it draws into. Created
// lazily on the device owner thread and rebuilt when the swapchain/format generation changes.
struct PresentImagePipeline final {
    PresentImagePipeline() = default;
    PresentImagePipeline(const PresentImagePipeline&) = delete;
    PresentImagePipeline& operator=(const PresentImagePipeline&) = delete;
    ~PresentImagePipeline();

    [[nodiscard]] bool
    ensureDeviceResources(const std::shared_ptr<vulkan_detail::DeviceAllocatorState>& deviceControl,
                          std::string& message);
    [[nodiscard]] bool ensureSwapchainResources(presentation_detail::SwapchainResources& resources,
                                                std::string& message);
    // Creates/refreshes the cached view for `display` and commits its strong source pin together
    // with the view identity. `previousRenderComplete` reports that the previous frame's render
    // fence is proven complete, so the pin may be replaced. Returns false (without a dangling view)
    // when the view cannot be created.
    [[nodiscard]] bool ensureDisplayView(vulkan_detail::DeviceAllocatorState& control,
                                         const GpuDisplayImageImpl& display,
                                         const std::shared_ptr<const GpuDisplayImage>& pin,
                                         bool previousRenderComplete, std::string& message);
    [[nodiscard]] bool ensureOverlay(vulkan_detail::DeviceAllocatorState& control,
                                     const GpuPresentOverlay& overlay, std::string& message);

    // Drops the cached display view and its identity. Used when a frame's preparation fails after
    // the view was committed, so no later frame can reuse a view whose strong pin is gone.
    void invalidateDisplayView() noexcept;

    // Destroys every device- and generation-dependent resource in dependency order (framebuffers
    // and views before the render pass, the graphics pipeline before the pipeline layout, the
    // descriptor set before its pool, and the pipeline layout before its set layout) so a partial
    // creation failure or a device change can be retried without destroying a dependent object
    // after its dependency.
    void resetDeviceResources() noexcept;

    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;
    // True only after every required device resource was created successfully. A partial failure
    // (e.g. the fragment shader) must never be treated as ready just because the vertex shader
    // exists.
    bool deviceResourcesReady = false;
    vk::raii::ShaderModule vertexShader{nullptr};
    vk::raii::ShaderModule fragmentShader{nullptr};
    vk::raii::DescriptorSetLayout setLayout{nullptr};
    vk::raii::PipelineLayout pipelineLayout{nullptr};
    vk::raii::DescriptorPool descriptorPool{nullptr};
    vk::raii::DescriptorSet descriptorSet{nullptr};
    vk::raii::Sampler sampler{nullptr};
    vk::raii::RenderPass renderPass{nullptr};
    vk::raii::Pipeline pipeline{nullptr};
    VkFormat pipelineFormat = VK_FORMAT_UNDEFINED;

    // Strong pin: kept until the render fence that sampled it is known complete.
    std::shared_ptr<const GpuDisplayImage> pinnedInput;
    vk::raii::ImageView displayView{nullptr};
    VkImage displayImage = VK_NULL_HANDLE;
    std::uint32_t displayGeneration = 0;

    VkBuffer paramsBuffer = VK_NULL_HANDLE;
    VmaAllocation paramsAllocation = VK_NULL_HANDLE;
    void* paramsMapped = nullptr;

    // Raw VMA-owned image; never wrapped in vk::raii (VMA owns the VkImage lifetime).
    VkImage overlayImage = VK_NULL_HANDLE;
    vk::raii::ImageView overlayView{nullptr};
    VmaAllocation overlayAllocation = VK_NULL_HANDLE;
    std::uint32_t overlayWidth = 0;
    std::uint32_t overlayHeight = 0;
    std::uint64_t overlayToken = 0;

    // Overlay staging retained until the render fence that copied it is known complete.
    VkBuffer stagedOverlayBuffer = VK_NULL_HANDLE;
    VmaAllocation stagedOverlayAllocation = VK_NULL_HANDLE;
    std::uint64_t stagedOverlayToken = 0;
    std::uint32_t overlayRowBytes = 0;
    bool stagedOverlayPending = false;
    bool overlayImageInitialized = false;

    std::vector<vk::raii::ImageView> swapchainViews;
    std::vector<vk::raii::Framebuffer> framebuffers;
    VkSwapchainKHR builtSwapchain = VK_NULL_HANDLE;
    // First target image plus the swapchain generation counter distinguish offscreen test targets
    // (no swapchain handle) that share a format/finalLayout but carry a different attachment, and
    // any swapchain generation. Raw handles alone are not identity: a destroyed swapchain/image can
    // be recycled with the same bits.
    VkImage builtImage = VK_NULL_HANDLE;
    std::uint64_t builtGeneration = 0;
    VkFormat builtFormat = VK_FORMAT_UNDEFINED;
    VkImageLayout builtFinalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::uint32_t builtImageCount = 0;
};

// Test-only device-resource construction fault injector: forces ensureDeviceResources to stop
// after `stage` completed stages (0 = before the vertex shader, 1 = after the vertex shader but
// before the fragment shader). Default (UINT32_MAX) is no fault. Used to prove a partial creation
// failure never marks the pipeline ready and a later attempt safely rebuilds.
void setPresentDeviceResourceFaultForTesting(std::uint32_t stage) noexcept;
void clearPresentDeviceResourceFaultForTesting() noexcept;

} // namespace present_image_detail
} // namespace bloom::render
