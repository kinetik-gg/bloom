#include "exr_backend.hpp"
#include "image_budget.hpp"
#include "stb_image_adapter.hpp"
#include <algorithm>
#include <array>
#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/color/ocio_cpu_input_processor.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/media/image.hpp>
#include <charconv>
#include <fstream>
#include <limits>
#include <span>
#include <utility>

namespace bloom::media {
namespace {
[[nodiscard]] bool cancelled(const CancelImageWork& cancel) { return cancel && cancel(); }

constexpr std::size_t kImageHeaderBytes = 16;
constexpr std::size_t kHashChunkBytes = 1024U * 1024U;
// PNG/JPEG peak admission phases: the decoded RGBA32F image (16 bytes/px), the codec's RGBA16
// staging buffer (4 channels * 2 bytes = 8 bytes/px), and the one-row float conversion scratch
// (16 bytes/px). Every phase that is live at once must fit the caller's explicit budget before the
// parser runs.
constexpr std::uint64_t kRgba32fBytesPerPixel = sizeof(render::Rgba32f);
constexpr std::uint64_t kPngJpegStagingBytesPerPixel = 8U;

// Streams the whole file through SHA-256 in bounded chunks. There is no whole-file cap: the hash is
// the content identity, the working set is one chunk, and cancellation is polled per chunk.
[[nodiscard]] ImageResult<core::Sha256Digest> hashFile(const std::filesystem::path& path,
                                                       const CancelImageWork& cancel) {
    if (cancelled(cancel))
        return {{}, {}, true};
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {
            {}, "Image file is missing or unreadable", false, ImageDiagnosticCode::FileUnavailable};
    core::Sha256Hasher hasher;
    std::vector<std::byte> buffer(kHashChunkBytes);
    for (;;) {
        if (cancelled(cancel))
            return {{}, {}, true};
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count > 0 && !hasher.update(std::span(buffer.data(), count)))
            return {{}, "Image digest failed", false, ImageDiagnosticCode::FileReadFailed};
        if (count < buffer.size()) {
            if (!input.eof())
                return {{}, "Image read failed", false, ImageDiagnosticCode::FileReadFailed};
            break;
        }
    }
    return {hasher.finalize(), {}};
}

[[nodiscard]] bool readHeader(const std::filesystem::path& path, const std::span<std::byte> head,
                              std::size_t& bytesRead) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    input.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    bytesRead = static_cast<std::size_t>(input.gcount());
    return !input.bad();
}

// A codec-level geometry check only: dimensions must be positive and their RGBA32F byte count must
// be representable. The caller's pixelBudget decides how large a decode is admitted.
[[nodiscard]] bool validInfo(const detail::ImageInfo& info) {
    if (info.width <= 0 || info.height <= 0)
        return false;
    const auto pixels =
        static_cast<std::uint64_t>(info.width) * static_cast<std::uint64_t>(info.height);
    return pixels <= std::numeric_limits<std::size_t>::max() / kRgba32fBytesPerPixel;
}

[[nodiscard]] bool isExrMagic(const std::span<const std::byte> bytes) {
    return bytes.size() >= 4 && static_cast<unsigned char>(bytes[0]) == 0x76U &&
           static_cast<unsigned char>(bytes[1]) == 0x2fU &&
           static_cast<unsigned char>(bytes[2]) == 0x31U &&
           static_cast<unsigned char>(bytes[3]) == 0x01U;
}

[[nodiscard]] bool isPngMagic(const std::span<const std::byte> bytes) {
    return bytes.size() >= 4 && static_cast<unsigned char>(bytes[0]) == 137U &&
           static_cast<unsigned char>(bytes[1]) == 'P' &&
           static_cast<unsigned char>(bytes[2]) == 'N' &&
           static_cast<unsigned char>(bytes[3]) == 'G';
}

[[nodiscard]] bool isTiffMagic(const std::span<const std::byte> bytes) {
    if (bytes.size() < 4)
        return false;
    const auto byte = [&](const std::size_t index) {
        return static_cast<unsigned char>(bytes[index]);
    };
    return (byte(0) == 'I' && byte(1) == 'I' &&
            ((byte(2) == 42U && byte(3) == 0U) || (byte(2) == 43U && byte(3) == 0U))) ||
           (byte(0) == 'M' && byte(1) == 'M' &&
            ((byte(2) == 0U && byte(3) == 42U) || (byte(2) == 0U && byte(3) == 43U)));
}

template <typename T> [[nodiscard]] ImageResult<T> providerMissing() {
    return {{},
            "TIFF provider is missing; MEDIA-3 must provide the worker adapter",
            false,
            ImageDiagnosticCode::ProviderMissing};
}

