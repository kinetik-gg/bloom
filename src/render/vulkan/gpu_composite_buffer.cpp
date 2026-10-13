#include "gpu_composite_private.hpp"
#include "gpu_native_buffer_allocation_private.hpp"

#include <cstring>
#include <limits>
#include <utility>

namespace bloom::render {
using vulkan_detail::DeviceAllocatorState;
std::uint64_t compositeBufferAllocationBytes(const CompositeBuffer& buffer) noexcept {
    if (!buffer.armed || buffer.state == nullptr || buffer.allocation == VK_NULL_HANDLE) {
        return 0;
    }
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(buffer.state->allocator, buffer.allocation, &info);
    return info.size;
}

std::uint64_t compositeImageAllocationBytes(const GpuImageImpl& image) noexcept {
    if (image.state == nullptr || image.allocation == VK_NULL_HANDLE) {
        return 0;
    }
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(image.state->allocator, image.allocation, &info);
    return info.size;
}

CompositeBuffer::~CompositeBuffer() { release(); }

void CompositeBuffer::releaseFailedAttempt() noexcept {
    const auto phase = state ? state->processingPhase : nullptr;
    const auto owner = state;
    const bool hadBacking = armed && storageToken.bytes() != 0;
    const bool hadStaging = stagingBuffer != VK_NULL_HANDLE && stagingStorageToken.bytes() != 0;
    auto backing = std::move(storageToken);
    auto staging = std::move(stagingStorageToken);
    release();
    if (owner) {
        if (hadBacking && owner->nativeBufferBackingsDestroyed != UINT64_MAX)
            ++owner->nativeBufferBackingsDestroyed;
        if (hadStaging && owner->nativeBufferBackingsDestroyed != UINT64_MAX)
            ++owner->nativeBufferBackingsDestroyed;
    }
    static_cast<void>(backing.releaseFailedAttemptAfterBackingDestroyed(phase));
    static_cast<void>(staging.releaseFailedAttemptAfterBackingDestroyed(phase));
}

CompositeBuffer& CompositeBuffer::operator=(CompositeBuffer&& other) noexcept {
    if (this != &other) {
        release();
        state = std::move(other.state);
        storageToken = std::move(other.storageToken);
        stagingStorageToken = std::move(other.stagingStorageToken);
        buffer = other.buffer;
        allocation = other.allocation;
        stagingBuffer = std::exchange(other.stagingBuffer, VK_NULL_HANDLE);
        stagingAllocation = std::exchange(other.stagingAllocation, VK_NULL_HANDLE);
        stagingBytes = std::exchange(other.stagingBytes, 0);
        bytes = other.bytes;
        hostVisible = other.hostVisible;
        armed = other.armed;
        other.state = nullptr;
        other.buffer = VK_NULL_HANDLE;
        other.allocation = VK_NULL_HANDLE;
        other.bytes = 0;
        other.hostVisible = false;
        other.armed = false;
    }
    return *this;
}

void CompositeBuffer::releaseStaging() noexcept {
    if (stagingBuffer != VK_NULL_HANDLE && state != nullptr) {
        vmaDestroyBuffer(state->allocator, stagingBuffer, stagingAllocation);
        if (stagingStorageToken.bytes() != 0 && state->nativeBufferBackingsDestroyed != UINT64_MAX)
            ++state->nativeBufferBackingsDestroyed;
        composite_detail::metadataStagingLiveBytes.fetch_sub(stagingBytes);
        composite_detail::metadataStagingReclaims.fetch_add(1);
    }
    stagingBuffer = VK_NULL_HANDLE;
    stagingAllocation = VK_NULL_HANDLE;
    stagingBytes = 0;
    stagingStorageToken = {};
}

void CompositeBuffer::recordInitialization(const VkCommandBuffer commandBuffer) const noexcept {
    if (stagingBuffer == VK_NULL_HANDLE) {
        return;
    }
    const auto* dispatcher = state->device.getDispatcher();
    VkBufferCopy copy{};
    copy.size = bytes;
    dispatcher->vkCmdCopyBuffer(commandBuffer, stagingBuffer, buffer, 1, &copy);
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.size = bytes;
    dispatcher->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                                     &barrier, 0, nullptr);
}

