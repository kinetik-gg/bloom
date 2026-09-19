#include "gpu_presentation_target_private.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bloom::render::presentation_detail {
namespace {

std::atomic<std::uint32_t> gConstructionFault{UINT32_MAX};

[[nodiscard]] bool faultAt(const std::uint32_t stage) noexcept {
    return gConstructionFault.load(std::memory_order_acquire) == stage;
}

// Creates the swapchain through the raw entry point and adopts the handle into RAII ownership,
// mirroring the existing render-resource idiom (no exception-enabled Vulkan-Hpp create wrappers).
[[nodiscard]] vk::raii::SwapchainKHR
makeSwapchain(vk::raii::Device const& device, VkSurfaceKHR surface,
              std::uint32_t computeQueueFamily, std::uint32_t presentQueueFamily, VkExtent2D extent,
              VkFormat format, VkColorSpaceKHR colorSpace, VkPresentModeKHR presentMode,
              std::uint32_t imageCount, VkSurfaceTransformFlagBitsKHR preTransform,
              VkCompositeAlphaFlagBitsKHR compositeAlpha, VkSwapchainKHR oldSwapchain) {
    const std::uint32_t familyIndices[2] = {computeQueueFamily, presentQueueFamily};
    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = format;
    createInfo.imageColorSpace = colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (computeQueueFamily == presentQueueFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = familyIndices;
    }
    createInfo.preTransform = preTransform;
    createInfo.compositeAlpha = compositeAlpha;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = oldSwapchain;

    VkSwapchainKHR raw = VK_NULL_HANDLE;
    const auto* dispatcher = device.getDispatcher();
    if (dispatcher->vkCreateSwapchainKHR == nullptr ||
        dispatcher->vkCreateSwapchainKHR(*device, &createInfo, nullptr, &raw) != VK_SUCCESS ||
        raw == VK_NULL_HANDLE) {
        return vk::raii::SwapchainKHR{nullptr};
    }
    return vk::raii::SwapchainKHR(device, raw);
}

[[nodiscard]] vk::raii::Semaphore makeSemaphore(vk::raii::Device const& device) {
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore raw = VK_NULL_HANDLE;
    const auto* dispatcher = device.getDispatcher();
    if (dispatcher->vkCreateSemaphore == nullptr ||
        dispatcher->vkCreateSemaphore(*device, &info, nullptr, &raw) != VK_SUCCESS ||
        raw == VK_NULL_HANDLE) {
        return vk::raii::Semaphore{nullptr};
    }
    return vk::raii::Semaphore(device, raw);
}

[[nodiscard]] vk::raii::Fence makeFence(vk::raii::Device const& device) {
    VkFenceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence raw = VK_NULL_HANDLE;
    const auto* dispatcher = device.getDispatcher();
    if (dispatcher->vkCreateFence == nullptr ||
        dispatcher->vkCreateFence(*device, &info, nullptr, &raw) != VK_SUCCESS ||
        raw == VK_NULL_HANDLE) {
        return vk::raii::Fence{nullptr};
    }
    return vk::raii::Fence(device, raw);
}

} // namespace

