// ADR 0026: multipart and layered OpenEXR probe, colour-layer selection and channel-set decode.
#include "generated_jpeg.hpp"
#include "png_test_support.hpp"

#include <Imath/half.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfMultiPartOutputFile.h>
#include <ImfOutputPart.h>
#include <ImfPartType.h>
#include <ImfStringAttribute.h>
#include <ImfTileDescription.h>
#include <ImfTiledOutputPart.h>
#include <bloom/media/image.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace bloom_output_png_test_support;
namespace media = bloom::media;

constexpr int kWidth = 3;
constexpr int kHeight = 2;
constexpr std::size_t kSamples = static_cast<std::size_t>(kWidth) * kHeight;
constexpr std::uint32_t kIdBits = 0xdeadbeefU;
constexpr std::uint32_t kCryptoHash = 0x3f2a1b0cU;

struct Plane final {
    std::string name;
    Imf::PixelType type;
    std::vector<float> values;
    std::vector<Imath::half> halves;
    std::vector<std::uint32_t> integers;
};

[[nodiscard]] Plane plane(std::string name, const Imf::PixelType type, const float base) {
    Plane result{std::move(name), type, std::vector<float>(kSamples), {}, {}};
    for (std::size_t index = 0; index < kSamples; ++index)
        result.values[index] = base + static_cast<float>(index) * 0.125F;
    if (type == Imf::HALF)
        for (const auto value : result.values)
            result.halves.emplace_back(value);
    if (type == Imf::UINT)
        result.integers.assign(kSamples, kIdBits);
    return result;
}

void insert(Imf::FrameBuffer& frameBuffer, Plane& source, const Imath::Box2i& data) {
    char* base = source.type == Imf::HALF   ? reinterpret_cast<char*>(source.halves.data())
                 : source.type == Imf::UINT ? reinterpret_cast<char*>(source.integers.data())
                                            : reinterpret_cast<char*>(source.values.data());
    const auto element = source.type == Imf::HALF ? sizeof(Imath::half) : sizeof(float);
    frameBuffer.insert(source.name,
                       Imf::Slice::Make(source.type, base, data.min, kWidth, kHeight, element,
                                        element * kWidth));
}

// Part "beauty": root RGBA, a diffuse layer, depth with negative/NaN data, a uint ID and a
// Cryptomatte rank pair. Part "data" (tiled): depth only.
void writeLayered(const std::filesystem::path& path) {
    const Imath::Box2i display(Imath::V2i(0, 0), Imath::V2i(3, 3));
    const Imath::Box2i data(Imath::V2i(1, 1), Imath::V2i(3, 2));
    std::vector<Plane> beauty;
    beauty.push_back(plane("R", Imf::HALF, 0.25F));
    beauty.push_back(plane("G", Imf::HALF, 0.125F));
    beauty.push_back(plane("B", Imf::HALF, 0.0625F));
    beauty.push_back(plane("A", Imf::HALF, 1.0F));
    std::ranges::fill(beauty.back().halves, Imath::half(1.0F));
    beauty.push_back(plane("diffuse.R", Imf::FLOAT, 2.0F));
    beauty.push_back(plane("diffuse.G", Imf::FLOAT, 3.0F));
    beauty.push_back(plane("diffuse.B", Imf::FLOAT, 4.0F));
    auto depth = plane("depth.Z", Imf::FLOAT, -5.0F);
    depth.values[1] = std::numeric_limits<float>::quiet_NaN();
    beauty.push_back(std::move(depth));
    beauty.push_back(plane("id", Imf::UINT, 0.0F));
    auto cryptoId = plane("CryptoObject00.r", Imf::FLOAT, 0.0F);
    std::ranges::fill(cryptoId.values, std::bit_cast<float>(kCryptoHash));
    beauty.push_back(std::move(cryptoId));
    beauty.push_back(plane("CryptoObject00.g", Imf::FLOAT, 0.5F));

    Imf::Header first(display, data);
    first.setName("beauty");
    first.setType(Imf::SCANLINEIMAGE);
    first.insert("cryptomatte/abc1234/name", Imf::StringAttribute("CryptoObject"));
    first.insert("cryptomatte/abc1234/manifest", Imf::StringAttribute("{\"cube\":\"3f2a1b0c\"}"));
    for (const auto& item : beauty)
        first.channels().insert(item.name, Imf::Channel(item.type));
    Imf::Header second(display, data);
    second.setName("data");
    second.setType(Imf::TILEDIMAGE);
    second.setTileDescription(Imf::TileDescription(2, 2, Imf::ONE_LEVEL));
    auto dataDepth = plane("depth.Z", Imf::FLOAT, 10.0F);
    second.channels().insert(dataDepth.name, Imf::Channel(Imf::FLOAT));

    const std::vector<Imf::Header> headers{first, second};
    Imf::MultiPartOutputFile file(path.string().c_str(), headers.data(),
                                  static_cast<int>(headers.size()));
    {
        Imf::OutputPart part(file, 0);
        Imf::FrameBuffer frameBuffer;
        for (auto& item : beauty)
            insert(frameBuffer, item, data);
        part.setFrameBuffer(frameBuffer);
        part.writePixels(kHeight);
    }
    {
        Imf::TiledOutputPart part(file, 1);
        Imf::FrameBuffer frameBuffer;
        insert(frameBuffer, dataDepth, data);
        part.setFrameBuffer(frameBuffer);
        part.writeTiles(0, part.numXTiles() - 1, 0, part.numYTiles() - 1);
    }
}

