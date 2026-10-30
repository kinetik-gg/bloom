#include "stb_image_adapter.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <zlib.h>

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
struct ByteStream {
    std::span<const std::byte> bytes;
    std::size_t position = 0;
};
int readBytes(void* user, char* data, const int size) {
    auto& stream = *static_cast<ByteStream*>(user);
    if (size <= 0)
        return 0;
    const auto count =
        std::min(static_cast<std::size_t>(size), stream.bytes.size() - stream.position);
    if (count == 0)
        return 0;
    std::memcpy(data, stream.bytes.data() + stream.position, count);
    stream.position += count;
    return static_cast<int>(count);
}
void skipBytes(void* user, const int count) {
    auto& stream = *static_cast<ByteStream*>(user);
    if (count < 0) {
        const auto distance = static_cast<std::uint64_t>(-static_cast<std::int64_t>(count));
        stream.position -= std::min<std::uint64_t>(stream.position, distance);
    } else {
        stream.position +=
            std::min(static_cast<std::size_t>(count), stream.bytes.size() - stream.position);
    }
}
int bytesEof(void* user) {
    const auto& stream = *static_cast<ByteStream*>(user);
    return stream.position == stream.bytes.size() ? 1 : 0;
}
const stbi_io_callbacks kByteCallbacks{readBytes, skipBytes, bytesEof};
} // namespace

namespace bloom::media::detail {
bool imageInfo(const std::filesystem::path& path, ImageInfo& info) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return false;
    if (!stbi_info_from_callbacks(&kFileCallbacks, &stream, &info.width, &info.height,
                                  &info.channels))
        return false;
    stream.clear();
    stream.seekg(0);
    info.sixteenBit = stbi_is_16_bit_from_callbacks(&kFileCallbacks, &stream) != 0;
    return true;
}
bool imageInfo(const std::span<const std::byte> bytes, ImageInfo& info) {
    ByteStream stream{bytes};
    if (!stbi_info_from_callbacks(&kByteCallbacks, &stream, &info.width, &info.height,
                                  &info.channels))
        return false;
    stream.position = 0;
    info.sixteenBit = stbi_is_16_bit_from_callbacks(&kByteCallbacks, &stream) != 0;
    return true;
}
static std::vector<std::uint16_t> decodeSamples(const stbi_io_callbacks& callbacks, void* source,
                                                const ImageInfo& info,
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
    const ImageAllocationBudgetScope budgetScope(pixelBudget);
    int width = 0;
    int height = 0;
    int channels = 0;
    auto* decoded = stbi_load_16_from_callbacks(&callbacks, source, &width, &height, &channels, 4);
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
namespace {
[[nodiscard]] std::uint32_t pngWord(const std::byte* bytes) noexcept {
    return (std::to_integer<std::uint32_t>(bytes[0]) << 24U) |
           (std::to_integer<std::uint32_t>(bytes[1]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[2]) << 8U) |
           std::to_integer<std::uint32_t>(bytes[3]);
}
[[nodiscard]] std::uint8_t paeth(const int a, const int b, const int c) noexcept {
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc)
        return static_cast<std::uint8_t>(a);
    return static_cast<std::uint8_t>(pb <= pc ? b : c);
}
// Reverses the five PNG row filters in place over `raw` (filter byte + row bytes per row).
[[nodiscard]] bool unfilterRows(std::vector<std::uint8_t>& raw, const std::size_t rowBytes,
                                const std::size_t height, const std::size_t pixelBytes) noexcept {
    std::uint8_t* previous = nullptr;
    for (std::size_t row = 0; row < height; ++row) {
        auto* line = raw.data() + row * (rowBytes + 1);
        const auto filter = line[0];
        auto* current = line + 1;
        switch (filter) {
        case 0:
            break;
        case 1:
            for (std::size_t i = pixelBytes; i < rowBytes; ++i)
                current[i] = static_cast<std::uint8_t>(current[i] + current[i - pixelBytes]);
            break;
        case 2:
            if (previous != nullptr)
                for (std::size_t i = 0; i < rowBytes; ++i)
                    current[i] = static_cast<std::uint8_t>(current[i] + previous[i]);
            break;
        case 3:
            for (std::size_t i = 0; i < rowBytes; ++i) {
                const unsigned left = i >= pixelBytes ? current[i - pixelBytes] : 0U;
                const unsigned up = previous != nullptr ? previous[i] : 0U;
                current[i] = static_cast<std::uint8_t>(current[i] + ((left + up) >> 1U));
            }
            break;
        case 4:
            for (std::size_t i = 0; i < rowBytes; ++i) {
                const int left = i >= pixelBytes ? current[i - pixelBytes] : 0;
                const int up = previous != nullptr ? previous[i] : 0;
                const int corner =
                    i >= pixelBytes && previous != nullptr ? previous[i - pixelBytes] : 0;
                current[i] = static_cast<std::uint8_t>(current[i] + paeth(left, up, corner));
            }
            break;
        default:
            return false;
        }
        previous = current;
    }
    return true;
}
} // namespace

