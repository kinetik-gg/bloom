#include "generated_jpeg.hpp"
#include "png_test_support.hpp"
#include <Imath/half.h>
#include <ImfChannelList.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>
#include <ImfStringAttribute.h>
#include <ImfTileDescription.h>
#include <ImfTiledOutputFile.h>
#include <algorithm>
#include <bloom/media/image.hpp>
#include <cmath>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace bloom_output_png_test_support;
void png(const std::filesystem::path& path, const bool sixteen = false) {
    // The existing independent chunk encoder supplies CRCs and framing. zlib supplies only
    // compression, and the production image decoder shares neither implementation.
    {
        std::ofstream file(path, std::ios::binary);
        const std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
        file.write(reinterpret_cast<const char*>(signature.data()), 8);
    }
    PngChunkBytes chunks(path);
    const std::vector<unsigned char> header{
        0, 0, 0, 1, 0, 0, 0, 1, static_cast<unsigned char>(sixteen ? 16 : 8), 6, 0, 0, 0};
    chunks.insertChunk(chunks.endOffset(), "IHDR", header);
    const std::vector<unsigned char> raw =
        sixteen ? std::vector<unsigned char>{0, 128, 1, 64, 2, 32, 3, 128, 0}
                : std::vector<unsigned char>{0, 128, 64, 32, 128};
    auto size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> compressed(size);
    if (compress2(compressed.data(), &size, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
        std::abort();
    compressed.resize(size);
    chunks.insertChunk(chunks.endOffset(), "IDAT", compressed);
    chunks.insertChunk(chunks.endOffset(), "IEND", {});
    chunks.save();
}

void exr(const std::filesystem::path& path, const bool tiled, const Imf::PixelType type,
         const bool luminance = false) {
    constexpr int width = 3;
    constexpr int height = 2;
    const Imath::Box2i display(Imath::V2i(-2, 1), Imath::V2i(2, 4));
    const Imath::Box2i data(Imath::V2i(-1, 2), Imath::V2i(1, 3));
    Imf::Header header(display, data);
    if (tiled)
        header.setTileDescription(Imf::TileDescription(2, 2, Imf::ONE_LEVEL));
    header.insert("chromaticities", Imf::ChromaticitiesAttribute(Imf::Chromaticities()));
    header.insert("alphaAssociation", Imf::StringAttribute("premultiplied"));
    if (luminance) {
        header.channels().insert("Y", Imf::Channel(type));
    } else {
        header.channels().insert("R", Imf::Channel(type));
        header.channels().insert("G", Imf::Channel(type));
        header.channels().insert("B", Imf::Channel(type));
    }
    header.channels().insert("A", Imf::Channel(type));

    constexpr auto sampleCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<float> red(sampleCount);
    std::vector<float> green(sampleCount);
    std::vector<float> blue(sampleCount);
    std::vector<float> alpha(sampleCount);
    std::vector<Imath::half> redHalf(sampleCount);
    std::vector<Imath::half> greenHalf(sampleCount);
    std::vector<Imath::half> blueHalf(sampleCount);
    std::vector<Imath::half> alphaHalf(sampleCount);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(x);
            red[index] = 0.25F + static_cast<float>(x) * 0.05F;
            green[index] = 0.125F + static_cast<float>(y) * 0.05F;
            blue[index] = 0.0625F;
            alpha[index] = 0.5F;
            redHalf[index] = red[index];
            greenHalf[index] = green[index];
            blueHalf[index] = blue[index];
            alphaHalf[index] = alpha[index];
        }
    }
    Imf::FrameBuffer buffer;
    const auto insert = [&](const char* name, const std::vector<float>& samples,
                            const std::vector<Imath::half>& halfSamples) {
        if (type == Imf::HALF) {
            buffer.insert(name, Imf::Slice::Make(type, halfSamples.data(), data,
                                                 sizeof(Imath::half), sizeof(Imath::half) * width));
        } else {
            buffer.insert(name, Imf::Slice::Make(type, samples.data(), data, sizeof(float),
                                                 sizeof(float) * width));
        }
    };
    if (luminance)
        insert("Y", red, redHalf);
    else {
        insert("R", red, redHalf);
        insert("G", green, greenHalf);
        insert("B", blue, blueHalf);
    }
    insert("A", alpha, alphaHalf);
    if (tiled) {
        Imf::TiledOutputFile output(path.string().c_str(), header, 1);
        output.setFrameBuffer(buffer);
        output.writeTiles(0, output.numXTiles() - 1, 0, output.numYTiles() - 1, 0);
    } else {
        Imf::OutputFile output(path.string().c_str(), header, 1);
        output.setFrameBuffer(buffer);
        output.writePixels(height);
    }
}

