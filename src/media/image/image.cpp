#include "stb_image_adapter.hpp"
#include <algorithm>
#include <array>
#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/media/image.hpp>
#include <charconv>
#include <fstream>
#include <span>
#include <utility>

namespace bloom::media {
namespace {
[[nodiscard]] bool cancelled(const CancelImageWork& cancel) { return cancel && cancel(); }
ImageResult<std::vector<std::byte>> readImage(const std::filesystem::path& path,
                                              const CancelImageWork& cancel) {
    if (cancelled(cancel))
        return {{}, {}, true};
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return {{}, "Image file is missing or unreadable"};
    const auto size = input.tellg();
    if (size <= 0 || size > static_cast<std::streamoff>(kMaxImageFileBytes))
        return {{}, "Image file exceeds the 64 MiB limit or is empty"};
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    for (std::size_t offset = 0; offset < bytes.size();) {
        if (cancelled(cancel))
            return {{}, {}, true};
        const auto count = std::min<std::size_t>(65536, bytes.size() - offset);
        if (!input.read(reinterpret_cast<char*>(bytes.data() + offset),
                        static_cast<std::streamsize>(count)))
            return {{}, "Image read failed"};
        offset += count;
    }
    if (input.peek() != std::char_traits<char>::eof())
        return {{}, "Image changed while reading"};
    return {std::move(bytes), {}};
}
[[nodiscard]] bool validInfo(const detail::ImageInfo& info) {
    return info.width > 0 && info.height > 0 &&
           static_cast<std::uint32_t>(info.width) <= kMaxImageDimension &&
           static_cast<std::uint32_t>(info.height) <= kMaxImageDimension &&
           static_cast<std::uint64_t>(info.width) * static_cast<std::uint64_t>(info.height) <=
               kMaxImagePixels;
}
} // namespace
std::filesystem::path resolveImagePath(std::string_view relativePath, std::string_view relinkHint,
                                       const std::filesystem::path& projectDirectory) {
    const auto relative = std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(relativePath.data()), relativePath.size()));
    if (!projectDirectory.empty()) {
        const auto candidate = projectDirectory / relative;
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error))
            return candidate;
    }
    auto uri = std::string(relinkHint);
    if (!uri.starts_with("file://"))
        return {};
    uri.erase(0, 7);
    std::string path;
    for (std::size_t index = 0; index < uri.size(); ++index) {
        if (uri[index] == '%' && index + 2 < uri.size()) {
            unsigned value = 0;
            const auto parsed =
                std::from_chars(uri.data() + index + 1, uri.data() + index + 3, value, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != uri.data() + index + 3 || value == 0)
                return {};
            path += static_cast<char>(value);
            index += 2;
        } else
            path += uri[index];
    }
#if defined(_WIN32)
    if (path.size() > 2 && path[0] == '/' && path[2] == ':')
        path.erase(0, 1);
