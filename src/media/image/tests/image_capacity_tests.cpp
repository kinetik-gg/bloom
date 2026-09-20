#include "image_budget.hpp"
#include "png_test_support.hpp"
#include <Imath/half.h>
#include <ImfChannelList.h>
#include <ImfCompression.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>
#include <ImfTileDescription.h>
#include <ImfTiledOutputFile.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bloom/media/image.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <zlib.h>

namespace {
using namespace bloom_output_png_test_support;
namespace media = bloom::media;

constexpr std::size_t kMiB = 1024ULL * 1024ULL;
constexpr std::size_t k512MiB = 512ULL * kMiB;
constexpr std::size_t k1GiB = 1024ULL * kMiB;

[[nodiscard]] float sampleValue(const std::uint64_t x, const std::uint64_t y) {
    return 0.1F + static_cast<float>((x + 2U * y) % 13U) / 20.0F;
}

void writeExr(const std::filesystem::path& path, const std::uint64_t width,
              const std::uint64_t height, const Imf::Compression compression,
              const int tileSize = 0) {
    Imf::Header header(static_cast<int>(width), static_cast<int>(height));
    header.compression() = compression;
    header.channels().insert("R", Imf::Channel(Imf::HALF));
    header.channels().insert("G", Imf::Channel(Imf::HALF));
    header.channels().insert("B", Imf::Channel(Imf::HALF));
    if (tileSize > 0)
        header.setTileDescription(Imf::TileDescription(
            static_cast<unsigned>(tileSize), static_cast<unsigned>(tileSize), Imf::ONE_LEVEL));
    const auto sampleCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<Imath::half> red(sampleCount);
    std::vector<Imath::half> green(sampleCount);
    std::vector<Imath::half> blue(sampleCount);
    for (std::uint64_t y = 0; y < height; ++y) {
        for (std::uint64_t x = 0; x < width; ++x) {
            const auto index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(x);
            const auto value = sampleValue(x, y);
            red[index] = value;
            green[index] = 0.5F * value;
            blue[index] = 0.25F * value;
        }
    }
    const Imath::Box2i window(
        Imath::V2i(0, 0), Imath::V2i(static_cast<int>(width) - 1, static_cast<int>(height) - 1));
    const auto rowStride = static_cast<std::size_t>(width) * sizeof(Imath::half);
    Imf::FrameBuffer buffer;
    buffer.insert("R",
                  Imf::Slice::Make(Imf::HALF, red.data(), window, sizeof(Imath::half), rowStride));
    buffer.insert(
        "G", Imf::Slice::Make(Imf::HALF, green.data(), window, sizeof(Imath::half), rowStride));
    buffer.insert("B",
                  Imf::Slice::Make(Imf::HALF, blue.data(), window, sizeof(Imath::half), rowStride));
    if (tileSize > 0) {
        Imf::TiledOutputFile output(path.string().c_str(), header, 1);
        output.setFrameBuffer(buffer);
        output.writeTiles(0, output.numXTiles() - 1, 0, output.numYTiles() - 1, 0);
    } else {
        Imf::OutputFile output(path.string().c_str(), header, 1);
        output.setFrameBuffer(buffer);
        output.writePixels(static_cast<int>(height));
    }
}

void appendBigEndian32(std::vector<unsigned char>& bytes, const std::uint32_t value) {
    bytes.push_back(static_cast<unsigned char>(value >> 24U));
    bytes.push_back(static_cast<unsigned char>(value >> 16U));
    bytes.push_back(static_cast<unsigned char>(value >> 8U));
    bytes.push_back(static_cast<unsigned char>(value));
}

// A real 8-bit RGB PNG (filter 0) written with zlib, used to exercise the streaming PNG path.
void writePng(const std::filesystem::path& path, const std::uint32_t width,
              const std::uint32_t height) {
    {
        std::ofstream file(path, std::ios::binary);
        const std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
        file.write(reinterpret_cast<const char*>(signature.data()), 8);
    }
    PngChunkBytes chunks(path);
    std::vector<unsigned char> ihdr;
    appendBigEndian32(ihdr, width);
    appendBigEndian32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunks.insertChunk(chunks.endOffset(), "IHDR", ihdr);
    const auto rowBytes = static_cast<std::size_t>(width) * 3U;
    std::vector<unsigned char> raw(static_cast<std::size_t>(height) * (1U + rowBytes));
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto rowStart = static_cast<std::size_t>(y) * (1U + rowBytes);
        raw[rowStart] = 0;
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto value = static_cast<unsigned char>(40U + ((x + 3U * y) % 200U));
            const auto offset = rowStart + 1U + static_cast<std::size_t>(x) * 3U;
            raw[offset] = value;
            raw[offset + 1U] = value;
            raw[offset + 2U] = value;
        }
    }
    auto bound = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> compressed(static_cast<std::size_t>(bound));
    if (compress2(compressed.data(), &bound, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
        std::abort();
    compressed.resize(static_cast<std::size_t>(bound));
    chunks.insertChunk(chunks.endOffset(), "IDAT", compressed);
    chunks.insertChunk(chunks.endOffset(), "IEND", {});
    chunks.save();
}

// Overwrite a box2i header attribute in an existing EXR to hand the bounded probe a bad window.
void patchExrWindow(const std::filesystem::path& path, const std::string& attribute,
                    const std::array<int, 4>& values) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file)
        return;
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
    const std::string marker = attribute + '\0' + "box2i" + '\0';
    const auto found = std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end());
    if (found == bytes.end())
        return;
    auto offset = static_cast<std::size_t>(found - bytes.begin()) + marker.size() + 4U;
    if (offset + 16U > bytes.size())
        return;
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto value = static_cast<unsigned int>(values[index]);
        for (unsigned byte = 0; byte < 4U; ++byte)
            bytes[offset + index * 4U + byte] = static_cast<char>((value >> (8U * byte)) & 0xFFU);
    }
    file.clear();
    file.seekp(0);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] bool near(const float a, const float b, const float tolerance = 0.01F) {
    return std::abs(a - b) <= tolerance;
}
} // namespace

