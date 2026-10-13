#pragma once

#include "gpu_device_private.hpp"

#include <bloom/render/ocio_gpu_program.hpp>

#include <limits>

namespace bloom::render::ocio_program_detail {

// Checked upload layout peak: actual image/staging allocation plus CPU padding backing.
[[nodiscard]] inline bool fitsRemainingBudget(const std::uint64_t imageBytes,
                                              const std::uint64_t stagingBytes,
                                              const std::uint64_t scratchBytes,
                                              const std::uint64_t remaining) noexcept {
    std::uint64_t total = imageBytes;
    if (total > std::numeric_limits<std::uint64_t>::max() - stagingBytes)
        return false;
    total += stagingBytes;
    if (total > std::numeric_limits<std::uint64_t>::max() - scratchBytes)
        return false;
    return total + scratchBytes <= remaining;
}

[[nodiscard]] inline VkFormat channelFormat(const OcioGpuTextureChannel channel,
                                            const std::uint32_t components) noexcept {
    if (components == 4)
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    return channel == OcioGpuTextureChannel::Red ? VK_FORMAT_R32_SFLOAT
                                                 : VK_FORMAT_R32G32B32_SFLOAT;
}

[[nodiscard]] inline std::uint32_t channelComponents(const OcioGpuTextureChannel channel) noexcept {
    return channel == OcioGpuTextureChannel::Red ? 1U : 3U;
}

[[nodiscard]] inline VkImageType imageType(const OcioGpuTextureDimensions dimensions) noexcept {
    switch (dimensions) {
    case OcioGpuTextureDimensions::OneD:
        return VK_IMAGE_TYPE_1D;
    case OcioGpuTextureDimensions::TwoD:
        return VK_IMAGE_TYPE_2D;
    case OcioGpuTextureDimensions::ThreeD:
        return VK_IMAGE_TYPE_3D;
    }
    return VK_IMAGE_TYPE_2D;
}

[[nodiscard]] inline VkImageViewType viewType(const OcioGpuTextureDimensions dimensions) noexcept {
    switch (dimensions) {
    case OcioGpuTextureDimensions::OneD:
        return VK_IMAGE_VIEW_TYPE_1D;
    case OcioGpuTextureDimensions::TwoD:
        return VK_IMAGE_VIEW_TYPE_2D;
    case OcioGpuTextureDimensions::ThreeD:
        return VK_IMAGE_VIEW_TYPE_3D;
    }
    return VK_IMAGE_VIEW_TYPE_2D;
}

[[nodiscard]] inline VkExtent3D textureExtent(const OcioGpuTextureDesc& texture) noexcept {
    switch (texture.dimensions) {
    case OcioGpuTextureDimensions::OneD:
        return {texture.width, 1, 1};
    case OcioGpuTextureDimensions::TwoD:
        return {texture.width, texture.height, 1};
    case OcioGpuTextureDimensions::ThreeD:
        return {texture.edgeLength, texture.edgeLength, texture.edgeLength};
    }
    return {1, 1, 1};
}

} // namespace bloom::render::ocio_program_detail