SwapchainResources::SwapchainResources(
    vk::raii::Device const& device, vk::raii::PhysicalDevice const& physical, VkSurfaceKHR surface,
    std::uint32_t computeQueueFamily, std::uint32_t presentQueueFamily, VkExtent2D extent,
    std::uint32_t requestedImageCount, VkFormat chosenFormat, VkColorSpaceKHR chosenColorSpace,
    VkPresentModeKHR chosenPresentMode, GpuPresentationRetirement chosenRetirement,
    VkSurfaceTransformFlagBitsKHR preTransform, VkCompositeAlphaFlagBitsKHR compositeAlpha,
    VkSwapchainKHR oldSwapchain, std::string& error)
    : format(chosenFormat), colorSpace(chosenColorSpace), presentMode(chosenPresentMode),
      retirement(chosenRetirement), width(extent.width), height(extent.height) {
    static_cast<void>(physical);
    const auto* dispatcher = device.getDispatcher();
    if (faultAt(0U)) {
        error = "injected swapchain-construction fault before creation";
        return;
    }
    swapchain = makeSwapchain(device, surface, computeQueueFamily, presentQueueFamily, extent,
                              chosenFormat, chosenColorSpace, chosenPresentMode,
                              requestedImageCount, preTransform, compositeAlpha, oldSwapchain);
    if (!*swapchain) {
        error = "the swapchain could not be created";
        return;
    }
    if (faultAt(1U)) {
        error = "injected swapchain-construction fault after creation";
        return;
    }

    std::uint32_t imageCount = 0;
    if (dispatcher->vkGetSwapchainImagesKHR == nullptr ||
        dispatcher->vkGetSwapchainImagesKHR(*device, *swapchain, &imageCount, nullptr) !=
            VK_SUCCESS ||
        imageCount == 0U) {
        error = "the swapchain reported no images";
        return;
    }
    images.resize(imageCount);
    if (dispatcher->vkGetSwapchainImagesKHR(*device, *swapchain, &imageCount, images.data()) !=
        VK_SUCCESS) {
        error = "the swapchain images could not be enumerated";
        return;
    }
    images.resize(imageCount);
    if (faultAt(2U)) {
        error = "injected swapchain-construction fault after image enumeration";
        return;
    }

    acquireSemaphore = makeSemaphore(device);
    renderFence = makeFence(device);
    if (!*acquireSemaphore || !*renderFence) {
        error = "the presentation synchronization objects could not be created";
        return;
    }

    renderSemaphores.reserve(imageCount);
    presentPending.assign(imageCount, false);
    presentIds.assign(imageCount, 0);
    for (std::size_t index = 0; index < imageCount; ++index) {
        vk::raii::Semaphore semaphore = makeSemaphore(device);
        if (!*semaphore) {
            error = "a per-image presentation semaphore could not be created";
            return;
        }
        renderSemaphores.emplace_back(std::move(semaphore));
    }
    if (retirement == GpuPresentationRetirement::PresentFence) {
        presentFences.reserve(imageCount);
        for (std::size_t index = 0; index < imageCount; ++index) {
            vk::raii::Fence fence = makeFence(device);
            if (!*fence) {
                error = "a per-image present fence could not be created";
                return;
            }
            presentFences.emplace_back(std::move(fence));
        }
    }
    if (faultAt(3U)) {
        error = "injected swapchain-construction fault after synchronization objects";
        return;
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = presentQueueFamily;
    VkCommandPool rawPool = VK_NULL_HANDLE;
    if (dispatcher->vkCreateCommandPool == nullptr ||
        dispatcher->vkCreateCommandPool(*device, &poolInfo, nullptr, &rawPool) != VK_SUCCESS ||
        rawPool == VK_NULL_HANDLE) {
        error = "the presentation command pool could not be created";
        return;
    }
    commandPool = vk::raii::CommandPool(device, rawPool);
    if (faultAt(4U)) {
        error = "injected swapchain-construction fault after the command pool";
        return;
    }

    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = rawPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    VkCommandBuffer rawCommand = VK_NULL_HANDLE;
    if (dispatcher->vkAllocateCommandBuffers == nullptr ||
        dispatcher->vkAllocateCommandBuffers(*device, &allocateInfo, &rawCommand) != VK_SUCCESS ||
        rawCommand == VK_NULL_HANDLE) {
        error = "the presentation command buffer could not be allocated";
        return;
    }
    commandBuffers.emplace_back(device, rawCommand, rawPool);
    valid = true;
}

bool selectFormat(const std::vector<VkSurfaceFormatKHR>& formats, VkFormat& format,
                  VkColorSpaceKHR& colorSpace, std::string& error) {
    if (formats.empty()) {
        error = "the surface offered no formats";
        return false;
    }
    if (formats.size() == 1U && formats[0].format == VK_FORMAT_UNDEFINED) {
        format = VK_FORMAT_B8G8R8A8_UNORM;
        colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        return true;
    }
    // Encoded display output: only the packed 8-bit BGRA/RGBA forms with an SRGB_NONLINEAR color
    // space are usable without a second gamma encode. Any other format/color-space pairing is
    // refused (the caller keeps the CPU path) rather than substituted.
    const auto match = [&](const VkFormat wanted) -> bool {
        for (const VkSurfaceFormatKHR& candidate : formats) {
            if (candidate.format == wanted &&
                candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                format = candidate.format;
                colorSpace = candidate.colorSpace;
                return true;
            }
        }
        return false;
    };
    if (match(VK_FORMAT_B8G8R8A8_UNORM) || match(VK_FORMAT_R8G8B8A8_UNORM)) {
        return true;
    }
    // The _SRGB formats are deliberately not accepted: the display product already emits
    // sRGB-encoded bytes, and an SRGB swapchain format would encode them a second time. An
    // unknown/unsupported pairing is refused so the caller keeps the CPU path.
    error = "the surface offers no encoded BGRA8/RGBA8 UNORM format with an SRGB_NONLINEAR color "
            "space";
    return false;
}

VkPresentModeKHR selectPresentMode(const std::vector<VkPresentModeKHR>& modes) {
    if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) {
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D computeExtent(const VkSurfaceCapabilitiesKHR& capabilities, std::uint32_t requestedWidth,
                         std::uint32_t requestedHeight) noexcept {
    if (capabilities.currentExtent.width != UINT32_MAX) {
        return capabilities.currentExtent;
    }
    const std::uint32_t width =
        std::clamp(requestedWidth == 0U ? capabilities.minImageExtent.width : requestedWidth,
                   capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    const std::uint32_t height =
        std::clamp(requestedHeight == 0U ? capabilities.minImageExtent.height : requestedHeight,
                   capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    return VkExtent2D{.width = width, .height = height};
}

std::uint32_t computeImageCount(const VkSurfaceCapabilitiesKHR& capabilities) noexcept {
    const std::uint32_t wanted = capabilities.minImageCount + 1U;
    return std::clamp(wanted, capabilities.minImageCount, capabilities.maxImageCount);
}

GpuPresentationTargetCode acquireImage(SwapchainResources& resources,
                                       vk::raii::Device const& device, std::string& message) {
    if (resources.presentOutstanding || resources.renderInFlight) {
        message = "a frame is already in flight";
        return GpuPresentationTargetCode::NotReady;
    }
    const auto* dispatcher = device.getDispatcher();
    if (dispatcher->vkAcquireNextImageKHR == nullptr) {
        message = "the driver exposes no vkAcquireNextImageKHR";
        return GpuPresentationTargetCode::DriverUnavailable;
    }
    std::uint32_t index = 0;
    const VkResult result = dispatcher->vkAcquireNextImageKHR(
        *device, *resources.swapchain, 0, *resources.acquireSemaphore, VK_NULL_HANDLE, &index);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        if (index >= resources.images.size()) {
            message = "the acquired image index is out of range";
            return GpuPresentationTargetCode::DriverUnavailable;
        }
        resources.acquiredIndex = index;
        return result == VK_SUBOPTIMAL_KHR ? GpuPresentationTargetCode::Suboptimal
                                           : GpuPresentationTargetCode::Ok;
    }
    if (result == VK_NOT_READY) {
        return GpuPresentationTargetCode::NotReady;
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return GpuPresentationTargetCode::OutOfDate;
    }
    if (result == VK_ERROR_DEVICE_LOST) {
        return GpuPresentationTargetCode::DeviceLost;
    }
    message = "vkAcquireNextImageKHR returned an unexpected result";
    return GpuPresentationTargetCode::DriverUnavailable;
}

GpuPresentationTargetCode presentImage(SwapchainResources& resources,
                                       vk::raii::Device const& device,
                                       vk::raii::Queue const& presentQueue, GpuClearColor color,
                                       std::string& message) {
    if (!resources.acquiredIndex.has_value()) {
        message = "no image was acquired for this present";
        return GpuPresentationTargetCode::NothingAcquired;
    }
    const std::uint32_t index = *resources.acquiredIndex;
    if (index >= resources.images.size() || resources.commandBuffers.empty()) {
        message = "the acquired image has no presentation resources";
        return GpuPresentationTargetCode::DriverUnavailable;
    }
    const auto* dispatcher = device.getDispatcher();
    const VkImage image = resources.images[index];
    const VkCommandBuffer command = *resources.commandBuffers.front();

    dispatcher->vkResetCommandBuffer(command, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (dispatcher->vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
        message = "the presentation command buffer could not be begun";
        return GpuPresentationTargetCode::DriverUnavailable;
    }
    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dispatcher->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                     &toTransfer);
    VkClearColorValue clear{};
    clear.float32[0] = static_cast<float>(color.red);
    clear.float32[1] = static_cast<float>(color.green);
    clear.float32[2] = static_cast<float>(color.blue);
    clear.float32[3] = static_cast<float>(color.alpha);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dispatcher->vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear,
                                     1, &range);
    VkImageMemoryBarrier toPresent = toTransfer;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    dispatcher->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &toPresent);
    if (dispatcher->vkEndCommandBuffer(command) != VK_SUCCESS) {
        message = "the presentation command buffer could not be recorded";
        return GpuPresentationTargetCode::DriverUnavailable;
    }

    const VkSemaphore acquire = *resources.acquireSemaphore;
    const VkSemaphore render = *resources.renderSemaphores[index];
    const VkFence renderFence = *resources.renderFence;
    dispatcher->vkResetFences(*device, 1, &renderFence);
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquire;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render;
    if (dispatcher->vkQueueSubmit(*presentQueue, 1, &submit, renderFence) != VK_SUCCESS) {
        message = "the clear command could not be submitted";
        return GpuPresentationTargetCode::DriverUnavailable;
    }
    resources.renderInFlight = true;

    const VkSwapchainKHR swapchain = *resources.swapchain;
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &index;

    VkSwapchainPresentFenceInfoEXT fenceInfo{};
    VkPresentIdKHR presentIdInfo{};
    if (resources.retirement == GpuPresentationRetirement::PresentFence) {
        if (index >= resources.presentFences.size()) {
            message = "the present fence for the acquired image is missing";
            return GpuPresentationTargetCode::NoRetirementMechanism;
        }
        VkFence presentFence = *resources.presentFences[index];
        dispatcher->vkResetFences(*device, 1, &presentFence);
        fenceInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
        fenceInfo.swapchainCount = 1;
        fenceInfo.pFences = &presentFence;
        present.pNext = &fenceInfo;
    } else if (resources.retirement == GpuPresentationRetirement::PresentWait) {
        const std::uint64_t presentId = resources.nextPresentId++;
        resources.presentIds[index] = presentId;
        presentIdInfo.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
        presentIdInfo.swapchainCount = 1;
        presentIdInfo.pPresentIds = &presentId;
        present.pNext = &presentIdInfo;
    } else {
        message = "no supported presentation-retirement mechanism";
        return GpuPresentationTargetCode::NoRetirementMechanism;
    }

    // The image and its acquire semaphore are handed to the presentation engine at the moment the
    // present request is queued. Mark the image conservatively before the call so no result path —
    // success, OutOfDate, unknown, or DeviceLost — can let a later render-fence observation report
    // a false retirement. The pending flag is only cleared when the chosen retirement mechanism
    // actually proves the engine is done with the image.
    resources.presentPending[index] = true;
    resources.presentOutstanding = true;
    resources.acquiredIndex.reset();
    const VkResult result = dispatcher->vkQueuePresentKHR(*presentQueue, &present);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        return result == VK_SUBOPTIMAL_KHR ? GpuPresentationTargetCode::Suboptimal
                                           : GpuPresentationTargetCode::Ok;
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        // Vulkan does not guarantee a failed/out-of-date present left the image unreferenced, so
        // the pending state is retained until the retirement mechanism proves otherwise.
        message = "the swapchain is out of date; retirement is retained until proven";
        return GpuPresentationTargetCode::OutOfDate;
    }
    if (result == VK_ERROR_DEVICE_LOST) {
        message = "the device was lost while presenting";
        return GpuPresentationTargetCode::DeviceLost;
    }
    message = "vkQueuePresentKHR returned an unexpected result; the image remains unretired";
    return GpuPresentationTargetCode::DriverUnavailable;
}

