#pragma once

// Shared native harness for the offscreen present proof: device/image/upload/resident-display
// helpers plus the test-only offscreen target that drives the production present draw into a VMA
// RGBA8/BGRA8 attachment and reads it back. Compiled against the frozen private headers only.

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_present_image.hpp>
#include <bloom/render/gpu_presentation_target.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>

#include "gpu_device_private.hpp"
#include "gpu_image_private.hpp"
#include "gpu_present_image_private.hpp"
#include "gpu_presentation_target_private.hpp"
#include "gpu_resident_display_private.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan_raii.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::render::present_native_support {

using bloom::core::PixelAspectRatio;
using bloom::render::GpuDevice;
using bloom::render::GpuDisplayImage;
using bloom::render::GpuDisplayImageReadbackCode;
using bloom::render::GpuImage;
using bloom::render::GpuImageUpload;
using bloom::render::GpuImageUploadDiagnosticCode;
using bloom::render::GpuImageUploadPollResult;
using bloom::render::GpuPresentationTargetCode;
using bloom::render::GpuPresentBackground;
using bloom::render::GpuPresentColor;
using bloom::render::GpuPresentImageParams;
using bloom::render::GpuPresentOverlay;
using bloom::render::GpuPresentRect;
using bloom::render::GpuPresentSourceWindow;
using bloom::render::GpuResidentDisplay;
using bloom::render::GpuResidentDisplayDiagnosticCode;
using bloom::render::GpuResidentDisplayPollResult;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;
using bloom::render::Rgba8;
using bloom::render::present_image_detail::PresentImagePipeline;
using bloom::render::present_image_detail::renderResidentIntoAcquired;
using bloom::render::presentation_detail::SwapchainResources;
using bloom::render::vulkan_detail::DeviceAllocatorState;

inline constexpr std::uint64_t kBudget = std::uint64_t{1} << 32;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

struct Options final {
    std::filesystem::path loader_path;
    bool require_device = false;
    bool valid = true;
};

[[nodiscard]] inline Options parseOptions(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader") {
            if (index + 1 >= argc) {
                options.valid = false;
                return options;
            }
            options.loader_path = argv[++index];
        } else if (argument == "--require-device") {
            options.require_device = true;
        } else {
            options.valid = false;
            return options;
        }
    }
    return options;
}

[[nodiscard]] inline std::optional<ImageWindow>
window(const std::int64_t x, const std::int64_t y, const std::uint64_t w, const std::uint64_t h) {
    const auto result = ImageWindow::create(x, y, w, h);
    return result ? std::optional(*result.value()) : std::nullopt;
}

[[nodiscard]] inline std::optional<Rgba32f> pixel(const float r, const float g, const float b,
                                                  const float a) {
    const auto result = Rgba32f::fromPremultiplied(r, g, b, a);
    return result ? std::optional(*result.value()) : std::nullopt;
}