// The common PNG subset decoded through zlib's inflate: non-interlaced 8/16-bit grey, grey+alpha,
// RGB and RGBA without tRNS. The samples equal stbi_load_16(..., 4) exactly: big-endian 16-bit
// samples, 8-bit samples widened as v * 257, grey replicated, opaque alpha at the maximum.
// Anything else, or any inconsistency, returns nullopt and the caller keeps the stb decoder.
std::optional<std::vector<std::uint16_t>> fastPngSamples(const std::span<const std::byte> bytes,
                                                         const ImageInfo& info,
                                                         const std::size_t pixelBudget) {
    constexpr std::array<unsigned char, 8> signature{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (bytes.size() < 33 || info.width <= 0 || info.height <= 0)
        return std::nullopt;
    for (std::size_t i = 0; i < signature.size(); ++i)
        if (std::to_integer<unsigned char>(bytes[i]) != signature[i])
            return std::nullopt;
    if (pngWord(bytes.data() + 8) != 13 || std::memcmp(bytes.data() + 12, "IHDR", 4) != 0)
        return std::nullopt;
    const auto width = static_cast<std::size_t>(pngWord(bytes.data() + 16));
    const auto height = static_cast<std::size_t>(pngWord(bytes.data() + 20));
    const auto depth = std::to_integer<unsigned>(bytes[24]);
    const auto colourType = std::to_integer<unsigned>(bytes[25]);
    if (width != static_cast<std::size_t>(info.width) ||
        height != static_cast<std::size_t>(info.height) || (depth != 8 && depth != 16) ||
        (colourType != 0 && colourType != 2 && colourType != 4 && colourType != 6) ||
        std::to_integer<unsigned>(bytes[26]) != 0 || std::to_integer<unsigned>(bytes[27]) != 0 ||
        std::to_integer<unsigned>(bytes[28]) != 0)
        return std::nullopt;
    const std::size_t channels = colourType == 0   ? 1
                                 : colourType == 2 ? 3
                                 : colourType == 4 ? 2
                                                   : 4;
    const std::size_t sampleBytes = depth / 8;
    const std::size_t pixelBytes = channels * sampleBytes;
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    if (width > std::numeric_limits<std::uint32_t>::max() / pixelBytes ||
        pixels > std::numeric_limits<std::uint64_t>::max() / 40U)
        return std::nullopt;
    const std::size_t rowBytes = width * pixelBytes;
    const auto inflatedBytes = static_cast<std::uint64_t>(rowBytes + 1) * height;
    // Peak: final RGBA32F + RGBA16 output + the inflated rows, within the caller's admission.
    if (pixels * 24U + inflatedBytes > pixelBudget)
        return std::nullopt;
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(inflatedBytes));
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK)
        return std::nullopt;
    struct End {
        z_stream& stream;
        ~End() { inflateEnd(&stream); }
    } end{stream};
    stream.next_out = raw.data();
    stream.avail_out = static_cast<uInt>(raw.size());
    if (raw.size() > std::numeric_limits<uInt>::max())
        return std::nullopt;
    bool finished = false;
    for (std::size_t position = 8; position + 12 <= bytes.size() && !finished;) {
        const auto length = static_cast<std::size_t>(pngWord(bytes.data() + position));
        const auto* type = bytes.data() + position + 4;
        if (length > bytes.size() - position - 12)
            return std::nullopt;
        if (std::memcmp(type, "tRNS", 4) == 0 || std::memcmp(type, "CgBI", 4) == 0)
            return std::nullopt;
        if (std::memcmp(type, "IEND", 4) == 0)
            break;
        if (std::memcmp(type, "IDAT", 4) == 0 && length != 0) {
            stream.next_in =
                reinterpret_cast<Bytef*>(const_cast<std::byte*>(bytes.data()) + position + 8);
            stream.avail_in = static_cast<uInt>(length);
            while (stream.avail_in != 0) {
                const auto status = inflate(&stream, Z_NO_FLUSH);
                if (status == Z_STREAM_END) {
                    finished = true;
                    break;
                }
                if (status != Z_OK || stream.avail_out == 0)
                    return std::nullopt;
            }
        }
        position += 12 + length;
    }
    if (!finished || stream.avail_out != 0 || !unfilterRows(raw, rowBytes, height, pixelBytes))
        return std::nullopt;
    std::vector<std::uint16_t> samples(static_cast<std::size_t>(pixels) * 4U);
    for (std::size_t row = 0; row < height; ++row) {
        const auto* line = raw.data() + row * (rowBytes + 1) + 1;
        auto* out = samples.data() + row * width * 4U;
        for (std::size_t x = 0; x < width; ++x) {
            std::array<std::uint16_t, 4> value{0, 0, 0, 0xFFFF};
            for (std::size_t c = 0; c < channels; ++c) {
                const auto* sample = line + (x * channels + c) * sampleBytes;
                value[c] = sampleBytes == 2
                               ? static_cast<std::uint16_t>((sample[0] << 8U) | sample[1])
                               : static_cast<std::uint16_t>(sample[0] * 257U);
            }
            if (channels <= 2) {
                const auto alpha = channels == 2 ? value[1] : std::uint16_t{0xFFFF};
                value = {value[0], value[0], value[0], alpha};
            } else if (channels == 3) {
                value[3] = 0xFFFF;
            }
            std::copy(value.begin(), value.end(), out + x * 4U);
        }
    }
    return samples;
}

std::vector<std::uint16_t> imageSamples(const std::filesystem::path& path, const ImageInfo& info,
                                        const std::size_t pixelBudget) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return {};
    return decodeSamples(kFileCallbacks, &stream, info, pixelBudget);
}
std::vector<std::uint16_t> stbImageSamples(const std::span<const std::byte> bytes,
                                           const ImageInfo& info, const std::size_t pixelBudget) {
    ByteStream stream{bytes};
    return decodeSamples(kByteCallbacks, &stream, info, pixelBudget);
}
std::vector<std::uint16_t> imageSamples(const std::span<const std::byte> bytes,
                                        const ImageInfo& info, const std::size_t pixelBudget) {
    if (auto fast = fastPngSamples(bytes, info, pixelBudget))
        return std::move(*fast);
    return stbImageSamples(bytes, info, pixelBudget);
}
} // namespace bloom::media::detail