int main() {
    ScratchDirectory scratch("media-image-capacity");
    Expectations check;

    // A real 6000x4000 (24 Mpixel) EXR above the old 256 MiB storage / 16.7 Mpixel ceilings.
    const auto largeExr = scratch.file("large-6000x4000.exr");
    writeExr(largeExr, 6000, 4000, Imf::ZIP_COMPRESSION);
    const auto largeProbe = media::probeImage(largeExr);
    check.expect(largeProbe.value.has_value() && largeProbe.value->width == 6000 &&
                     largeProbe.value->height == 4000 && largeProbe.value->bitDepth == 16,
                 "6000x4000 EXR probes above the old pixel/storage caps");
    if (largeProbe.value.has_value()) {
        const auto capped = media::decodeImage(largeExr, {}, {}, {}, {}, 300ULL * kMiB);
        check.expect(!capped && capped.code == media::ImageDiagnosticCode::PixelBudgetExceeded,
                     "6000x4000 EXR is refused by a constrained budget before allocation");
        const auto decoded =
            media::decodeImage(largeExr, {}, {}, {}, {}, k512MiB, largeProbe.value->contentDigest);
        check.expect(decoded.value.has_value(),
                     "6000x4000 EXR decodes under an explicit above-256-MiB budget");
        if (decoded.value.has_value()) {
            const auto extent = (*decoded.value)->descriptor()->dataWindow().extent();
            const auto pixel = (*decoded.value)->read(0, 0);
            check.expect(extent.width() == 6000 && extent.height() == 4000 && pixel &&
                             near(pixel.value()->red(), 0.1F) &&
                             near(pixel.value()->green(), 0.05F),
                         "6000x4000 EXR decode preserves geometry and pixels");
        }
        const auto recovered = media::decodeImage(largeExr, {}, {}, {}, {}, k512MiB);
        check.expect(recovered.value.has_value(),
                     "valid recovery after the constrained-budget refusal");
    }

    // A real uncompressed EXR larger than the old 64 MiB whole-file cap: no compressed workaround.
    const auto bigFileExr = scratch.file("large-6000x4000-none.exr");
    writeExr(bigFileExr, 6000, 4000, Imf::NO_COMPRESSION);
    check.expect(std::filesystem::file_size(bigFileExr) > 64ULL * kMiB,
                 "the recovery fixture really exceeds the old 64 MiB file cap");
    const auto bigFileProbe = media::probeImage(bigFileExr);
    check.expect(bigFileProbe.value.has_value() && bigFileProbe.value->width == 6000 &&
                     bigFileProbe.value->height == 4000,
                 "a >64 MiB EXR probes without a whole-file cap");
    if (bigFileProbe.value.has_value()) {
        const auto constrained = media::decodeImage(bigFileExr, {}, {}, {}, {}, 300ULL * kMiB);
        check.expect(!constrained &&
                         constrained.code == media::ImageDiagnosticCode::PixelBudgetExceeded,
                     "the >64 MiB EXR is refused by a constrained budget before allocation");
        const auto decoded = media::decodeImage(bigFileExr, {}, {}, {}, {}, k512MiB,
                                                bigFileProbe.value->contentDigest);
        check.expect(decoded.value.has_value(), "the >64 MiB EXR decodes under an adequate budget");
    }

    // A tiled EXR with several tile-row chunks exercises tile-aligned scanline decoding.
    const auto tiledExr = scratch.file("tiled-ramp.exr");
    writeExr(tiledExr, 40, 20, Imf::ZIP_COMPRESSION, 4);
    const auto tiledDecoded = media::decodeImage(tiledExr, {}, {}, {}, {}, 4ULL * kMiB);
    check.expect(tiledDecoded.value.has_value(), "a multi-tile-row EXR decodes");
    if (tiledDecoded.value.has_value()) {
        const auto pixel = (*tiledDecoded.value)->read(5, 9);
        check.expect(pixel && near(pixel.value()->red(), sampleValue(5, 9)) &&
                         near(pixel.value()->green(), 0.5F * sampleValue(5, 9)),
                     "tile-aligned chunk decoding preserves pixels across chunk boundaries");
    }

    // Extents above the old 16384-per-axis cap now probe and decode when the budget admits them.
    const auto wideExr = scratch.file("wide-20000.exr");
    writeExr(wideExr, 20000, 2, Imf::ZIP_COMPRESSION);
    const auto wideDecoded = media::decodeImage(wideExr, {}, {}, {}, {}, 4ULL * kMiB);
    check.expect(wideDecoded.value.has_value() &&
                     (*wideDecoded.value)->descriptor()->dataWindow().extent().width() == 20000,
                 "a 20000-wide EXR is no longer rejected by the old dimension cap");

    {
        const auto path = scratch.file("overflow-window.exr");
        writeExr(path, 3, 2, Imf::ZIP_COMPRESSION);
        patchExrWindow(path, "displayWindow",
                       {std::numeric_limits<int>::lowest(), std::numeric_limits<int>::lowest(),
                        std::numeric_limits<int>::max(), std::numeric_limits<int>::max()});
        const auto result = media::probeImage(path);
        check.expect(!result && (result.code == media::ImageDiagnosticCode::DimensionsExceeded ||
                                 result.code == media::ImageDiagnosticCode::InvalidDataWindow ||
                                 result.code == media::ImageDiagnosticCode::InvalidHeader),
                     "an overflowing display window is refused with a typed diagnostic");
    }
    {
        const auto path = scratch.file("inverted-window.exr");
        writeExr(path, 3, 2, Imf::ZIP_COMPRESSION);
        patchExrWindow(path, "dataWindow", {10, 10, 1, 1});
        const auto result = media::probeImage(path);
        check.expect(!result && (result.code == media::ImageDiagnosticCode::InvalidDataWindow ||
                                 result.code == media::ImageDiagnosticCode::InvalidHeader ||
                                 result.code == media::ImageDiagnosticCode::Truncated),
                     "an inverted data window is refused with a typed diagnostic");
    }

    // A 6000x4000 (24 Mpixel) PNG above the old 16.7 Mpixel ceiling, under the same explicit
    // budget.
    const auto largePng = scratch.file("large-6000x4000.png");
    writePng(largePng, 6000, 4000);
    const auto largePngProbe = media::probeImage(largePng);
    check.expect(largePngProbe.value.has_value() && largePngProbe.value->width == 6000 &&
                     largePngProbe.value->height == 4000 &&
                     largePngProbe.value->format == media::ImageFormat::Png,
                 "6000x4000 PNG probes above the old 16.7 Mpixel cap");
    const media::ImageInterpretation raw{.colorSpace = media::ImageColorSpace::Raw,
                                         .inputColorSpaceId = {},
                                         .alphaAssociation = media::ImageAlphaAssociation::Auto};
    {
        // Peak = final RGBA32F (384 MB) + RGBA16 staging (192 MB) = 576 MB, so a 500 MB budget must
        // refuse before the parser allocates.
        const auto constrained = media::decodeImage(largePng, raw, {}, {}, {}, 500ULL * kMiB);
        check.expect(!constrained &&
                         constrained.code == media::ImageDiagnosticCode::PixelBudgetExceeded,
                     "a >16 Mpixel PNG is refused when the peak working set exceeds the budget");
        const auto decoded = media::decodeImage(largePng, raw, {}, {}, {}, k1GiB);
        check.expect(decoded.value.has_value() &&
                         (*decoded.value)->descriptor()->dataWindow().extent().width() == 6000,
                     "a >16 Mpixel PNG decodes under the same explicit budget");
        if (decoded.value.has_value()) {
            const auto pixel = (*decoded.value)->read(0, 0);
            check.expect(pixel && near(pixel.value()->red(), 40.0F / 255.0F),
                         "the >16 Mpixel PNG preserves decoded pixels");
        }
        const auto recovered = media::decodeImage(largePng, raw, {}, {}, {}, k1GiB);
        check.expect(recovered.value.has_value(),
                     "valid PNG recovery after the constrained-budget refusal");
    }

    // A 20000-wide PNG exercises the raised stb per-dimension ceiling.
    const auto widePng = scratch.file("wide-20000.png");
    writePng(widePng, 20000, 2);
    const auto widePngDecoded = media::decodeImage(widePng, raw, {}, {}, {}, 2ULL * kMiB);
    check.expect(widePngDecoded.value.has_value() &&
                     (*widePngDecoded.value)->descriptor()->dataWindow().extent().width() == 20000,
                 "a 20000-wide PNG is no longer rejected by the old dimension cap");

    // Pure admission arithmetic at values where naive addition/multiplication would wrap.
    {
        using bloom::media::detail::checkedSizeProduct;
        using bloom::media::detail::decodeWorkingSetFits;
        constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
        constexpr std::size_t kMaxSize = std::numeric_limits<std::size_t>::max();
        check.expect(decodeWorkingSetFits(0, 0, 0, 0), "an empty working set fits a zero budget");
        check.expect(decodeWorkingSetFits(kMax, 0, 0, kMaxSize),
                     "a final image exactly at the budget is admitted");
        check.expect(!decodeWorkingSetFits(kMax, 1, 0, kMaxSize),
                     "final plus staging that would wrap is refused");
        check.expect(!decodeWorkingSetFits(kMax - 1, 2, 0, kMaxSize),
                     "a two-term wrapping sum is refused");
        check.expect(!decodeWorkingSetFits(0, kMax, kMax, kMaxSize),
                     "staging plus row beyond the budget is refused");
        check.expect(decodeWorkingSetFits(0, kMax - 1, 1, kMaxSize),
                     "scratch exactly at the budget is admitted");
        std::uint64_t product = 0;
        check.expect(!checkedSizeProduct(kMax, 2, product), "a wrapping product is refused");
        check.expect(checkedSizeProduct(1ULL << 40, 1ULL << 20, product) && product == (1ULL << 60),
                     "a representable product is returned");
    }

    // A header claiming a billion-by-billion PNG is refused before any giant allocation.
    {
        const auto path = scratch.file("huge-ihdr.png");
        writePng(path, 4, 4);
        PngChunkBytes chunks(path);
        const std::array<std::uint32_t, 2> dimensions{1000000000U, 1000000000U};
        for (std::size_t field = 0; field < dimensions.size(); ++field) {
            for (std::size_t byte = 0; byte < 4; ++byte) {
                const auto shift = 8U * (3U - static_cast<unsigned>(byte));
                chunks.patchDataByteWithValidCrc(
                    "IHDR", 0, field * 4U + byte,
                    static_cast<unsigned char>((dimensions[field] >> shift) & 0xFFU));
            }
        }
        chunks.save();
        const auto decoded = media::decodeImage(path, raw, {}, {}, {}, 256ULL * kMiB);
        check.expect(!decoded && decoded.code != media::ImageDiagnosticCode::None,
                     "a hostile billion-by-billion PNG header is refused without allocating");
    }

    // Concurrent decodes with distinct budgets must not interfere (the parser ceiling is
    // per-thread).
    const auto smallPng = scratch.file("budget-scope.png");
    writePng(smallPng, 8, 8);
    std::atomic<int> opened{0};
    std::atomic<int> refused{0};
    const auto worker = [&](const std::size_t budget, std::atomic<int>& counter, const bool want) {
        for (int iteration = 0; iteration < 32; ++iteration) {
            const auto attempt = media::decodeImage(smallPng, raw, {}, {}, {}, budget);
            if (static_cast<bool>(attempt.value) == want)
                ++counter;
        }
    };
    std::thread tight(worker, 1ULL, std::ref(refused), false);
    std::thread open(worker, k512MiB, std::ref(opened), true);
    tight.join();
    open.join();
    check.expect(opened.load() == 32,
                 "concurrent open-budget PNG decode is unaffected by a tight-budget thread");
    check.expect(refused.load() == 32,
                 "concurrent tight-budget PNG decode is unaffected by an open-budget thread");

    return check.failures() == 0 ? 0 : 1;
}
