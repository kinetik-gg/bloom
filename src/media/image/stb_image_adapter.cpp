#include "stb_image_adapter.hpp"

#include <cstdlib>
#include <cstring>
#include <limits>

namespace {
constexpr std::size_t allocationLimit = 268435456;
thread_local std::size_t allocatedBytes = 0;
struct alignas(std::max_align_t) Allocation {
    std::size_t size;
};
void* imageAllocate(std::size_t size) {
    if (size > allocationLimit - allocatedBytes)
        return nullptr;
    auto* block = static_cast<Allocation*>(std::malloc(sizeof(Allocation) + size));
    if (!block)
        return nullptr;
    block->size = size;
    allocatedBytes += size;
    return block + 1;
}
void imageFree(void* pointer) {
    if (!pointer)
        return;
    auto* block = static_cast<Allocation*>(pointer) - 1;
    allocatedBytes -= block->size;
    std::free(block);
}
void* imageReallocate(void* pointer, std::size_t size) {
    if (!pointer)
        return imageAllocate(size);
    auto* block = static_cast<Allocation*>(pointer) - 1;
    if (size > allocationLimit - (allocatedBytes - block->size))
        return nullptr;
    const auto oldSize = block->size;
    auto* replacement = static_cast<Allocation*>(std::realloc(block, sizeof(Allocation) + size));
    if (!replacement)
        return nullptr;
    replacement->size = size;
    allocatedBytes = allocatedBytes - oldSize + size;
    return replacement + 1;
}
} // namespace

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_MAX_DIMENSIONS 16384
#define STBI_MALLOC(size) imageAllocate(size)
#define STBI_REALLOC(pointer, size) imageReallocate(pointer, size)
#define STBI_FREE(pointer) imageFree(pointer)
#include "../third_party/stb_image/stb_image.h"

namespace bloom::media::detail {
bool imageInfo(std::span<const std::byte> bytes, ImageInfo& info) {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return false;
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const auto size = static_cast<int>(bytes.size());
    int channels = 0;
    if (!stbi_info_from_memory(data, size, &info.width, &info.height, &channels))
        return false;
    info.sixteenBit = stbi_is_16_bit_from_memory(data, size) != 0;
    return true;
}
std::vector<std::uint16_t> imageSamples(std::span<const std::byte> bytes, const ImageInfo& info) {
    if (bytes.size() > 67108864 || info.width <= 0 || info.height <= 0 || info.width > 16384 ||
        info.height > 16384 || static_cast<std::uint64_t>(info.width) * info.height > 16777216)
        return {};
    int width = 0, height = 0, channels = 0;
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    auto* pixels = stbi_load_16_from_memory(data, static_cast<int>(bytes.size()), &width, &height,
                                            &channels, 4);
    if (!pixels)
        return {};
    if (width != info.width || height != info.height) {
        stbi_image_free(pixels);
        return {};
    }
    try {
        std::vector<std::uint16_t> result(pixels,
                                          pixels + static_cast<std::size_t>(width) * height * 4);
        stbi_image_free(pixels);
        return result;
    } catch (...) {
        stbi_image_free(pixels);
        throw;
    }
}
} // namespace bloom::media::detail
