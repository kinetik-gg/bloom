// The test-only sparse readback of the resident RGBA8 display image, split out of
// gpu_resident_display_resources.cpp so the debug full-frame readback and the shared readback
// quarantine stay cohesive and this bounded sparse copy has its own translation unit. It shares the
// one process quarantine/fuse with the full readback (gpu_resident_display_readback_private.hpp);
// it never creates a second one.

#include "gpu_resident_display_readback_private.hpp"

#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::render {
namespace {

constexpr std::uint64_t kSparseReadbackDeadlineNanoseconds = 5ULL * 1000ULL * 1000ULL * 1000ULL;

[[nodiscard]] GpuDisplayImageSparseReadback
displaySparseReadbackFailure(const GpuDisplayImageReadbackCode code,
                             const std::string_view message) noexcept {
    GpuDisplayImageSparseReadback result;
    result.code = code;
    try {
        result.message.assign(message);
    } catch (...) {
        result.message.clear();
    }
    return result;
}

} // namespace

GpuDisplayImageSparseReadback
readbackResidentDisplayImageSparse(const GpuDisplayImage& image,
                                   const std::span<const ImagePixelCoordinate> coordinates,
                                   const std::uint64_t byteBudget) noexcept {
    using readback_detail::StagingBuffer;
    if (readback_detail::displayFuse().load()) {
        return displaySparseReadbackFailure(GpuDisplayImageReadbackCode::DeviceUnavailable,
                                            "a prior display readback could not be retired");
    }
    try {
        if (image.impl_ == nullptr || image.impl_->image == VK_NULL_HANDLE) {
            return displaySparseReadbackFailure(GpuDisplayImageReadbackCode::DeviceUnavailable,
                                                "no resident display image");
        }
        GpuDisplayImageImpl& impl = *image.impl_;
        if (!impl.onOwnerThread()) {
            return displaySparseReadbackFailure(
                GpuDisplayImageReadbackCode::WrongThread,
                "sparse readback must run on the device owner thread");
        }
        if (impl.deviceLost) {
            return displaySparseReadbackFailure(GpuDisplayImageReadbackCode::DeviceLost,
                                                "the device was lost");
        }
        if (impl.submissionUnretired) {
            return displaySparseReadbackFailure(GpuDisplayImageReadbackCode::DeviceUnavailable,
                                                "a prior display readback is not retired");
        }
        if (coordinates.empty()) {
            return displaySparseReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                                "the sparse readback requested no coordinates");
        }
        const std::uint64_t bytes = coordinates.size() * sizeof(Rgba8);
        if (bytes == 0 || bytes > byteBudget) {
            return displaySparseReadbackFailure(GpuDisplayImageReadbackCode::OverBudget,
                                                "the sparse readback exceeds the byte budget");
        }
        for (const auto& coordinate : coordinates) {
            if (coordinate.x >= impl.width || coordinate.y >= impl.height) {
                return displaySparseReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                                    "a sparse coordinate is outside the image");
            }
        }
        auto& slot = readback_detail::displayQuarantine();
        if (!readback_detail::tryRetireDisplayQuarantine(slot)) {
            return displaySparseReadbackFailure(GpuDisplayImageReadbackCode::DeviceUnavailable,
                                                "a prior display readback is still unretired");
        }
        auto& state = *impl.state;
        const VkDevice device = static_cast<VkDevice>(*state.device);
        const auto* dispatcher = state.device.getDispatcher();

        StagingBuffer staging;
        staging.state = &state;
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo stagingAllocation{};
        stagingAllocation.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAllocation.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo stagingInfo{};
        if (vmaCreateBuffer(state.allocator, &bufferInfo, &stagingAllocation, &staging.buffer,
                            &staging.allocation, &stagingInfo) != VK_SUCCESS) {
            return displaySparseReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                                "the sparse staging buffer could not be created");
        }
        staging.armed = true;

        vk::raii::CommandPool pool{nullptr};
        {
            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            poolInfo.queueFamilyIndex = state.computeQueueFamily;
            VkCommandPool raw = VK_NULL_HANDLE;
            if (dispatcher->vkCreateCommandPool(device, &poolInfo, nullptr, &raw) != VK_SUCCESS) {
                return displaySparseReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                                    "the sparse command pool could not be created");
            }
            pool = vk::raii::CommandPool(state.device, raw);
        }
        vk::raii::CommandBuffer commandBuffer{nullptr};
        {
            VkCommandBufferAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocateInfo.commandPool = *pool;
            allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocateInfo.commandBufferCount = 1;
            VkCommandBuffer raw = VK_NULL_HANDLE;
            if (dispatcher->vkAllocateCommandBuffers(device, &allocateInfo, &raw) != VK_SUCCESS) {
                return displaySparseReadbackFailure(
                    GpuDisplayImageReadbackCode::ReadbackFailed,
                    "the sparse command buffer could not be allocated");
            }
            commandBuffer = vk::raii::CommandBuffer(state.device, raw, *pool);
        }
        vk::raii::Fence fence{nullptr};
        {
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence raw = VK_NULL_HANDLE;
            if (dispatcher->vkCreateFence(device, &fenceInfo, nullptr, &raw) != VK_SUCCESS) {
                return displaySparseReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                                    "the sparse fence could not be created");
            }
            fence = vk::raii::Fence(state.device, raw);
        }

        const VkCommandBuffer rawCommandBuffer = static_cast<VkCommandBuffer>(*commandBuffer);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (dispatcher->vkBeginCommandBuffer(rawCommandBuffer, &beginInfo) != VK_SUCCESS) {
            return displaySparseReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                                "the sparse command buffer could not begin");
        }
        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = impl.image;
        toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                         1, &toTransfer);
        for (std::size_t index = 0; index < coordinates.size(); ++index) {
            VkBufferImageCopy copy{};
            copy.bufferOffset = index * sizeof(Rgba8);
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageOffset = {static_cast<std::int32_t>(coordinates[index].x),
                                static_cast<std::int32_t>(coordinates[index].y), 0};
            copy.imageExtent = {1, 1, 1};
            dispatcher->vkCmdCopyImageToBuffer(rawCommandBuffer, impl.image,
                                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer,
                                               1, &copy);
        }
        VkImageMemoryBarrier toRead = toTransfer;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                         nullptr, 1, &toRead);
        if (dispatcher->vkEndCommandBuffer(rawCommandBuffer) != VK_SUCCESS) {
            return displaySparseReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                                "the sparse command buffer could not end");
        }
        const VkFence rawFence = static_cast<VkFence>(*fence);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &rawCommandBuffer;
        if (dispatcher->vkQueueSubmit(static_cast<VkQueue>(*state.computeQueue), 1, &submit,
                                      rawFence) != VK_SUCCESS) {
            return displaySparseReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                                "the sparse readback submission failed");
        }
        const VkResult waited = dispatcher->vkWaitForFences(device, 1, &rawFence, VK_TRUE,
                                                            kSparseReadbackDeadlineNanoseconds);
        if (waited != VK_SUCCESS && waited != VK_ERROR_DEVICE_LOST) {
            slot.state = impl.state;
            slot.staging = std::move(staging);
            slot.pool = std::move(pool);
            slot.buffer = std::move(commandBuffer);
            slot.fence = std::move(fence);
            slot.occupied = true;
            impl.submissionUnretired = true;
            readback_detail::displayFuse().store(true);
            return displaySparseReadbackFailure(
                GpuDisplayImageReadbackCode::ReadbackFailed,
                "the sparse readback completion is unknown; retained");
        }
        if (waited == VK_ERROR_DEVICE_LOST) {
            impl.deviceLost = true;
            return displaySparseReadbackFailure(GpuDisplayImageReadbackCode::DeviceLost,
                                                "the device was lost during the sparse readback");
        }
        impl.submissionUnretired = false;
        if (vmaInvalidateAllocation(state.allocator, staging.allocation, 0, bytes) != VK_SUCCESS) {
            return displaySparseReadbackFailure(
                GpuDisplayImageReadbackCode::ReadbackFailed,
                "the sparse readback buffer could not be invalidated");
        }
        GpuDisplayImageSparseReadback result;
        result.pixels.resize(coordinates.size());
        const auto* bytesPointer = static_cast<const std::uint8_t*>(stagingInfo.pMappedData);
        for (std::size_t index = 0; index < coordinates.size(); ++index) {
            const std::uint8_t* texel = bytesPointer + index * sizeof(Rgba8);
            result.pixels[index] = Rgba8{texel[0], texel[1], texel[2], texel[3]};
        }
        return result;
    } catch (const std::bad_alloc&) {
        return displaySparseReadbackFailure(
            GpuDisplayImageReadbackCode::ReadbackFailed,
            "the sparse readback host buffer could not be allocated");
    } catch (...) {
        return displaySparseReadbackFailure(GpuDisplayImageReadbackCode::ReadbackFailed,
                                            "the sparse readback failed unexpectedly");
    }
}

} // namespace bloom::render
