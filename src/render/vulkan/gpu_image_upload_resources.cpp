#include "gpu_image_upload_private.hpp"

#include <cstdint>
#include <utility>

namespace bloom::render {

UploadStagingBuffer& UploadStagingBuffer::operator=(UploadStagingBuffer&& other) noexcept {
    if (this != &other) {
        release();
        state = other.state;
        buffer = other.buffer;
        allocation = other.allocation;
        bytes = other.bytes;
        armed = other.armed;
        token = std::move(other.token);
        other.state = nullptr;
        other.buffer = VK_NULL_HANDLE;
        other.allocation = VK_NULL_HANDLE;
        other.bytes = 0;
        other.armed = false;
    }
    return *this;
}

UploadStagingBuffer::~UploadStagingBuffer() { release(); }

void UploadStagingBuffer::release() noexcept {
    if (armed && state != nullptr) {
        vmaDestroyBuffer(state->allocator, buffer, allocation);
        if (token.bytes() != 0 && state->nativeBufferBackingsDestroyed != UINT64_MAX)
            ++state->nativeBufferBackingsDestroyed;
    }
    token = {};
    state = nullptr;
    buffer = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
    bytes = 0;
    armed = false;
}

std::uint64_t uploadAllocationBytes(vulkan_detail::DeviceAllocatorState& state,
                                    const VmaAllocation allocation) noexcept {
    if (allocation == VK_NULL_HANDLE) {
        return 0;
    }
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(state.allocator, allocation, &info);
    return static_cast<std::uint64_t>(info.size);
}

vulkan_detail::NativeBufferAllocationOutcome
createUploadStagingBuffer(vulkan_detail::DeviceAllocatorState& state, const std::uint64_t bytes,
                          const std::uint64_t maximumBackingBytes,
                          UploadStagingBuffer& out) noexcept {
    if (bytes == 0) {
        return vulkan_detail::NativeBufferAllocationOutcome::Failed;
    }
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    UploadStagingBuffer pending;
    VmaAllocationInfo actual{};
    const auto outcome = vulkan_detail::allocateCoveredBuffer(
        state, bufferInfo, allocationInfo, maximumBackingBytes, pending.buffer, pending.allocation,
        actual, pending.token, vulkan_detail::NativeBufferPurpose::Upload);
    if (outcome != vulkan_detail::NativeBufferAllocationOutcome::Allocated)
        return outcome;
    pending.state = &state;
    pending.bytes = actual.size;
    pending.armed = true;
    out = std::move(pending);
    return outcome;
}

} // namespace bloom::render
