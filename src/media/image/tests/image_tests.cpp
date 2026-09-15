#include "../../../output/tests/png_test_support.hpp"
#include "generated_jpeg.hpp"
#include <bloom/media/image.hpp>
#include <cmath>
#include <fstream>
#include <span>
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
    for (const std::string separator : {".", "_", ""}) {
        const auto prefix = "take" + separator;
        png(scratch.file(prefix + "0001.png"));
        png(scratch.file(prefix + "0003.png"));
        const auto sequence = media::scanSequence(scratch.file(prefix + "0001.png"));
        check.expect(sequence.value.has_value() && sequence.value->members.size() == 2 &&
                         sequence.value->gaps == std::vector<std::int64_t>{2} &&
                         sequence.value->padding == 4,
                     "sequence form detects members, padding and gap");
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