GpuPresentationTargetCode pollPresent(SwapchainResources& resources, vk::raii::Device const& device,
                                      const bool retiring, std::string& message) {
    const auto* dispatcher = device.getDispatcher();
    if (resources.renderInFlight && dispatcher->vkGetFenceStatus != nullptr) {
        const VkResult renderResult = dispatcher->vkGetFenceStatus(*device, *resources.renderFence);
        if (renderResult == VK_SUCCESS) {
            resources.renderInFlight = false;
        } else if (renderResult == VK_ERROR_DEVICE_LOST) {
            message = "the device was lost while the render fence was outstanding";
            return GpuPresentationTargetCode::DeviceLost;
        }
    }
    if (resources.presentOutstanding) {
        bool anyPending = false;
        for (std::size_t index = 0; index < resources.presentPending.size(); ++index) {
            if (!resources.presentPending[index]) {
                continue;
            }
            if (resources.retirement == GpuPresentationRetirement::PresentFence) {
                if (index >= resources.presentFences.size()) {
                    message = "the present fence for a pending image is missing";
                    anyPending = true;
                    continue;
                }
                const VkResult presentFence =
                    dispatcher->vkGetFenceStatus(*device, *resources.presentFences[index]);
                if (presentFence == VK_SUCCESS) {
                    resources.presentPending[index] = false;
                } else if (presentFence == VK_ERROR_DEVICE_LOST) {
                    message = "the device was lost while a present fence was outstanding";
                    return GpuPresentationTargetCode::DeviceLost;
                } else {
                    anyPending = true;
                }
            } else if (resources.retirement == GpuPresentationRetirement::PresentWait) {
                const VkResult result = dispatcher->vkWaitForPresentKHR(
                    *device, *resources.swapchain, resources.presentIds[index], 0);
                if (result == VK_SUCCESS) {
                    resources.presentPending[index] = false;
                } else if (result == VK_ERROR_DEVICE_LOST) {
                    return GpuPresentationTargetCode::DeviceLost;
                } else {
                    anyPending = true;
                    message = "presentation completion is not yet proven";
                }
            } else {
                anyPending = true;
            }
        }
        resources.presentOutstanding = anyPending;
    }
    if (retiring) {
        if (!resources.presentOutstanding && !resources.renderInFlight &&
            !resources.acquiredIndex.has_value()) {
            return GpuPresentationTargetCode::Retired;
        }
        return GpuPresentationTargetCode::RetirePending;
    }
    return GpuPresentationTargetCode::Ok;
}

bool swapchainResourcesValid(const SwapchainResources& resources) noexcept {
    return resources.valid;
}

bool presentStillPending(const SwapchainResources& resources) noexcept {
    for (const bool pending : resources.presentPending) {
        if (pending) {
            return true;
        }
    }
    return false;
}

GpuPresentationTargetCode presentationCodeFromWaitResult(const VkResult result) noexcept {
    if (result == VK_SUCCESS) {
        return GpuPresentationTargetCode::Ok;
    }
    if (result == VK_ERROR_DEVICE_LOST) {
        return GpuPresentationTargetCode::DeviceLost;
    }
    return GpuPresentationTargetCode::RetirePending;
}

void setSwapchainConstructionFaultForTesting(const std::uint32_t stage) noexcept {
    gConstructionFault.store(stage, std::memory_order_release);
}

void clearSwapchainConstructionFaultForTesting() noexcept {
    gConstructionFault.store(UINT32_MAX, std::memory_order_release);
}

} // namespace bloom::render::presentation_detail