// One RGBA8 pixel (128, 64, 32, 128), framed by the independent test chunk encoder.
void writeRgbaPng(const std::filesystem::path& path) {
    {
        std::ofstream file(path, std::ios::binary);
        const std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
        file.write(reinterpret_cast<const char*>(signature.data()), 8);
    }
    PngChunkBytes chunks(path);
    chunks.insertChunk(chunks.endOffset(), "IHDR", {0, 0, 0, 1, 0, 0, 0, 1, 8, 6, 0, 0, 0});
    const std::vector<unsigned char> raw{0, 128, 64, 32, 128};
    auto size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> compressed(size);
    if (compress2(compressed.data(), &size, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
        std::abort();
    compressed.resize(size);
    chunks.insertChunk(chunks.endOffset(), "IDAT", compressed);
    chunks.insertChunk(chunks.endOffset(), "IEND", {});
    chunks.save();
}

[[nodiscard]] media::ImageInterpretation select(std::string part, std::string layer) {
    media::ImageInterpretation interpretation;
    interpretation.colorSpace = media::ImageColorSpace::Linear;
    interpretation.part = std::move(part);
    interpretation.layer = std::move(layer);
    return interpretation;
}

[[nodiscard]] bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::ranges::find(values, value) != values.end();
}

} // namespace

