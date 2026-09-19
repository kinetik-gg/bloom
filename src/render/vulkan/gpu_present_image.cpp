#include "gpu_present_image_private.hpp"

#include "gpu_resident_display_private.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace bloom::render {
namespace present_image_detail {
namespace {

[[nodiscard]] bool isUnormSwapchainFormatImpl(const VkFormat format) noexcept {
    return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_R8G8B8A8_UNORM;
}

[[nodiscard]] bool finitePositive(const float value) noexcept {
    return std::isfinite(value) && value > 0.0F;
}

[[nodiscard]] bool finiteValue(const float value) noexcept { return std::isfinite(value); }

[[nodiscard]] bool finitePositiveDouble(const double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] bool finiteValueDouble(const double value) noexcept { return std::isfinite(value); }

[[nodiscard]] bool finiteColor(const GpuPresentColor& color) noexcept {
    return finiteValue(color.red) && finiteValue(color.green) && finiteValue(color.blue) &&
           finiteValue(color.alpha);
}

// Validates the caller-derived mapping before any presenter state is mutated or a command is
// recorded. The target extent must match the actual acquired attachment (the viewport and the
// shader's targetExtent must agree), the source/destination rectangles must be positive and finite,
// the colors/origins/tile must be finite, and the enums must be in range. A source window that
// extends beyond the display image (a lawful crop/pan) is allowed; the shader clamps its taps.
[[nodiscard]] bool validatePresentParams(const presentation_detail::SwapchainResources& resources,
                                         const GpuPresentImageParams& params,
                                         std::string& message) {
    if (params.targetWidth == 0U || params.targetHeight == 0U ||
        params.targetWidth != resources.width || params.targetHeight != resources.height) {
        message = "the present target extent does not match the acquired attachment";
        return false;
    }
    if (static_cast<std::uint32_t>(params.channel) >
        static_cast<std::uint32_t>(GpuPresentChannel::Alpha)) {
        message = "the present channel mode is out of range";
        return false;
    }
    if (static_cast<std::uint32_t>(params.background) >
        static_cast<std::uint32_t>(GpuPresentBackground::White)) {
        message = "the present background mode is out of range";
        return false;
    }
    if (!finiteValue(params.destination.x) || !finiteValue(params.destination.y) ||
        !finitePositive(params.destination.width) || !finitePositive(params.destination.height)) {
        message = "the present destination rectangle is not finite and positive";
        return false;
    }
    if (!finiteValueDouble(params.source.x) || !finiteValueDouble(params.source.y) ||
        !finitePositiveDouble(params.source.width) || !finitePositiveDouble(params.source.height)) {
        message = "the present source window is not finite and positive";
        return false;
    }
    if (!finiteColor(params.backgroundColor) || !finiteColor(params.checkerColorA) ||
        !finiteColor(params.checkerColorB)) {
        message = "the present colors are not finite";
        return false;
    }
    if (!finitePositive(params.checkerTilePixels) || !finiteValue(params.checkerOriginX) ||
        !finiteValue(params.checkerOriginY)) {
        message = "the present checkerboard parameters are not finite and positive";
        return false;
    }
    return true;
}

[[nodiscard]] PresentImageUniforms makeUniforms(const GpuPresentImageParams& params,
                                                const std::uint32_t sourceWidth,
                                                const std::uint32_t sourceHeight,
                                                const bool hasOverlay) {
    PresentImageUniforms uniforms{};
    uniforms.targetExtent[0] = static_cast<float>(params.targetWidth);
    uniforms.targetExtent[1] = static_cast<float>(params.targetHeight);
    uniforms.destinationRect[0] = params.destination.x;
    uniforms.destinationRect[1] = params.destination.y;
    uniforms.destinationRect[2] = params.destination.width;
    uniforms.destinationRect[3] = params.destination.height;
    uniforms.imageWindow[0] = static_cast<float>(params.source.x);
    uniforms.imageWindow[1] = static_cast<float>(params.source.y);
    uniforms.imageWindow[2] = static_cast<float>(params.source.width);
    uniforms.imageWindow[3] = static_cast<float>(params.source.height);
    uniforms.sourceExtent[0] = static_cast<float>(sourceWidth);
    uniforms.sourceExtent[1] = static_cast<float>(sourceHeight);
    uniforms.backgroundColor[0] = params.backgroundColor.red;
    uniforms.backgroundColor[1] = params.backgroundColor.green;
    uniforms.backgroundColor[2] = params.backgroundColor.blue;
    uniforms.backgroundColor[3] = params.backgroundColor.alpha;
    uniforms.checkerColorA[0] = params.checkerColorA.red;
    uniforms.checkerColorA[1] = params.checkerColorA.green;
    uniforms.checkerColorA[2] = params.checkerColorA.blue;
    uniforms.checkerColorA[3] = params.checkerColorA.alpha;
    uniforms.checkerColorB[0] = params.checkerColorB.red;
    uniforms.checkerColorB[1] = params.checkerColorB.green;
    uniforms.checkerColorB[2] = params.checkerColorB.blue;
    uniforms.checkerColorB[3] = params.checkerColorB.alpha;
    uniforms.checkerParams[0] = params.checkerTilePixels;
    uniforms.checkerParams[1] = 0.0F;
    uniforms.checkerParams[2] = params.checkerOriginX;
    uniforms.checkerParams[3] = params.checkerOriginY;
    uniforms.modes[0] = static_cast<std::uint32_t>(params.channel);
    uniforms.modes[1] = static_cast<std::uint32_t>(params.background);
    uniforms.modes[2] = hasOverlay ? 1U : 0U;
    uniforms.modes[3] = 0U;
    return uniforms;
}
} // namespace

GpuPresentationTargetCode
renderResidentIntoAcquired(presentation_detail::SwapchainResources& resources,
                           const std::shared_ptr<vulkan_detail::DeviceAllocatorState>& control,
                           std::shared_ptr<PresentImagePipeline>& presenter,
                           const std::shared_ptr<const GpuDisplayImage>& input,
                           const GpuPresentImageParams& params, const GpuPresentOverlay& overlay,
                           std::string& message) {
    if (control == nullptr) {
        message = "the device has no live allocator state";
        return GpuPresentationTargetCode::DriverUnavailable;
    }
    if (std::this_thread::get_id() != control->owner) {
        message = "resident presentation may only run on the device owner thread";
        return GpuPresentationTargetCode::WrongThread;
    }
    if (!resources.acquiredIndex.has_value() || resources.commandBuffers.empty()) {
        message = "no image was acquired for the resident present";
        return GpuPresentationTargetCode::NothingAcquired;
    }
    if (!isUnormSwapchainFormatImpl(resources.format)) {
        message = "the swapchain format is not UNORM; refusing the sRGB double-encode path";
        return GpuPresentationTargetCode::PresentationUnavailable;
    }
    // Gate 8: the resident image is written by compute and read by fragment. Sampling it without an
    // explicit queue-ownership transfer requires the compute and present queues to be one family.
    if (control->computeQueueFamily != control->presentQueueFamily) {
        message = "resident display sampling requires the same compute/present queue family; use "
                  "the CPU path until combined selection lands";
        return GpuPresentationTargetCode::PresentationUnavailable;
    }
    if (input == nullptr) {
        message = "the resident display image handle is null";
        return GpuPresentationTargetCode::InvalidArgument;
    }
    const GpuDisplayImageImpl* display = gpuDisplayImageImpl(*input);
    if (display == nullptr || display->image == VK_NULL_HANDLE ||
        display->state.get() != control.get()) {
        message = "the resident display image is unavailable or belongs to another device";
        return GpuPresentationTargetCode::InvalidArgument;
    }
    if (!validatePresentParams(resources, params, message)) {
        return GpuPresentationTargetCode::InvalidArgument;
    }
    if (presenter == nullptr) {
        presenter = std::make_shared<PresentImagePipeline>();
    }
    // The previous frame's render fence is known complete as soon as a new image was acquired: the
    // target refuses acquire while a frame is in flight. A new display view may therefore replace
    // the previous strong pin. The view and its pin are updated together inside ensureDisplayView,
    // so a later preparation failure (e.g. an invalid overlay token) can never leave a cached view
    // without the strong source pin that keeps its VkImage alive.
    const bool previousRenderComplete = !resources.renderInFlight;
    const bool prepared =
        presenter->ensureDeviceResources(control, message) &&
        presenter->ensureSwapchainResources(resources, message) &&
        presenter->ensureDisplayView(*control, *display, input, previousRenderComplete, message) &&
        presenter->ensureOverlay(*control, overlay, message);
    if (!prepared) {
        // A newly requested overlay that could not be prepared must not be silently dropped: report
        // failure so the caller keeps the previous complete frame. The display view/pin committed
        // by ensureDisplayView remains internally consistent (view and pin were set together).
        return GpuPresentationTargetCode::DriverUnavailable;
    }

    const bool hasOverlay = presenter->stagedOverlayPending || presenter->overlayToken != 0U;
    PresentImageUniforms uniforms =
        makeUniforms(params, display->width, display->height, hasOverlay);
    std::memcpy(presenter->paramsMapped, &uniforms, sizeof(uniforms));
    if (vmaFlushAllocation(control->allocator, presenter->paramsAllocation, 0, sizeof(uniforms)) !=
        VK_SUCCESS) {
        message = "the viewer present uniform buffer could not be flushed";
        return GpuPresentationTargetCode::DriverUnavailable;
    }

    // The resident display producer (gpu_resident_display) leaves its output image in
    // SHADER_READ_ONLY_OPTIMAL (its final barrier is TRANSFER_DST -> SHADER_READ_ONLY), so
    // the descriptor and source barrier must agree with that actual layout. Sampling a
    // SHADER_READ_ONLY image as GENERAL would be a layout mismatch.
    const VkDescriptorImageInfo displayInfo{*presenter->sampler, *presenter->displayView,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkImageView overlayView = hasOverlay ? *presenter->overlayView : *presenter->displayView;
    const VkDescriptorImageInfo overlayInfo{*presenter->sampler, overlayView,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo paramsInfo{presenter->paramsBuffer, 0,
                                            sizeof(PresentImageUniforms)};
    VkWriteDescriptorSet writes[3]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = *presenter->descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &displayInfo;
    writes[1] = writes[0];
    writes[1].dstBinding = 1;
    writes[1].pImageInfo = &overlayInfo;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = *presenter->descriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].pBufferInfo = &paramsInfo;
    control->device.getDispatcher()->vkUpdateDescriptorSets(*control->device, 3, writes, 0,
                                                            nullptr);

    const std::uint32_t index = *resources.acquiredIndex;
    if (index >= presenter->framebuffers.size()) {
        message = "the acquired image has no presentation framebuffer";
        return GpuPresentationTargetCode::DriverUnavailable;
    }
    const auto* dispatcher = control->device.getDispatcher();
    const VkCommandBuffer command = *resources.commandBuffers.front();
    dispatcher->vkResetCommandBuffer(command, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (dispatcher->vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
        message = "the resident present command buffer could not be begun";
        return GpuPresentationTargetCode::DriverUnavailable;
    }
    VkImageMemoryBarrier sourceBarrier{};
    sourceBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    sourceBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sourceBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sourceBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceBarrier.image = display->image;
    sourceBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // The resident display writes the packed image from a compute pass AND a transfer copy, so the
    // source scope must cover both TRANSFER_WRITE and SHADER_WRITE.
    sourceBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    sourceBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dispatcher->vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &sourceBarrier);

    if (presenter->stagedOverlayPending) {
        // Upload is recorded in the SAME command buffer as the draw; the staging buffer stays
        // allocated until the next present proves this render fence complete.
        VkImageMemoryBarrier overlayBarrier{};
        overlayBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        overlayBarrier.oldLayout = presenter->overlayImageInitialized
                                       ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                       : VK_IMAGE_LAYOUT_UNDEFINED;
        overlayBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        overlayBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        overlayBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        overlayBarrier.image = presenter->overlayImage;
        overlayBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        overlayBarrier.srcAccessMask =
            presenter->overlayImageInitialized ? VK_ACCESS_SHADER_READ_BIT : 0;
        overlayBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dispatcher->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                         1, &overlayBarrier);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {presenter->overlayWidth, presenter->overlayHeight, 1};
        region.bufferRowLength = presenter->overlayRowBytes / 4U;
        dispatcher->vkCmdCopyBufferToImage(command, presenter->stagedOverlayBuffer,
                                           presenter->overlayImage,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageMemoryBarrier toRead = overlayBarrier;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dispatcher->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                         nullptr, 1, &toRead);
        presenter->overlayImageInitialized = true;
        presenter->overlayToken = presenter->stagedOverlayToken;
        presenter->stagedOverlayPending = false;
    }

    VkRenderPassBeginInfo renderPassBegin{};
    renderPassBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassBegin.renderPass = *presenter->renderPass;
    renderPassBegin.framebuffer = *presenter->framebuffers[index];
    renderPassBegin.renderArea = {{0, 0}, {resources.width, resources.height}};
    dispatcher->vkCmdBeginRenderPass(command, &renderPassBegin, VK_SUBPASS_CONTENTS_INLINE);
    dispatcher->vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, *presenter->pipeline);
    VkViewport viewport{};
    viewport.width = static_cast<float>(resources.width);
    viewport.height = static_cast<float>(resources.height);
    viewport.minDepth = 0.0F;
    viewport.maxDepth = 1.0F;
    dispatcher->vkCmdSetViewport(command, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, {resources.width, resources.height}};
    dispatcher->vkCmdSetScissor(command, 0, 1, &scissor);
    const VkDescriptorSet set = *presenter->descriptorSet;
    dispatcher->vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        *presenter->pipelineLayout, 0, 1, &set, 0, nullptr);
    dispatcher->vkCmdDraw(command, 3, 1, 0, 0);
    dispatcher->vkCmdEndRenderPass(command);
    if (dispatcher->vkEndCommandBuffer(command) != VK_SUCCESS) {
        message = "the resident present command buffer could not be recorded";
        return GpuPresentationTargetCode::DriverUnavailable;
    }
    return GpuPresentationTargetCode::Ok;
}

} // namespace present_image_detail
} // namespace bloom::render
