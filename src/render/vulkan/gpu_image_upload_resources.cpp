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
    }
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

bool createUploadStagingBuffer(vulkan_detail::DeviceAllocatorState& state,
                               const std::uint64_t bytes, UploadStagingBuffer& out) noexcept {
    if (bytes == 0) {
        return false;
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

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    if (vmaCreateBuffer(state.allocator, &bufferInfo, &allocationInfo, &buffer, &allocation,
                        nullptr) != VK_SUCCESS) {
        return false;
    }
    out.state = &state;
    out.buffer = buffer;
    out.allocation = allocation;
    out.bytes = bytes;
    out.armed = true;
    return true;
}

} // namespace bloom::render