[[nodiscard]] inline std::optional<Rgba32fImage> makeImage(const ImageWindow imageWindow,
                                                           const std::vector<Rgba32f>& pixels) {
    const auto descriptor =
        Rgba32fImageDescriptor::create(imageWindow, imageWindow, PixelAspectRatio::square());
    if (!descriptor || pixels.size() != descriptor.value()->layout().pixelCount) {
        return std::nullopt;
    }
    auto builder = Rgba32fImageBuilder::create(*descriptor.value(), kBudget);
    if (!builder) {
        return std::nullopt;
    }
    const auto width = imageWindow.extent().width();
    for (std::uint32_t y = 0; y < imageWindow.extent().height(); ++y) {
        const auto row = builder.value()->row(imageWindow.originY() + y);
        if (!row) {
            return std::nullopt;
        }
        for (std::uint32_t x = 0; x < width; ++x) {
            (*row.value())[x] = pixels[static_cast<std::size_t>(y) * width + x];
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    return frozen ? std::optional(std::move(*frozen.value())) : std::nullopt;
}

[[nodiscard]] inline std::optional<GpuImage> upload(GpuImageUpload& uploader,
                                                    std::shared_ptr<const Rgba32fImage> source) {
    if (uploader.begin({std::move(source)}, kBudget).code != GpuImageUploadDiagnosticCode::None) {
        return std::nullopt;
    }
    GpuImageUploadPollResult poll = GpuImageUploadPollResult::Pending;
    while (poll == GpuImageUploadPollResult::Pending) {
        poll = uploader.poll();
    }
    return poll == GpuImageUploadPollResult::Ready ? std::optional(uploader.takeImage())
                                                   : std::nullopt;
}

// Runs the resident display job and reads the RGBA8 output back. The readback seeds the independent
// oracle; the displayed bytes are the exact bytes the present shader samples.
[[nodiscard]] inline std::optional<std::vector<Rgba8>>
displayReadback(GpuResidentDisplay& display, const std::shared_ptr<const GpuImage>& input) {
    const auto begin = display.begin(input, kBudget);
    if (begin.code != GpuResidentDisplayDiagnosticCode::None) {
        std::cerr << "resident display begin rejected: " << begin.message << '\n';
        return std::nullopt;
    }
    GpuResidentDisplayPollResult poll = GpuResidentDisplayPollResult::Pending;
    while (poll == GpuResidentDisplayPollResult::Pending) {
        poll = display.poll();
    }
    if (poll != GpuResidentDisplayPollResult::Ready) {
        std::cerr << "resident display job failed: " << display.diagnostic().message << '\n';
        return std::nullopt;
    }
    const auto readback = display.readback();
    if (readback.code != GpuDisplayImageReadbackCode::None) {
        std::cerr << "resident display readback failed: " << readback.message << '\n';
        return std::nullopt;
    }
    return readback.pixels;
}

struct ReadbackTarget final {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
};

inline void destroyTarget(DeviceAllocatorState& control, ReadbackTarget& target) noexcept {
    if (target.image != VK_NULL_HANDLE) {
        vmaDestroyImage(control.allocator, target.image, target.allocation);
    }
    target.image = VK_NULL_HANDLE;
    target.allocation = VK_NULL_HANDLE;
}

[[nodiscard]] inline bool createTarget(DeviceAllocatorState& control, const VkFormat format,
                                       const std::uint32_t width, const std::uint32_t height,
                                       ReadbackTarget& target) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    return vmaCreateImage(control.allocator, &imageInfo, &allocationInfo, &target.image,
                          &target.allocation, nullptr) == VK_SUCCESS;
}

struct OffscreenResult final {
    GpuPresentationTargetCode code = GpuPresentationTargetCode::PresentationUnavailable;
    bool ok = false;
    std::string message;
    std::vector<Rgba8> pixels;
};

// Renders through the production path with a caller-owned, reusable presenter and reads the
// attachment back as semantic RGBA8 regardless of channel packing.
[[nodiscard]] inline OffscreenResult
renderOffscreenWith(const std::shared_ptr<DeviceAllocatorState>& control,
                    const std::shared_ptr<PresentImagePipeline>& presenter, const VkFormat format,
                    const std::uint32_t width, const std::uint32_t height,
                    std::shared_ptr<const GpuDisplayImage> input,
                    const GpuPresentImageParams& params, const GpuPresentOverlay& overlay) {
    OffscreenResult result;
    ReadbackTarget target;
    if (!createTarget(*control, format, width, height, target)) {
        result.message = "the offscreen attachment could not be created";
        return result;
    }
    const auto* dispatcher = control->device.getDispatcher();
    const VkExtent2D extent{width, height};
    SwapchainResources resources(control->device, extent, format, target.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, control->computeQueueFamily);
    if (resources.commandBuffers.empty() || !*resources.renderFence) {
        result.message = "the offscreen swapchain resources are incomplete";
        destroyTarget(*control, target);
        return result;
    }
    std::shared_ptr<PresentImagePipeline> localPresenter = presenter;
    result.code = renderResidentIntoAcquired(resources, control, localPresenter, std::move(input),
                                             params, overlay, result.message);
    if (result.code != GpuPresentationTargetCode::Ok) {
        destroyTarget(*control, target);
        return result;
    }

    const VkCommandBuffer renderCommand = *resources.commandBuffers.front();
    const VkFence renderFence = *resources.renderFence;
    dispatcher->vkResetFences(*control->device, 1, &renderFence);
    VkSubmitInfo renderSubmit{};
    renderSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    renderSubmit.commandBufferCount = 1;
    renderSubmit.pCommandBuffers = &renderCommand;
    if (dispatcher->vkQueueSubmit(*control->computeQueue, 1, &renderSubmit, renderFence) !=
        VK_SUCCESS) {
        result.message = "the offscreen present draw could not be submitted";
        destroyTarget(*control, target);
        return result;
    }

    VkBuffer readbackBuffer = VK_NULL_HANDLE;
    VmaAllocation readbackAllocation = VK_NULL_HANDLE;
    VmaAllocationInfo readbackInfo{};
    vk::raii::CommandPool readPool{nullptr};
    vk::raii::CommandBuffer readCommand{nullptr};
    vk::raii::Fence readFence{nullptr};
    const auto cleanup = [&]() {
        if (readbackBuffer != VK_NULL_HANDLE && readbackAllocation != VK_NULL_HANDLE) {
            vmaDestroyBuffer(control->allocator, readbackBuffer, readbackAllocation);
        }
        readbackBuffer = VK_NULL_HANDLE;
        readbackAllocation = VK_NULL_HANDLE;
        destroyTarget(*control, target);
    };

    const VkResult renderWait = dispatcher->vkWaitForFences(
        *control->device, 1, &renderFence, VK_TRUE, 5ULL * 1000ULL * 1000ULL * 1000ULL);
    if (renderWait != VK_SUCCESS) {
        result.message = "the offscreen present render fence did not signal";
        cleanup();
        return result;
    }

    const std::uint64_t byteCount = static_cast<std::uint64_t>(width) * height * sizeof(Rgba8);
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = byteCount;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo readAllocation{};
    readAllocation.usage = VMA_MEMORY_USAGE_AUTO;
    readAllocation.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer rawBuffer = VK_NULL_HANDLE;
    if (vmaCreateBuffer(control->allocator, &bufferInfo, &readAllocation, &rawBuffer,
                        &readbackAllocation, &readbackInfo) != VK_SUCCESS) {
        result.message = "the offscreen readback buffer could not be created";
        cleanup();
        return result;
    }
    readbackBuffer = rawBuffer;

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = control->computeQueueFamily;
    VkCommandPool rawPool = VK_NULL_HANDLE;
    if (dispatcher->vkCreateCommandPool(*control->device, &poolInfo, nullptr, &rawPool) !=
        VK_SUCCESS) {
        result.message = "the offscreen readback pool could not be created";
        cleanup();
        return result;
    }
    readPool = vk::raii::CommandPool(control->device, rawPool);
    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = rawPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    VkCommandBuffer rawCommand = VK_NULL_HANDLE;
    if (dispatcher->vkAllocateCommandBuffers(*control->device, &allocateInfo, &rawCommand) !=
        VK_SUCCESS) {
        result.message = "the offscreen readback command could not be allocated";
        cleanup();
        return result;
    }
    readCommand = vk::raii::CommandBuffer(control->device, rawCommand, rawPool);
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence rawFence = VK_NULL_HANDLE;
    if (dispatcher->vkCreateFence(*control->device, &fenceInfo, nullptr, &rawFence) != VK_SUCCESS) {
        result.message = "the offscreen readback fence could not be created";
        cleanup();
        return result;
    }
    readFence = vk::raii::Fence(control->device, rawFence);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dispatcher->vkBeginCommandBuffer(*readCommand, &beginInfo) != VK_SUCCESS) {
        result.message = "the offscreen readback command could not begin";
        cleanup();
        return result;
    }
    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = target.image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dispatcher->vkCmdPipelineBarrier(
        *readCommand,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    dispatcher->vkCmdCopyImageToBuffer(
        *readCommand, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuffer, 1, &copy);
    if (dispatcher->vkEndCommandBuffer(*readCommand) != VK_SUCCESS) {
        result.message = "the offscreen readback command could not end";
        cleanup();
        return result;
    }
    VkSubmitInfo readSubmit{};
    readSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    readSubmit.commandBufferCount = 1;
    readSubmit.pCommandBuffers = &rawCommand;
    if (dispatcher->vkQueueSubmit(*control->computeQueue, 1, &readSubmit, rawFence) != VK_SUCCESS) {
        result.message = "the offscreen readback could not be submitted";
        cleanup();
        return result;
    }
    const VkResult readWait = dispatcher->vkWaitForFences(*control->device, 1, &rawFence, VK_TRUE,
                                                          5ULL * 1000ULL * 1000ULL * 1000ULL);
    if (readWait != VK_SUCCESS) {
        result.message = "the offscreen readback fence did not signal";
        cleanup();
        return result;
    }
    if (vmaInvalidateAllocation(control->allocator, readbackAllocation, 0, byteCount) !=
        VK_SUCCESS) {
        result.message = "the offscreen readback buffer could not be invalidated";
        cleanup();
        return result;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(readbackInfo.pMappedData);
    result.pixels.resize(static_cast<std::size_t>(width) * height);
    for (std::size_t index = 0; index < result.pixels.size(); ++index) {
        const std::uint8_t* texel = bytes + index * 4U;
        if (format == VK_FORMAT_B8G8R8A8_UNORM) {
            result.pixels[index] = Rgba8{texel[2], texel[1], texel[0], texel[3]};
        } else {
            result.pixels[index] = Rgba8{texel[0], texel[1], texel[2], texel[3]};
        }
    }
    result.ok = true;
    cleanup();
    return result;
}

[[nodiscard]] inline OffscreenResult
renderOffscreen(const std::shared_ptr<DeviceAllocatorState>& control, const VkFormat format,
                const std::uint32_t width, const std::uint32_t height,
                std::shared_ptr<const GpuDisplayImage> input, const GpuPresentImageParams& params,
                const GpuPresentOverlay& overlay) {
    auto presenter = std::make_shared<PresentImagePipeline>();
    return renderOffscreenWith(control, presenter, format, width, height, std::move(input), params,
                               overlay);
}

[[nodiscard]] inline std::vector<Rgba32f> sentinelPixels() {
    return {*pixel(1.0F, 0.0F, 0.0F, 1.0F), *pixel(0.0F, 1.0F, 0.0F, 1.0F),
            *pixel(0.0F, 0.0F, 1.0F, 1.0F), *pixel(1.0F, 1.0F, 1.0F, 1.0F)};
}

// Premultiplied 2x2 with alpha variation: opaque red, opaque black, opaque blue, and a
// half-alpha white (premultiplied 0.5,0.5,0.5,0.5 -> straight white 1.0, alpha 0.5). The alpha
// variation is what makes a straight-vs-premultiplied filter divergence observable.
[[nodiscard]] inline std::vector<Rgba32f> premultipliedPixels() {
    return {*pixel(1.0F, 0.0F, 0.0F, 1.0F), *pixel(0.0F, 0.0F, 0.0F, 1.0F),
            *pixel(0.0F, 0.0F, 1.0F, 1.0F), *pixel(0.5F, 0.5F, 0.5F, 0.5F)};
}

[[nodiscard]] inline GpuPresentImageParams identityParams(const std::uint32_t width,
                                                          const std::uint32_t height) {
    GpuPresentImageParams params;
    params.targetWidth = width;
    params.targetHeight = height;
    params.destination =
        GpuPresentRect{0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)};
    params.source =
        GpuPresentSourceWindow{0.0, 0.0, static_cast<double>(width), static_cast<double>(height)};
    params.background = GpuPresentBackground::Black;
    params.backgroundColor = GpuPresentColor{0.0F, 0.0F, 0.0F, 1.0F};
    return params;
}

} // namespace bloom::render::present_native_support