void CompositeBuffer::release() noexcept {
    releaseStaging();
    if (armed && state != nullptr) {
        vmaDestroyBuffer(state->allocator, buffer, allocation);
        if (storageToken.bytes() != 0 && state->nativeBufferBackingsDestroyed != UINT64_MAX)
            ++state->nativeBufferBackingsDestroyed;
    }
    storageToken = {};
    state = nullptr;
    buffer = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
    bytes = 0;
    hostVisible = false;
    armed = false;
}

bool createCompositeBuffer(const std::shared_ptr<DeviceAllocatorState>& owner,
                           const std::uint64_t bytes, const bool hostVisible,
                           const void* const initialData, CompositeBuffer& out,
                           const bool restoreFailedPrefix) noexcept {
    if (owner == nullptr || bytes == 0 || owner->allocator == VK_NULL_HANDLE) {
        return false;
    }
    DeviceAllocatorState& state = *owner;
    // Metadata and status buffers are tiny and bounded by the caller's budget; refuse anything that
    // could not be addressed in one binding.
    if (bytes > state.maxStorageBufferRange) {
        return false;
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (hostVisible || initialData != nullptr) {
        bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationCreate{};
    allocationCreate.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostVisible) {
        allocationCreate.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    CompositeBuffer created;
    struct Cleanup final {
        CompositeBuffer& created;
        bool restore;
        ~Cleanup() {
            if (restore)
                created.releaseFailedAttempt();
        }
    } cleanup{created, restoreFailedPrefix};
    created.state = owner;
    created.bytes = bytes;
    created.hostVisible = hostVisible;
    VmaAllocationInfo allocationInfo{};
    if (vulkan_detail::allocateCoveredBuffer(
            state, bufferInfo, allocationCreate, std::numeric_limits<std::uint64_t>::max(),
            created.buffer, created.allocation, allocationInfo, created.storageToken,
            vulkan_detail::NativeBufferPurpose::Processing,
            restoreFailedPrefix) != vulkan_detail::NativeBufferAllocationOutcome::Allocated) {
        return false;
    }
    created.armed = true;

    if (hostVisible && initialData != nullptr && allocationInfo.pMappedData == nullptr) {
        return false;
    }
    bool directMapped = false;
    if (hostVisible && initialData != nullptr && allocationInfo.pMappedData != nullptr) {
        std::memcpy(allocationInfo.pMappedData, initialData, static_cast<std::size_t>(bytes));
        if (composite_detail::mappedMemoryVisibility(false, state.allocator, created.allocation,
                                                     bytes) != VK_SUCCESS) {
            return false;
        }
        directMapped = true;
    }
    if (!directMapped && initialData != nullptr) {
        // Retained staging is recorded into the owning operation command buffer.
        VkBufferCreateInfo stagingInfo{};
        stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingInfo.size = bytes;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo stagingAllocation{};
        stagingAllocation.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAllocation.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VmaAllocation stagingHandle = VK_NULL_HANDLE;
        VmaAllocationInfo stagingMapped{};
        NativeBufferStorageToken stagingToken;
        if (vulkan_detail::allocateCoveredBuffer(
                state, stagingInfo, stagingAllocation, std::numeric_limits<std::uint64_t>::max(),
                stagingBuffer, stagingHandle, stagingMapped, stagingToken,
                vulkan_detail::NativeBufferPurpose::Processing,
                restoreFailedPrefix) != vulkan_detail::NativeBufferAllocationOutcome::Allocated) {
            return false;
        }
        created.stagingBuffer = stagingBuffer;
        created.stagingAllocation = stagingHandle;
        created.stagingBytes = stagingMapped.size;
        created.stagingStorageToken = std::move(stagingToken);
        composite_detail::metadataStagingLiveBytes.fetch_add(created.stagingBytes);
        if (stagingMapped.pMappedData == nullptr) {
            return false;
        }
        std::memcpy(stagingMapped.pMappedData, initialData, static_cast<std::size_t>(bytes));
        if (composite_detail::mappedMemoryVisibility(false, state.allocator, stagingHandle,
                                                     bytes) != VK_SUCCESS) {
            return false;
        }
    }

    out = std::move(created);
    return true;
}

} // namespace bloom::render