void patchExrVersionFlag(const std::filesystem::path& path, const std::uint32_t flag) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(4);
    std::array<unsigned char, 4> bytes{};
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    auto version = static_cast<std::uint32_t>(bytes[0]) |
                   (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                   (static_cast<std::uint32_t>(bytes[2]) << 16U) |
                   (static_cast<std::uint32_t>(bytes[3]) << 24U);
    version |= flag;
    bytes = {static_cast<unsigned char>(version), static_cast<unsigned char>(version >> 8U),
             static_cast<unsigned char>(version >> 16U),
             static_cast<unsigned char>(version >> 24U)};
    file.clear();
    file.seekp(4);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}
} // namespace
int main() {
    using namespace bloom_output_png_test_support;
    namespace media = bloom::media;
    ScratchDirectory scratch("media-image");
    Expectations check;
    for (bool sixteen : {false, true}) {
        const auto path = scratch.file(sixteen ? "image16.png" : "image.png");
        png(path, sixteen);
        const auto probe = media::probeImage(path);
        check.expect(probe.value.has_value() && probe.value->width == 1 &&
                         probe.value->bitDepth == (sixteen ? 16 : 8),
                     "PNG dimensions and precision");
        const auto result = media::decodeImage(path);
        check.expect(result.value.has_value(), "PNG decode succeeds");
        if (result.value.has_value()) {
            const auto pixel = (*result.value)->read(0, 0);
            check.expect(pixel && std::abs(pixel.value()->red() - 0.108F) < 0.002F,
                         "inverse sRGB then premultiply");
        }
        const auto raw = media::decodeImage(path, {media::ImageColorSpace::Raw});
        check.expect(raw.value.has_value() &&
                         std::abs((*raw.value)->pixels()[0].red() - 0.252F) < 0.002F,
                     "Raw preserves encoded RGB then premultiplies");
        check.expect(!media::decodeImage(path, {}, {}, {}, {}, 1),
                     "pixel budget rejects before decode");
        check.expect(media::decodeImage(path, {}, {}, [] { return true; }).cancelled,
                     "cancel before read");
        PngChunkBytes broken(path);
        broken.truncateTo(30);
        broken.save();
        check.expect(!media::decodeImage(path), "truncated PNG rejected");
        png(path, sixteen);
        PngChunkBytes huge(path);
        huge.patchDataByteWithValidCrc("IHDR", 0, 0, 127);
        huge.save();
        check.expect(!media::probeImage(path) && !media::decodeImage(path),
                     "huge dimensions rejected");
    }
    const auto jpeg = scratch.file("tiny.jpg");
    {
        std::ofstream file(jpeg, std::ios::binary);
        file.write(reinterpret_cast<const char*>(kGeneratedJpeg.data()),
                   static_cast<std::streamsize>(kGeneratedJpeg.size()));
    }
    const auto jpg = media::decodeImage(jpeg);
    check.expect(jpg.value.has_value() &&
                     (*jpg.value)->descriptor()->dataWindow().extent().width() == 2,
                 "generated JPEG decodes");
    {
        const auto tiff = scratch.file("provider-missing.tiff");
        std::ofstream file(tiff, std::ios::binary);
        file.write("II*\0", 4);
        file.close();
        const auto probe = media::probeImage(tiff);
        const auto decoded = media::decodeImage(tiff);
        check.expect(!probe && probe.code == media::ImageDiagnosticCode::ProviderMissing &&
                         !decoded && decoded.code == media::ImageDiagnosticCode::ProviderMissing,
                     "TIFF magic reaches the provider seam and refuses without a provider");
    }
    for (const auto fixture :
         {std::pair{"scanline-half.exr", false}, std::pair{"tiled-float.exr", true}}) {
        const auto path = scratch.file(fixture.first);
        exr(path, fixture.second, fixture.second ? Imf::FLOAT : Imf::HALF);
        const auto probe = media::probeImage(path);
        check.expect(probe.value.has_value() && probe.value->format == media::ImageFormat::Exr &&
                         probe.value->width == 3 && probe.value->height == 2 &&
                         probe.value->bitDepth == (fixture.second ? 32 : 16) &&
                         probe.value->colorSpace == media::ImageColorSpace::Linear &&
                         probe.value->alphaAssociation ==
                             media::ImageAlphaAssociation::Premultiplied,
                     "EXR probe reports bounded dimensions, precision, and interpretation");
        if (probe.value.has_value()) {
            const auto decoded = media::decodeImage(
                path, {}, {}, {}, {}, media::kMaxImageStorageBytes, probe.value->contentDigest);
            check.expect(decoded.value.has_value(), "EXR decode accepts its pinned content digest");
            if (decoded.value.has_value()) {
                const auto pixel = (*decoded.value)->read(-1, 2);
                check.expect(pixel && std::abs(pixel.value()->red() - 0.25F) < 0.01F &&
                                 std::abs(pixel.value()->alpha() - 0.5F) < 0.01F,
                             "EXR premultiplied samples are not multiplied twice");
                check.expect((*decoded.value)->descriptor()->displayWindow().originX() == -2 &&
                                 (*decoded.value)->descriptor()->displayWindow().originY() == 1,
                             "EXR data and display windows retain their origins");
            }
            auto wrongBytes = bloom::core::Sha256Digest::Bytes{};
            std::copy(probe.value->contentDigest.bytes().begin(),
                      probe.value->contentDigest.bytes().end(), wrongBytes.begin());
            wrongBytes[0] ^= 1U;
            const auto wrongDigest = bloom::core::Sha256Digest::fromBytes(wrongBytes);
            const auto rejected =
                media::decodeImage(path, {}, {}, {}, {}, media::kMaxImageStorageBytes, wrongDigest);
            check.expect(!rejected && rejected.code == media::ImageDiagnosticCode::DigestMismatch,
                         "EXR digest mismatch is typed and clean");
        }
    }
    {
        const auto path = scratch.file("luminance.exr");
        exr(path, false, Imf::FLOAT, true);
        const auto decoded = media::decodeImage(path);
        check.expect(decoded.value.has_value(), "EXR Y plus alpha decodes");
        if (decoded.value.has_value()) {
            const auto pixel = (*decoded.value)->read(-1, 2);
            check.expect(pixel &&
                             std::abs(pixel.value()->red() - pixel.value()->green()) < 0.001F &&
                             std::abs(pixel.value()->green() - pixel.value()->blue()) < 0.001F,
                         "EXR luminance is replicated to RGB");
        }
    }
    {
        const auto path = scratch.file("truncated.exr");
        exr(path, false, Imf::HALF);
        std::ifstream source(path, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(source)), {});
        bytes.resize(8);
        std::ofstream target(path, std::ios::binary | std::ios::trunc);
        target.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        target.close();
        const auto result = media::probeImage(path);
        check.expect(!result && result.code != media::ImageDiagnosticCode::None,
                     "truncated EXR is refused with a typed diagnostic");
    }
    for (const auto fixture :
         {std::pair{"deep.exr", 0x00000800U}, std::pair{"multipart.exr", 0x00001000U}}) {
        const auto path = scratch.file(fixture.first);
        exr(path, false, Imf::HALF);
        patchExrVersionFlag(path, fixture.second);
        const auto result = media::probeImage(path);
        const auto expected = fixture.second == 0x00000800U
                                  ? media::ImageDiagnosticCode::UnsupportedDeepImage
                                  : media::ImageDiagnosticCode::UnsupportedMultipart;
        check.expect(!result && result.code == expected,
                     "deep and multipart EXR fixtures are refused with typed diagnostics");
    }
    // "" with an empty base name is the render-farm form: 0000.png, 0001.png, ... (owner,
    // 2026-09-15: such a folder imported as individual images).
    // Any name shape: the last digit run is the frame; prefix, suffix and padding are free.
    for (const auto* prefix : {"take.", "take_", "take", ""}) {
        png(scratch.file(std::string(prefix) + "0001.png"));
        png(scratch.file(std::string(prefix) + "0003.png"));
        const auto sequence = media::scanSequence(scratch.file(std::string(prefix) + "0001.png"));
        check.expect(sequence.value.has_value() && sequence.value->members.size() == 2 &&
                         sequence.value->gaps == std::vector<std::int64_t>{2} &&
                         sequence.value->padding == 4,
                     "sequence form detects members, padding and gap");
    }
    {
        exr(scratch.file("plates.0001.exr"), false, Imf::HALF);
        exr(scratch.file("plates.0002.exr"), false, Imf::HALF);
        const auto sequence = media::scanSequence(scratch.file("plates.0001.exr"));
        check.expect(sequence.value.has_value() && sequence.value->members.size() == 2 &&
                         sequence.value->pattern == "plates.####.exr",
                     "EXR files scan as numbered image sequences");
    }
    {
        png(scratch.file("stereo0001_left.png"));
        png(scratch.file("stereo0002_left.png"));
        png(scratch.file("stereo0001_right.png"));
        const auto sequence = media::scanSequence(scratch.file("stereo0001_left.png"));
        check.expect(sequence.value.has_value() && sequence.value->members.size() == 2 &&
                         sequence.value->pattern == "stereo####_left.png",
                     "a suffix after the frame number is part of the member identity");
    }
    {
        png(scratch.file("mixed7.png"));
        png(scratch.file("mixed08.png"));
        png(scratch.file("mixed009.PNG"));
        const auto sequence = media::scanSequence(scratch.file("mixed7.png"));
        check.expect(sequence.value.has_value() && sequence.value->members.size() == 3 &&
                         sequence.value->first == 7 && sequence.value->last == 9 &&
                         !sequence.value->diagnostics.empty(),
                     "mixed padding and extension case still form one sequence, with a note");
    }
    for (std::size_t size = 1; size < 64; ++size) {
        const auto path = scratch.file("hostile.png");
        {
            std::ofstream file(path, std::ios::binary);
            const std::string bytes(size, static_cast<char>(size));
            file.write(bytes.data(), static_cast<std::streamsize>(size));
        }
        check.expect(!media::decodeImage(path), "hostile random short payload rejected");
    }
    return check.failures() == 0 ? 0 : 1;
}
