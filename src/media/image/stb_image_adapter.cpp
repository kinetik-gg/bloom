#include "stb_image_adapter.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>

namespace {
// Thread-local so concurrent decodes with distinct request budgets cannot interfere. A default
// only covers header inspection; imageSamples() installs the caller's peak budget for its parse.
thread_local std::size_t allocationLimit = 268435456;
thread_local std::size_t allocatedBytes = 0;
struct alignas(std::max_align_t) Allocation {
    std::size_t size;
};
[[nodiscard]] bool exceedsLimit(const std::size_t size, const std::size_t liveBytes) {
    return allocationLimit <= liveBytes || size > allocationLimit - liveBytes;
}
[[nodiscard]] bool allocationSizeFits(const std::size_t size) {
    return size <= std::numeric_limits<std::size_t>::max() - sizeof(Allocation);
}
void* imageAllocate(std::size_t size) {
    if (!allocationSizeFits(size) || exceedsLimit(size, allocatedBytes))
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
    if (!allocationSizeFits(size) || exceedsLimit(size, allocatedBytes - block->size))
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

namespace bloom::media::detail {
ImageAllocationBudgetScope::ImageAllocationBudgetScope(const std::size_t limitBytes) noexcept
    : previous_(allocationLimit) {
    allocationLimit = limitBytes;
}
ImageAllocationBudgetScope::~ImageAllocationBudgetScope() noexcept { allocationLimit = previous_; }
} // namespace bloom::media::detail

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
// The upstream per-dimension ceiling, not a total-pixel cap: the codec's own integer overflow
// guards (and this adapter's explicit peak budget) bound the decoded samples.
#define STBI_MAX_DIMENSIONS (1 << 24)
#define STBI_MALLOC(size) imageAllocate(size)
#define STBI_REALLOC(pointer, size) imageReallocate(pointer, size)
#define STBI_FREE(pointer) imageFree(pointer)
#include "stb_image.h"

namespace {
int readStream(void* user, char* data, int size) {
    auto* stream = static_cast<std::ifstream*>(user);
    stream->read(data, size);
    return static_cast<int>(stream->gcount());
}
void skipStream(void* user, int count) {
    static_cast<std::ifstream*>(user)->seekg(count, std::ios::cur);
}
int streamEof(void* user) { return static_cast<std::ifstream*>(user)->eof() ? 1 : 0; }
const stbi_io_callbacks kFileCallbacks{readStream, skipStream, streamEof};
} // namespace

namespace bloom::media::detail {
bool imageInfo(const std::filesystem::path& path, ImageInfo& info) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return false;
    int channels = 0;
    if (!stbi_info_from_callbacks(&kFileCallbacks, &stream, &info.width, &info.height, &channels))
        return false;
    stream.clear();
    stream.seekg(0);
    info.sixteenBit = stbi_is_16_bit_from_callbacks(&kFileCallbacks, &stream) != 0;
    return true;
}
std::vector<std::uint16_t> imageSamples(const std::filesystem::path& path, const ImageInfo& info,
                                        const std::size_t pixelBudget) {
    if (info.width <= 0 || info.height <= 0)
        return {};
    const auto pixels =
        static_cast<std::uint64_t>(info.width) * static_cast<std::uint64_t>(info.height);
    // Peak admission: final RGBA32F (16 bytes/px) plus this parse's RGBA16 staging (8 bytes/px)
    // must fit the caller's explicit budget, so the parser is never asked to over-allocate.
    constexpr std::uint64_t kPeakBytesPerPixel = 24U;
    if (pixels > std::numeric_limits<std::uint64_t>::max() / kPeakBytesPerPixel ||
        pixels * kPeakBytesPerPixel > pixelBudget)
        return {};
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return {};
    const ImageAllocationBudgetScope budgetScope(pixelBudget);
    int width = 0;
    int height = 0;
    int channels = 0;
    auto* decoded =
        stbi_load_16_from_callbacks(&kFileCallbacks, &stream, &width, &height, &channels, 4);
    if (!decoded)
        return {};
    if (width != info.width || height != info.height) {
        stbi_image_free(decoded);
        return {};
    }
    try {
        std::vector<std::uint16_t> result(decoded,
                                          decoded + static_cast<std::size_t>(width) * height * 4);
        stbi_image_free(decoded);
        return result;
    } catch (...) {
        stbi_image_free(decoded);
        throw;
    }
}
} // namespace bloom::media::detail