#endif
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
}
ImageResult<ImageProbe> probeImage(const std::filesystem::path& path,
                                   const CancelImageWork& cancel) {
    try {
        auto bytes = readImage(path, cancel);
        if (!bytes.value.has_value())
            return {{}, bytes.diagnostic, bytes.cancelled};
        detail::ImageInfo info;
        if (!detail::imageInfo(*bytes.value, info) || !validInfo(info))
            return {{}, "Invalid PNG/JPEG header or image dimensions exceed limits"};
        const auto digest = core::Sha256Hasher::hash(*bytes.value);
        if (!digest)
            return {{}, "Image digest failed"};
        return {ImageProbe{static_cast<std::uint32_t>(info.width),
                           static_cast<std::uint32_t>(info.height),
                           static_cast<std::uint8_t>(info.sixteenBit ? 16 : 8), *digest},
                {}};
    } catch (const std::exception&) {
        return {{}, "Image probe allocation or I/O failed"};
    }
}
ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeImage(const std::filesystem::path& path, ImageInterpretation interpretation,
            std::shared_ptr<const color::CpuInputProcessor> processor,
            const CancelImageWork& cancel, const ImageProgress& progress, std::size_t pixelBudget,
            std::optional<core::Sha256Digest> expectedDigest) {
    try {
        if (progress)
            progress(0, 0);
        auto bytes = readImage(path, cancel);
        if (!bytes.value.has_value())
            return {{}, bytes.diagnostic, bytes.cancelled};
        if (expectedDigest && core::Sha256Hasher::hash(*bytes.value) != expectedDigest)
            return {{}, "Image content changed; relink the asset"};
        detail::ImageInfo info;
        if (!detail::imageInfo(*bytes.value, info) || !validInfo(info))
            return {{}, "Invalid PNG/JPEG header or image dimensions exceed limits"};
        const auto width = static_cast<std::uint32_t>(info.width);
        const auto height = static_cast<std::uint32_t>(info.height);
        if (static_cast<std::uint64_t>(width) * height * sizeof(render::Rgba32f) > pixelBudget)
            return {{}, "Decoded image exceeds the pixel storage budget"};
        if (cancelled(cancel))
            return {{}, {}, true};
        auto samples = detail::imageSamples(*bytes.value, info);
        if (samples.empty())
            return {{}, "PNG/JPEG decode failed"};
        bytes.value.reset();
        if (cancelled(cancel))
            return {{}, {}, true};
        const bool convert = interpretation.colorSpace == ImageColorSpace::Auto ||
                             interpretation.colorSpace == ImageColorSpace::Srgb;
        if (convert && !processor) {
            const auto resolved = color::resolveBloomNeutralV1BuiltIn(
                color::OcioConfigLocatorKind::BloomBuiltIn, color::kBloomNeutralV1ConfigUri,
                color::kBloomNeutralV1ConfigDigest);
            if (!resolved)
                return {{}, "Bloom Neutral input config could not be resolved"};
            processor = color::CpuInputProcessor::prepare(*resolved.resolved());
        }
        if (convert && !processor)
            return {{}, "Input colour processor could not be prepared"};
        const auto window = render::ImageWindow::create(0, 0, width, height);
        const auto descriptor = render::Rgba32fImageDescriptor::create(
            *window.value(), *window.value(), core::PixelAspectRatio::square());
        auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), pixelBudget);
        if (!builder)
            return {{}, "Process image allocation failed"};
        std::vector<std::array<float, 4>> row(width);
        for (std::uint32_t y = 0; y < height; ++y) {
            if (cancelled(cancel))
                return {{}, {}, true};
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
                for (std::size_t c = 0; c < 4; ++c)
                    row[x][c] = static_cast<float>(samples[offset + c]) / 65535.0F;
                if (interpretation.alphaAssociation == ImageAlphaAssociation::Premultiplied) {
                    for (std::size_t c = 0; c < 3; ++c)
                        row[x][c] = row[x][3] > 0.0F ? row[x][c] / row[x][3] : 0.0F;
                }
            }
            if (convert && !processor->apply(row))
                return {{}, "Input colour conversion failed"};
            auto outputRow = builder.value()->row(y);
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto& sample = row[x];
                auto pixel = render::Rgba32f::fromPremultiplied(
                    sample[0] * sample[3], sample[1] * sample[3], sample[2] * sample[3], sample[3]);
                if (!pixel)
                    return {{}, "Input colour conversion produced an invalid pixel"};
                (*outputRow.value())[x] = *pixel.value();
            }
            if (progress)
                progress(y + 1, height);
        }
        auto image = std::move(*builder.value()).freeze();
        if (!image)
            return {{}, "Process image validation failed"};
        return {std::make_shared<const render::Rgba32fImage>(std::move(*image.value())), {}};
    } catch (const std::exception&) {
        return {{}, "Image decode allocation or I/O failed"};
    }
}
} // namespace bloom::media