int main() {
    ScratchDirectory scratch("media-exr-channels");
    Expectations check;
    const auto path = scratch.file("layered.exr");
    writeLayered(path);

    const auto probe = media::probeImage(path);
    check.expect(probe.value.has_value(), "layered multipart EXR probes");
    if (!probe.value)
        return 1;
    check.expect(probe.value->parts.size() == 2, "probe enumerates both parts");
    if (probe.value->parts.size() == 2) {
        const auto& beauty = probe.value->parts[0];
        const auto& data = probe.value->parts[1];
        check.expect(beauty.name == "beauty" && !beauty.tiled && data.name == "data" &&
                         data.tiled,
                     "part names and types");
        check.expect(beauty.channels.size() == 11 && beauty.dataX == 1 && beauty.dataY == 1 &&
                         beauty.dataWidth == kWidth && beauty.displayWidth == 4,
                     "part channels and windows");
        check.expect(contains(beauty.layers, "") && contains(beauty.layers, "diffuse") &&
                         contains(beauty.layers, "depth") &&
                         contains(beauty.layers, "CryptoObject00"),
                     "part layers");
        check.expect(beauty.colourLayers == std::vector<std::string>{"", "diffuse"},
                     "colour layers are root and diffuse only");
        check.expect(data.colourLayers.empty(), "data part has no colour layer");
        const auto id = std::ranges::find_if(beauty.channels, [](const auto& channel) {
            return channel.name == "id";
        });
        check.expect(id != beauty.channels.end() &&
                         id->sampleKind == media::ImageChannelSampleKind::Uint,
                     "uint channels are probed as uint");
    }
    check.expect(probe.value->width == kWidth && probe.value->bitDepth == 16,
                 "scalar probe describes the first part's root layer");
    check.expect(probe.value->findPart("data") == &probe.value->parts[1] &&
                     probe.value->findPart("") == &probe.value->parts[0] &&
                     probe.value->findPart("missing") == nullptr,
                 "parts are found by name");

    const auto revision = media::captureImageSourceRevision(path, media::kMaxImageStorageBytes);
    check.expect(revision.value.has_value(), "layered EXR revision captures");
    if (!revision.value)
        return 1;
    const auto& source = **revision.value;

    const auto root = media::decodeImage(source, select({}, {}));
    check.expect(root.value.has_value(), "default selection decodes the root colour layer");
    if (root.value) {
        const auto pixel = (*root.value)->read(1, 1);
        check.expect(pixel && pixel.value()->red() == 0.25F && pixel.value()->alpha() == 1.0F,
                     "root colour values");
    }
    const auto diffuse = media::decodeImage(source, select({}, "diffuse"));
    check.expect(diffuse.value.has_value(), "a named colour layer decodes");
    if (diffuse.value) {
        const auto pixel = (*diffuse.value)->read(2, 1);
        check.expect(pixel && pixel.value()->red() == 2.125F && pixel.value()->green() == 3.125F &&
                         pixel.value()->blue() == 4.125F && pixel.value()->alpha() == 1.0F,
                     "a layer without alpha is opaque");
    }
    const auto dataOnly =
        media::decodeImage(source, select("data", "depth"));
    check.expect(!dataOnly && dataOnly.code == media::ImageDiagnosticCode::NoColourChannels,
                 "a data-only layer has no colour image");
    const auto missingPart = media::decodeImage(source, select("missing", {}));
    check.expect(!missingPart && missingPart.code == media::ImageDiagnosticCode::PartNotFound,
                 "a missing part is typed");
    const auto missingLayer = media::decodeImage(source, select({}, "specular"));
    check.expect(!missingLayer && missingLayer.code == media::ImageDiagnosticCode::LayerNotFound,
                 "a missing layer is typed");

    const auto channels = media::decodeImageChannels(source, {});
    check.expect(channels.value.has_value(), "the first part decodes as a channel set");
    if (channels.value) {
        const auto& set = **channels.value;
        check.expect(set.channels().size() == 11, "every channel is decoded");
        check.expect(set.dataWindow().originX() == 1 && set.displayWindow().extent().width() == 4,
                     "channel set windows");
        const auto depth = set.find("depth.Z");
        check.expect(depth && set.plane(*depth)[0] == -5.0F && std::isnan(set.plane(*depth)[1]),
                     "data channels keep negatives and NaN");
        check.expect(depth && set.channels()[*depth].role == bloom::render::ChannelRole::Depth,
                     "depth role");
        const auto id = set.find("id");
        check.expect(id && std::bit_cast<std::uint32_t>(set.plane(*id)[5]) == kIdBits &&
                         set.channels()[*id].sampleKind == bloom::render::ChannelSampleKind::Uint,
                     "uint samples keep their bit pattern");
        const auto crypto = set.find("CryptoObject00.r");
        check.expect(crypto && std::bit_cast<std::uint32_t>(set.plane(*crypto)[3]) == kCryptoHash &&
                         set.channels()[*crypto].role == bloom::render::ChannelRole::Id,
                     "Cryptomatte ID hashes are bit-exact");
        const auto red = set.find("R");
        check.expect(red && set.plane(*red)[0] == 0.25F &&
                         set.channels()[*red].encoding == bloom::render::ChannelValueEncoding::Colour,
                     "colour channels keep source values");
        check.expect(std::ranges::any_of(set.metadata(),
                                         [](const auto& entry) {
                                             return entry.first == "cryptomatte/abc1234/manifest" &&
                                                    entry.second == "{\"cube\":\"3f2a1b0c\"}";
                                         }),
                     "part string metadata travels with the channel set");
    }
    const auto selected =
        media::decodeImageChannels(source, {.part = "beauty", .channels = {"id", "depth.Z"}});
    check.expect(selected.value && (*selected.value)->channels().size() == 2 &&
                     (*selected.value)->channels()[0].name == "id",
                 "a selection decodes only the named channels in order");
    const auto tiled = media::decodeImageChannels(source, {.part = "data"});
    check.expect(tiled.value && (*tiled.value)->plane(0)[kSamples - 1] == 10.625F,
                 "a tiled part decodes as a channel set");
    const auto missingChannel =
        media::decodeImageChannels(source, {.channels = {"specular.R"}});
    check.expect(!missingChannel &&
                     missingChannel.code == media::ImageDiagnosticCode::ChannelNotFound,
                 "a missing channel is typed");
    const auto tooSmall =
        media::decodeImageChannels(source, {}, {}, {}, source.retainedBytes() + 64U);
    check.expect(!tooSmall && tooSmall.code == media::ImageDiagnosticCode::PixelBudgetExceeded,
                 "channel-set admission charges every plane");
    bool stop = false;
    const auto cancelled = media::decodeImageChannels(
        source, {}, [&] { return stop; }, [&](std::uint64_t, std::uint64_t) { stop = true; });
    check.expect(!cancelled && cancelled.cancelled, "channel-set decode is cancellable");

    const auto jpegPath = scratch.file("still.jpg");
    {
        std::ofstream file(jpegPath, std::ios::binary);
        file.write(reinterpret_cast<const char*>(kGeneratedJpeg.data()),
                   static_cast<std::streamsize>(kGeneratedJpeg.size()));
    }
    const auto jpeg = media::captureImageSourceRevision(jpegPath, media::kMaxImageStorageBytes);
    check.expect(jpeg.value.has_value(), "JPEG revision captures");
    if (jpeg.value) {
        const auto still = media::decodeImageChannels(**jpeg.value, {});
        check.expect(still.value.has_value(), "a JPEG still provides a channel set");
        if (still.value) {
            const auto& set = **still.value;
            check.expect(set.channels().size() == 3 && set.channels()[0].name == "R" &&
                             set.channels()[0].role == bloom::render::ChannelRole::ColorR &&
                             set.channels()[2].role == bloom::render::ChannelRole::ColorB &&
                             set.channels()[1].encoding ==
                                 bloom::render::ChannelValueEncoding::Colour,
                         "JPEG channels are R, G, B colour channels without alpha");
            check.expect(set.colourSpace() == (*jpeg.value)->probe().colorSpaceTag,
                         "an uninterpreted still records its declared colour space");
            check.expect(std::ranges::all_of(set.plane(0),
                                             [](const float value) {
                                                 return value >= 0.0F && value <= 1.0F;
                                             }),
                         "JPEG channel values are normalized source samples");
        }
        auto interpreted = media::ImageChannelSelection{};
        interpreted.inputColorSpaceId = "sRGB - Texture";
        const auto labelled = media::decodeImageChannels(**jpeg.value, interpreted);
        check.expect(labelled.value && (*labelled.value)->colourSpace() == "sRGB - Texture",
                     "the asset's input colour space is recorded on the channel set");
        const auto alphaMissing =
            media::decodeImageChannels(**jpeg.value, {.channels = {"A"}});
        check.expect(!alphaMissing &&
                         alphaMissing.code == media::ImageDiagnosticCode::ChannelNotFound,
                     "a JPEG has no alpha channel");
        const auto partMissing = media::decodeImageChannels(**jpeg.value, {.part = "beauty"});
        check.expect(!partMissing && partMissing.code == media::ImageDiagnosticCode::PartNotFound,
                     "a still has no named parts");
    }
    const auto pngPath = scratch.file("rgba.png");
    writeRgbaPng(pngPath);
    const auto png = media::captureImageSourceRevision(pngPath, media::kMaxImageStorageBytes);
    check.expect(png.value.has_value(), "PNG revision captures");
    if (png.value) {
        const auto still = media::decodeImageChannels(**png.value, {});
        check.expect(still.value && (*still.value)->channels().size() == 4 &&
                         (*still.value)->channels()[3].role == bloom::render::ChannelRole::Alpha,
                     "an RGBA PNG provides R, G, B and A");
        if (still.value) {
            const auto& set = **still.value;
            check.expect(std::abs(set.plane(0)[0] - 128.0F / 255.0F) < 1.0e-6F &&
                             std::abs(set.plane(1)[0] - 64.0F / 255.0F) < 1.0e-6F &&
                             std::abs(set.plane(2)[0] - 32.0F / 255.0F) < 1.0e-6F &&
                             std::abs(set.plane(3)[0] - 128.0F / 255.0F) < 1.0e-6F,
                         "PNG channels are straight, unconverted source samples");
        }
    }
    return check.failures() == 0 ? 0 : 1;
}
