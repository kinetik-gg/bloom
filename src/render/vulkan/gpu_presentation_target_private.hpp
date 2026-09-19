#pragma once

// Private Vulkan-backed state for the presentation target. Never included by a public consumer,
// the CPU stub, or a UI translation unit. It reuses the device's already-enumerated
// Vulkan-Hpp native ownership: the swapchain, semaphores, fences, and command pool are RAII-owned
// and are destroyed on the owner thread, only after the presentation engine is proven done with
// every image.

#if !defined(VK_NO_PROTOTYPES)
#define VK_NO_PROTOTYPES 1
#endif
#if !defined(VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL)
#define VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL 0
#endif
#if !defined(VULKAN_HPP_NO_EXCEPTIONS)
#define VULKAN_HPP_NO_EXCEPTIONS 1
#endif

#include <bloom/render/gpu_presentation_target.hpp>

#include "gpu_device_private.hpp"

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bloom::render::presentation_detail {

[[nodiscard]] inline VkSurfaceKHR toSurfaceHandle(const std::uint64_t bits) noexcept {
    return reinterpret_cast<VkSurfaceKHR>(static_cast<std::uintptr_t>(bits));
}

// One swapchain generation and every resource that references it. It is created in full by its
// constructor and torn down in full by its destructor on the owner thread. The acquire semaphore is
// a single reused handle because this package bounds inflight work to one acquire/present at a
// time; the render-complete semaphore and the present fence are per swapchain image, so an image's
// semaphore is never re-signaled until the presentation engine is proven done with it.
struct SwapchainResources final {
    vk::raii::SwapchainKHR swapchain{nullptr};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    GpuPresentationRetirement retirement = GpuPresentationRetirement::None;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<VkImage> images;
    std::vector<vk::raii::Semaphore> renderSemaphores;
    std::vector<vk::raii::Fence> presentFences; // empty unless retirement == PresentFence
    std::vector<bool> presentPending;
    std::vector<std::uint64_t> presentIds;
    vk::raii::Semaphore acquireSemaphore{nullptr};
    vk::raii::Fence renderFence{nullptr};
    vk::raii::CommandPool commandPool{nullptr};
    std::vector<vk::raii::CommandBuffer> commandBuffers;

    // Set true only after every member has been created successfully. create()/recreate() publish a
    // target only when this is true, so a partially constructed generation (images enumerated but a
    // semaphore/fence/pool failing afterwards) is never handed to a caller.
    bool valid = false;
    bool renderInFlight = false;
    bool presentOutstanding = false;
    std::uint64_t nextPresentId = 1;
    std::optional<std::uint32_t> acquiredIndex;

    SwapchainResources(vk::raii::Device const& device, vk::raii::PhysicalDevice const& physical,
                       VkSurfaceKHR surface, std::uint32_t computeQueueFamily,
                       std::uint32_t presentQueueFamily, VkExtent2D extent,
                       std::uint32_t requestedImageCount, VkFormat chosenFormat,
                       VkColorSpaceKHR chosenColorSpace, VkPresentModeKHR chosenPresentMode,
                       GpuPresentationRetirement chosenRetirement,
                       VkSurfaceTransformFlagBitsKHR preTransform,
                       VkCompositeAlphaFlagBitsKHR compositeAlpha, VkSwapchainKHR oldSwapchain,
                       std::string& error);
};

// Format/present-mode/extent selection. All are pure queries against the surface capabilities and
// the offered formats; none touches the swapchain.
[[nodiscard]] bool selectFormat(const std::vector<VkSurfaceFormatKHR>& formats, VkFormat& format,
                                VkColorSpaceKHR& colorSpace, std::string& error);
[[nodiscard]] VkPresentModeKHR selectPresentMode(const std::vector<VkPresentModeKHR>& modes);
[[nodiscard]] VkExtent2D computeExtent(const VkSurfaceCapabilitiesKHR& capabilities,
                                       std::uint32_t requestedWidth,
                                       std::uint32_t requestedHeight) noexcept;
[[nodiscard]] std::uint32_t
computeImageCount(const VkSurfaceCapabilitiesKHR& capabilities) noexcept;

// Driver operations. Each is owner-thread-only and returns the target code plus a message. None
// waits idle: acquire uses a zero timeout, present is asynchronous, and retirement polling uses
// vkGetFenceStatus or a zero-timeout vkWaitForPresentKHR.
[[nodiscard]] GpuPresentationTargetCode
acquireImage(SwapchainResources& resources, vk::raii::Device const& device, std::string& message);
[[nodiscard]] GpuPresentationTargetCode presentImage(SwapchainResources& resources,
                                                     vk::raii::Device const& device,
                                                     vk::raii::Queue const& presentQueue,
                                                     GpuClearColor color, std::string& message);
// Returns RetirePending while any presentation is unproven (including an unknown driver result),
// Ok when the swapchain is idle but not retiring, and Retired when it is safe to destroy.
[[nodiscard]] GpuPresentationTargetCode pollPresent(SwapchainResources& resources,
                                                    vk::raii::Device const& device, bool retiring,
                                                    std::string& message);

// --- Narrow, device-free seams for the deterministic CPU tests -----------------------------------

// True only after the constructor reached its final successful step.
[[nodiscard]] bool swapchainResourcesValid(const SwapchainResources& resources) noexcept;
// True while any present has not been proven complete by the presentation engine.
[[nodiscard]] bool presentStillPending(const SwapchainResources& resources) noexcept;
// Classifies a fence/present-wait result conservatively: only VK_SUCCESS proves completion;
// VK_ERROR_DEVICE_LOST is terminal; anything else (timeout/unknown) is unproven.
[[nodiscard]] GpuPresentationTargetCode presentationCodeFromWaitResult(VkResult result) noexcept;

// Test-only construction fault injector: forces the SwapchainResources constructor to stop after
// `stage` completed stages (0 = before the swapchain). Default (UINT32_MAX) is no fault. Used to
// prove a mid-construction failure publishes no target and never calls the driver through a null
// object.
void setSwapchainConstructionFaultForTesting(std::uint32_t stage) noexcept;
void clearSwapchainConstructionFaultForTesting() noexcept;

} // namespace bloom::render::presentation_detail