ImageResult<std::shared_ptr<const render::Rgba32fImage>>
applyInputProcessor(const std::shared_ptr<const render::Rgba32fImage>& image,
                    const std::shared_ptr<const color::CpuColorSpaceProcessor>& processor,
                    const CancelImageWork& cancel, const std::size_t pixelBudget) {
    if (!processor)
        return {image, {}};
    if (!image || image->descriptor() == nullptr)
        return {{},
                "Image provider returned an invalid RGBA32F image",
                false,
                ImageDiagnosticCode::ProviderFailed};
    const auto descriptor = *image->descriptor();
    auto builder = render::Rgba32fImageBuilder::create(descriptor, pixelBudget);
    if (!builder)
        return {{},
                "Decoded image exceeds the pixel storage budget",
                false,
                ImageDiagnosticCode::PixelBudgetExceeded};
    const auto extent = descriptor.dataWindow().extent();
    const auto source = image->pixels();
    std::vector<std::array<float, 4>> row(extent.width());
    for (std::uint32_t y = 0; y < extent.height(); ++y) {
        if (cancelled(cancel))
            return {{}, {}, true, ImageDiagnosticCode::Cancelled};
        for (std::uint32_t x = 0; x < extent.width(); ++x) {
            const auto& pixel = source[static_cast<std::size_t>(y) * extent.width() + x];
            const auto alpha = pixel.alpha();
            row[x] = {alpha > 0.0F ? pixel.red() / alpha : 0.0F,
                      alpha > 0.0F ? pixel.green() / alpha : 0.0F,
                      alpha > 0.0F ? pixel.blue() / alpha : 0.0F, alpha};
        }
        if (!processor->apply(row))
            return {{},
                    "Input colour conversion failed",
                    false,
                    ImageDiagnosticCode::ColorSpaceUnavailable};
        auto outputRow =
            builder.value()->row(descriptor.dataWindow().originY() + static_cast<std::int64_t>(y));
        if (!outputRow)
            return {
                {}, "Image output row is unavailable", false, ImageDiagnosticCode::DecodeFailed};
        for (std::uint32_t x = 0; x < extent.width(); ++x) {
            const auto& sample = row[x];
            const auto output = render::Rgba32f::fromPremultiplied(
                sample[0] * sample[3], sample[1] * sample[3], sample[2] * sample[3], sample[3]);
            if (!output)
                return {{},
                        "Input colour conversion produced an invalid pixel",
                        false,
                        ImageDiagnosticCode::ColorSpaceUnavailable};
            (*outputRow.value())[x] = *output.value();
        }
    }
    auto output = std::move(*builder.value()).freeze();
    if (!output)
        return {{},
                "Input colour conversion produced an invalid image",
                false,
                ImageDiagnosticCode::ColorSpaceUnavailable};
    return {std::make_shared<const render::Rgba32fImage>(std::move(*output.value())), {}};
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
ImageResult<ImageProbe> probeImage(const std::filesystem::path& path, const CancelImageWork& cancel,
                                   const ImageProvider* provider) {
    try {
        auto digest = hashFile(path, cancel);
        if (!digest.value.has_value())
            return {{}, digest.diagnostic, digest.cancelled};
        std::array<std::byte, kImageHeaderBytes> head{};
        std::size_t headBytes = 0;
        if (!readHeader(path, head, headBytes))
            return {{},
                    "Image file is missing or unreadable",
                    false,
                    ImageDiagnosticCode::FileUnavailable};
        const auto view = std::span<const std::byte>(head.data(), headBytes);
        if (isExrMagic(view))
            return detail::probeExr(path, *digest.value, cancel);
        if (isTiffMagic(view)) {
            if (provider == nullptr || !provider->decode)
                return providerMissing<ImageProbe>();
            const auto response = provider->decode({.path = path,
                                                    .interpretation = {},
                                                    .pixelBudget = kMaxImageStorageBytes,
                                                    .expectedDigest = *digest.value});
            if (!response.value.has_value())
                return {{},
                        response.diagnostic,
                        response.cancelled,
                        response.code == ImageDiagnosticCode::None
                            ? ImageDiagnosticCode::ProviderFailed
                            : response.code};
            return {response.value->probe, {}};
        }
        detail::ImageInfo info;
        if (!detail::imageInfo(path, info) || !validInfo(info))
            return {{},
                    "Invalid PNG/JPEG header or image dimensions",
                    false,
                    ImageDiagnosticCode::InvalidHeader};
        return {ImageProbe{static_cast<std::uint32_t>(info.width),
                           static_cast<std::uint32_t>(info.height),
                           static_cast<std::uint8_t>(info.sixteenBit ? 16 : 8),
                           *digest.value,
                           isPngMagic(view) ? ImageFormat::Png : ImageFormat::Jpeg,
                           ImageColorSpace::Srgb,
                           ImageAlphaAssociation::Straight,
                           "srgb_rec709_display",
                           {}},
                {}};
    } catch (const std::exception&) {
        return {{}, "Image probe allocation or I/O failed"};
    }
}
ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeImage(const std::filesystem::path& path, const ImageInterpretation& interpretation,
            std::shared_ptr<const color::CpuColorSpaceProcessor> processor,
            const CancelImageWork& cancel, const ImageProgress& progress, std::size_t pixelBudget,
            std::optional<core::Sha256Digest> expectedDigest, const ImageProvider* provider) {
    try {
        if (progress)
            progress(0, 0);
        auto digest = hashFile(path, cancel);
        if (!digest.value.has_value())
            return {{}, digest.diagnostic, digest.cancelled};
        if (expectedDigest && *digest.value != *expectedDigest)
            return {{},
                    "Image content changed; relink the asset",
                    false,
                    ImageDiagnosticCode::DigestMismatch};
        std::array<std::byte, kImageHeaderBytes> head{};
        std::size_t headBytes = 0;
        if (!readHeader(path, head, headBytes))
            return {{},
                    "Image file is missing or unreadable",
                    false,
                    ImageDiagnosticCode::FileUnavailable};
        const auto view = std::span<const std::byte>(head.data(), headBytes);
        if (isExrMagic(view))
            return detail::decodeExr(path, interpretation, std::move(processor), cancel, progress,
                                     pixelBudget);
        if (isTiffMagic(view)) {
            if (provider == nullptr || !provider->decode)
                return providerMissing<std::shared_ptr<const render::Rgba32fImage>>();
            if (!processor && !interpretation.inputColorSpaceId.empty()) {
                const auto resolved = color::resolveBloomNeutralV1BuiltIn(
                    color::OcioConfigLocatorKind::BloomBuiltIn, color::kBloomNeutralV1ConfigUri,
                    color::kBloomNeutralV1ConfigDigest);
                if (!resolved)
                    return {{},
                            "Bloom Neutral input config could not be resolved",
                            false,
                            ImageDiagnosticCode::ColorSpaceUnavailable};
                auto prepared = color::CpuColorSpaceProcessor::prepare(
                    *resolved.resolved(), interpretation.inputColorSpaceId,
                    resolved.resolved()->processColorSpaceId());
                if (!prepared)
                    return {{},
                            "Input colour space is unavailable in the selected config",
                            false,
                            ImageDiagnosticCode::ColorSpaceUnavailable};
                processor = std::move(prepared).takeProcessor();
            }
            const auto response = provider->decode({.path = path,
                                                    .interpretation = interpretation,
                                                    .pixelBudget = pixelBudget,
                                                    .expectedDigest = expectedDigest});
            if (!response.value.has_value())
                return {{},
                        response.diagnostic,
                        response.cancelled,
                        response.code == ImageDiagnosticCode::None
                            ? ImageDiagnosticCode::ProviderFailed
                            : response.code};
            return applyInputProcessor(response.value->image, processor, cancel, pixelBudget);
        }
        detail::ImageInfo info;
        if (!detail::imageInfo(path, info) || !validInfo(info))
            return {{},
                    "Invalid PNG/JPEG header or image dimensions",
                    false,
                    ImageDiagnosticCode::InvalidHeader};
        const auto width = static_cast<std::uint64_t>(info.width);
        const auto height = static_cast<std::uint64_t>(info.height);
        std::uint64_t pixels = 0;
        std::uint64_t finalBytes = 0;
        std::uint64_t stagingBytes = 0;
        std::uint64_t rowBytes = 0;
        const bool sized =
            detail::checkedSizeProduct(width, height, pixels) &&
            detail::checkedSizeProduct(pixels, kRgba32fBytesPerPixel, finalBytes) &&
            detail::checkedSizeProduct(pixels, kPngJpegStagingBytesPerPixel, stagingBytes) &&
            detail::checkedSizeProduct(width, sizeof(std::array<float, 4>), rowBytes);
        if (!sized ||
            !detail::decodeWorkingSetFits(finalBytes, stagingBytes, rowBytes, pixelBudget))
            return {{},
                    "Decoded image exceeds the pixel storage budget",
                    false,
                    ImageDiagnosticCode::PixelBudgetExceeded};
        if (cancelled(cancel))
            return {{}, {}, true};
        auto samples = detail::imageSamples(path, info, pixelBudget);
        if (samples.empty())
            return {{},
                    "PNG/JPEG decode failed or exceeded the pixel budget",
                    false,
                    ImageDiagnosticCode::DecodeFailed};
        if (cancelled(cancel))
            return {{}, {}, true};
        const bool convert = !interpretation.inputColorSpaceId.empty() ||
                             interpretation.colorSpace == ImageColorSpace::Auto ||
                             interpretation.colorSpace == ImageColorSpace::Srgb ||
                             processor != nullptr;
        std::shared_ptr<const color::CpuInputProcessor> compatibilityProcessor;
        if (convert && !processor) {
            const auto resolved = color::resolveBloomNeutralV1BuiltIn(
                color::OcioConfigLocatorKind::BloomBuiltIn, color::kBloomNeutralV1ConfigUri,
                color::kBloomNeutralV1ConfigDigest);
            if (!resolved)
                return {{}, "Bloom Neutral input config could not be resolved"};
            if (interpretation.inputColorSpaceId.empty()) {
                // Direct media callers from the 1.19 API do not carry a project config. Preserve
                // their exact Bloom Neutral default while all project-managed paths supply the
                // general config processor explicitly.
                compatibilityProcessor = color::CpuInputProcessor::prepare(*resolved.resolved());
            } else {
                auto prepared = color::CpuColorSpaceProcessor::prepare(
                    *resolved.resolved(), interpretation.inputColorSpaceId,
                    resolved.resolved()->processColorSpaceId());
                if (!prepared)
                    return {{},
                            "Input colour space is unavailable in the selected config",
                            false,
                            ImageDiagnosticCode::ColorSpaceUnavailable};
                processor = std::move(prepared).takeProcessor();
            }
        }
        if (convert && !processor && !compatibilityProcessor)
            return {{},
                    "Input colour processor could not be prepared",
                    false,
                    ImageDiagnosticCode::ColorSpaceUnavailable};
        const auto window = render::ImageWindow::create(0, 0, width, height);
        const auto descriptor = render::Rgba32fImageDescriptor::create(
            *window.value(), *window.value(), core::PixelAspectRatio::square());
        auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), pixelBudget);
        if (!builder)
            return {{}, "Process image allocation failed"};
        std::vector<std::array<float, 4>> row(static_cast<std::size_t>(width));
        for (std::uint64_t y = 0; y < height; ++y) {
            if (cancelled(cancel))
                return {{}, {}, true};
            for (std::uint64_t x = 0; x < width; ++x) {
                const auto offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                     static_cast<std::size_t>(x)) *
                                    4;
                for (std::size_t c = 0; c < 4; ++c)
                    row[static_cast<std::size_t>(x)][c] =
                        static_cast<float>(samples[offset + c]) / 65535.0F;
                if (interpretation.alphaAssociation == ImageAlphaAssociation::Premultiplied) {
                    for (std::size_t c = 0; c < 3; ++c)
                        row[static_cast<std::size_t>(x)][c] =
                            row[static_cast<std::size_t>(x)][3] > 0.0F
                                ? row[static_cast<std::size_t>(x)][c] /
                                      row[static_cast<std::size_t>(x)][3]
                                : 0.0F;
                }
            }
            if (convert && ((processor && !processor->apply(row)) ||
                            (compatibilityProcessor && !compatibilityProcessor->apply(row))))
                return {{},
                        "Input colour conversion failed",
                        false,
                        ImageDiagnosticCode::ColorSpaceUnavailable};
            auto outputRow = builder.value()->row(static_cast<std::int64_t>(y));
            for (std::uint64_t x = 0; x < width; ++x) {
                const auto& sample = row[static_cast<std::size_t>(x)];
                auto pixel = render::Rgba32f::fromPremultiplied(
                    sample[0] * sample[3], sample[1] * sample[3], sample[2] * sample[3], sample[3]);
                if (!pixel)
                    return {{}, "Input colour conversion produced an invalid pixel"};
                (*outputRow.value())[static_cast<std::size_t>(x)] = *pixel.value();
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

ImageResult<ImageProviderEncodeResponse> encodeImage(const ImageProviderEncodeRequest& request,
                                                     const ImageProvider* provider) {
    if (provider == nullptr || !provider->encode)
        return providerMissing<ImageProviderEncodeResponse>();
    try {
        const auto response = provider->encode(request);
        if (!response.value.has_value())
            return {{},
                    response.diagnostic,
                    response.cancelled,
                    response.code == ImageDiagnosticCode::None ? ImageDiagnosticCode::ProviderFailed
                                                               : response.code};
        if (!response.value->written)
            return {{},
                    "Image provider did not write the requested artifact",
                    false,
                    ImageDiagnosticCode::ProviderFailed};
        return {response.value, {}};
    } catch (...) {
        return {{}, "Image provider failed", false, ImageDiagnosticCode::ProviderFailed};
    }
}
} // namespace bloom::media
