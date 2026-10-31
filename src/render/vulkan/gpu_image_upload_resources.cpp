#include "gpu_image_upload_private.hpp"
#include "gpu_native_image_allocation_private.hpp"

#include <bit>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

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
        half = std::move(other.half);
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
    half.release();
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

UploadHalfImage& UploadHalfImage::operator=(UploadHalfImage&& other) noexcept {
    if (this != &other) {
        release();
        state = std::exchange(other.state, nullptr);
        image = std::exchange(other.image, VK_NULL_HANDLE);
        allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
        token = std::move(other.token);
        width = std::exchange(other.width, 0U);
        height = std::exchange(other.height, 0U);
    }
    return *this;
}

void UploadHalfImage::release() noexcept {
    if (image != VK_NULL_HANDLE && state != nullptr) {
        vmaDestroyImage(state->allocator, image, allocation);
        if (state->nativeImageBackingsDestroyed != UINT64_MAX)
            ++state->nativeImageBackingsDestroyed;
    }
    token = {};
    state = nullptr;
    image = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
    width = 0;
    height = 0;
}

bool uploadHalfStagingSupported(vulkan_detail::DeviceAllocatorState& state) noexcept {
    if (state.physicalDevice == nullptr)
        return false;
    const auto* dispatcher = state.physicalDevice.getDispatcher();
    auto* const physical = static_cast<VkPhysicalDevice>(*state.physicalDevice);
    std::uint32_t familyCount = 0;
    dispatcher->vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    dispatcher->vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
    if (state.computeQueueFamily >= familyCount ||
        (families[state.computeQueueFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U)
        return false;
    VkFormatProperties half{};
    dispatcher->vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R16G16B16A16_SFLOAT, &half);
    VkFormatProperties full{};
    dispatcher->vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R32G32B32A32_SFLOAT, &full);
    const VkFormatFeatureFlags halfNeeds =
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    return (half.optimalTilingFeatures & halfNeeds) == halfNeeds &&
           (full.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0U;
}

bool createUploadHalfImage(vulkan_detail::DeviceAllocatorState& state, const std::uint32_t width,
                           const std::uint32_t height, UploadHalfImage& out) noexcept {
    VkImageCreateInfo imageInfo;
    std::memset(&imageInfo, 0, sizeof(imageInfo));
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    UploadHalfImage pending;
    if (!vulkan_detail::allocateCoveredImage(state, imageInfo, pending.image, pending.allocation,
                                             pending.token))
        return false;
    pending.state = &state;
    pending.width = width;
    pending.height = height;
    out = std::move(pending);
    return true;
}

bool packExactHalf(const float* const source, std::uint16_t* const destination,
                   const std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        const auto bits = std::bit_cast<std::uint32_t>(source[index]);
        const auto sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
        const auto exponent = static_cast<std::int32_t>((bits >> 23U) & 0xFFU);
        const auto mantissa = bits & 0x7FFFFFU;
        std::uint16_t half = 0;
        if (exponent == 0) {
            if (mantissa != 0U)
                return false; // a float subnormal is below every half
            half = sign;
        } else if (exponent == 0xFF) {
            if (mantissa != 0U)
                return false; // NaN payloads are not carried
            half = static_cast<std::uint16_t>(sign | 0x7C00U);
        } else {
            const std::int32_t unbiased = exponent - 127;
            if (unbiased >= -14 && unbiased <= 15) {
                if ((mantissa & 0x1FFFU) != 0U)
                    return false;
                half = static_cast<std::uint16_t>(
                    sign | static_cast<std::uint32_t>((unbiased + 15) << 10) | (mantissa >> 13U));
            } else if (unbiased >= -24 && unbiased < -14) {
                // Half subnormal: value = significand * 2^-24 with significand < 1024.
                const std::uint32_t significand = mantissa | 0x800000U;
                const auto shift = static_cast<std::uint32_t>(13 + (-14 - unbiased));
                if ((significand & ((1U << shift) - 1U)) != 0U)
                    return false;
                half = static_cast<std::uint16_t>(sign | (significand >> shift));
            } else {
                return false;
            }
        }
        destination[index] = half;
    }
    return true;
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
