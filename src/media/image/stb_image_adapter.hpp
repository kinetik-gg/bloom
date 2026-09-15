#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bloom::media::detail {
struct ImageInfo {
    int width = 0;
    int height = 0;
    bool sixteenBit = false;
};
[[nodiscard]] bool imageInfo(std::span<const std::byte> bytes, ImageInfo& info);
[[nodiscard]] std::vector<std::uint16_t> imageSamples(std::span<const std::byte> bytes,
                                                      const ImageInfo& info);
} // namespace bloom::media::detail
