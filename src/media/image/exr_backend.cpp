#include "exr_backend.hpp"
#include "image_budget.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_cpu_input_processor.hpp>

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfChromaticities.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfInputFile.h>
#include <ImfIntAttribute.h>
#include <ImfPixelType.h>
#include <ImfStandardAttributes.h>
#include <ImfStringAttribute.h>
#include <ImfTileDescriptionAttribute.h>
#include <ImfVersion.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::media::detail {
namespace {

constexpr std::array<std::uint32_t, 8> kRec709D65ChromaticityBits{
    0x3f23d70aU, 0x3ea8f5c3U, 0x3e99999aU, 0x3f19999aU,
    0x3e19999aU, 0x3d75c28fU, 0x3ea01a37U, 0x3ea872b0U};
constexpr std::array<float, 8> kAcesAp0Chromaticities{0.7347F, 0.2653F,  0.0F,     1.0F,
                                                      0.0001F, -0.0770F, 0.32168F, 0.33767F};
constexpr std::size_t kMaximumChannels = 4;

template <typename T>
[[nodiscard]] ImageResult<T> failure(const ImageDiagnosticCode code, std::string message) {
    return {{}, std::move(message), false, code};
}

[[nodiscard]] bool cancelled(const CancelImageWork& cancel) { return cancel && cancel(); }

[[nodiscard]] std::string utf8Path(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

[[nodiscard]] bool readVersionField(const std::filesystem::path& path, std::uint32_t& version,
                                    ImageDiagnosticCode& code) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        code = ImageDiagnosticCode::FileUnavailable;
        return false;
    }
    std::array<unsigned char, 8> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        code = ImageDiagnosticCode::Truncated;
        return false;
    }
    version = static_cast<std::uint32_t>(bytes[4]) | (static_cast<std::uint32_t>(bytes[5]) << 8U) |
              (static_cast<std::uint32_t>(bytes[6]) << 16U) |
              (static_cast<std::uint32_t>(bytes[7]) << 24U);
    return true;
}

[[nodiscard]] std::optional<std::uint64_t> boxExtent(const int minimum, const int maximum) {
    const auto distance = static_cast<std::int64_t>(maximum) - minimum;
    if (distance < 0)
        return std::nullopt;
    return static_cast<std::uint64_t>(distance) + 1U;
}

[[nodiscard]] std::optional<ImageDiagnosticCode> validateVersion(const std::uint32_t version) {
    const auto raw = static_cast<int>(version);
    if (Imf::getVersion(raw) != Imf::EXR_VERSION)
        return ImageDiagnosticCode::UnsupportedVersion;
    if (!Imf::supportsFlags(Imf::getFlags(raw)))
        return ImageDiagnosticCode::UnsupportedVersion;
    if (Imf::isNonImage(raw))
        return ImageDiagnosticCode::UnsupportedDeepImage;
    if (Imf::isMultiPart(raw))
        return ImageDiagnosticCode::UnsupportedMultipart;
    return std::nullopt;
}

struct ExrLayout final {
    Imath::Box2i dataWindow;
    Imath::Box2i displayWindow;
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    std::uint64_t displayWidth = 0;
    std::uint64_t displayHeight = 0;
    std::uint8_t bitDepth = 0;
    bool hasAlpha = false;
    bool luminance = false;
    bool tiled = false;
    std::uint32_t tileHeight = 1U;
    ImageAlphaAssociation alphaAssociation = ImageAlphaAssociation::Premultiplied;
    std::string colorSpaceTag;
    std::string interpretationAssumption;
};

[[nodiscard]] bool sameBits(const float value, const std::uint32_t expected) {
    return std::bit_cast<std::uint32_t>(value) == expected;
}

[[nodiscard]] bool isRec709D65(const Imf::Chromaticities& chromaticities) {
    const std::array<float, 8> values{chromaticities.red.x,   chromaticities.red.y,
                                      chromaticities.green.x, chromaticities.green.y,
                                      chromaticities.blue.x,  chromaticities.blue.y,
                                      chromaticities.white.x, chromaticities.white.y};
    for (std::size_t index = 0; index < values.size(); ++index)
        if (!std::isfinite(values[index]) ||
            !sameBits(values[index], kRec709D65ChromaticityBits[index]))
            return false;
    return true;
}

[[nodiscard]] bool isAcesAp1(const Imf::Chromaticities& chromaticities) {
    const std::array<std::uint32_t, 8> expected = bloom::color::kAcesCgV1ChromaticityBits;
    const std::array<float, 8> values{chromaticities.red.x,   chromaticities.red.y,
                                      chromaticities.green.x, chromaticities.green.y,
                                      chromaticities.blue.x,  chromaticities.blue.y,
                                      chromaticities.white.x, chromaticities.white.y};
    for (std::size_t index = 0; index < values.size(); ++index)
        if (!std::isfinite(values[index]) ||
            std::bit_cast<std::uint32_t>(values[index]) != expected[index])
            return false;
    return true;
}

[[nodiscard]] bool isAcesAp0(const Imf::Chromaticities& chromaticities) {
    const std::array<float, 8> values{chromaticities.red.x,   chromaticities.red.y,
                                      chromaticities.green.x, chromaticities.green.y,
                                      chromaticities.blue.x,  chromaticities.blue.y,
                                      chromaticities.white.x, chromaticities.white.y};
    for (std::size_t index = 0; index < values.size(); ++index)
        if (!std::isfinite(values[index]) ||
            std::abs(values[index] - kAcesAp0Chromaticities[index]) > 1.0e-6F)
            return false;
    return true;
}

[[nodiscard]] std::optional<ImageAlphaAssociation> alphaAssociation(const Imf::Header& header) {
    // OpenEXR has no required alpha-association attribute. OIIO and a few production tools use
    // these bounded metadata spellings; accepting them makes the interpretation explicit while
    // retaining the OpenEXR convention (associated/premultiplied) when absent.
    if (const auto* attribute =
            header.findTypedAttribute<Imf::StringAttribute>("alphaAssociation")) {
        const auto& value = attribute->value();
        if (value == "straight" || value == "unassociated")
            return ImageAlphaAssociation::Straight;
        if (value == "premultiplied" || value == "associated")
            return ImageAlphaAssociation::Premultiplied;
        return std::nullopt;
    }
    if (const auto* attribute = header.findTypedAttribute<Imf::IntAttribute>("unassociatedAlpha"))
        return attribute->value() != 0 ? ImageAlphaAssociation::Straight
                                       : ImageAlphaAssociation::Premultiplied;
    if (const auto* attribute =
            header.findTypedAttribute<Imf::IntAttribute>("oiio:UnassociatedAlpha"))
        return attribute->value() != 0 ? ImageAlphaAssociation::Straight
                                       : ImageAlphaAssociation::Premultiplied;
    return ImageAlphaAssociation::Premultiplied;
}

[[nodiscard]] std::optional<ExrLayout> inspectHeader(const Imf::Header& header, const bool tiled,
                                                     ImageDiagnosticCode& error) {
    const auto dataWidth = boxExtent(header.dataWindow().min.x, header.dataWindow().max.x);
    const auto dataHeight = boxExtent(header.dataWindow().min.y, header.dataWindow().max.y);
    const auto displayWidth = boxExtent(header.displayWindow().min.x, header.displayWindow().max.x);
    const auto displayHeight =
        boxExtent(header.displayWindow().min.y, header.displayWindow().max.y);
    if (!dataWidth || !dataHeight || !displayWidth || !displayHeight) {
        error = ImageDiagnosticCode::InvalidDataWindow;
        return std::nullopt;
    }
    // There is no fixed dimension or pixel ceiling: OpenEXR is admitted at decode time by the
    // caller's pixelBudget. The only geometry constraint is what the image types can represent
    // (32-bit extent) plus multiplication overflow, and this check never allocates pixel storage.
    const auto maximumExtent =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
    const auto productOverflows = [](const std::uint64_t a, const std::uint64_t b) {
        return b != 0 && a > std::numeric_limits<std::uint64_t>::max() / b;
    };
    if (*dataWidth > maximumExtent || *dataHeight > maximumExtent ||
        *displayWidth > maximumExtent || *displayHeight > maximumExtent ||
        productOverflows(*dataWidth, *dataHeight) ||
        productOverflows(*displayWidth, *displayHeight)) {
        error = ImageDiagnosticCode::DimensionsExceeded;
        return std::nullopt;
    }
    std::uint32_t tileHeight = 1U;
    if (tiled) {
        const auto* tiles = header.findTypedAttribute<Imf::TileDescriptionAttribute>("tiles");
        if (tiles == nullptr || tiles->value().mode != Imf::ONE_LEVEL) {
            error = ImageDiagnosticCode::UnsupportedTiledLevels;
            return std::nullopt;
        }
        tileHeight = static_cast<std::uint32_t>(tiles->value().ySize);
        if (tileHeight == 0U) {
            error = ImageDiagnosticCode::InvalidHeader;
            return std::nullopt;
        }
    }

    const auto& channels = header.channels();
    std::size_t channelCount = 0;
    const Imf::Channel* firstChannel = nullptr;
    bool hasR = false;
    bool hasG = false;
    bool hasB = false;
    bool hasY = false;
    bool hasA = false;
    for (auto it = channels.begin(); it != channels.end(); ++it) {
        if (++channelCount > kMaximumChannels) {
            error = ImageDiagnosticCode::UnsupportedChannels;
            return std::nullopt;
        }
        const std::string_view name = it.name();
        if (name == "R")
            hasR = true;
        else if (name == "G")
            hasG = true;
        else if (name == "B")
            hasB = true;
        else if (name == "Y")
            hasY = true;
        else if (name == "A")
            hasA = true;
        else {
            error = ImageDiagnosticCode::UnsupportedChannels;
            return std::nullopt;
        }
        if (firstChannel == nullptr)
            firstChannel = &it.channel();
        if (it.channel().type != Imf::HALF && it.channel().type != Imf::FLOAT) {
            error = ImageDiagnosticCode::UnsupportedChannelType;
            return std::nullopt;
        }
        if (it.channel().xSampling != 1 || it.channel().ySampling != 1) {
            error = ImageDiagnosticCode::UnsupportedChannels;
            return std::nullopt;
        }
        if (firstChannel->type != it.channel().type) {
            error = ImageDiagnosticCode::UnsupportedChannelType;
            return std::nullopt;
        }
    }
    const bool rgb = hasR && hasG && hasB;
    if ((!rgb && !hasY) || (rgb && hasY) || (hasA && !rgb && !hasY) ||
        (rgb && channelCount != (hasA ? 4U : 3U)) || (!rgb && channelCount != (hasA ? 2U : 1U))) {
        error = ImageDiagnosticCode::UnsupportedChannels;
        return std::nullopt;
    }
    const auto alpha = alphaAssociation(header);
    if (!alpha) {
        error = ImageDiagnosticCode::InvalidHeader;
        return std::nullopt;
    }

    ExrLayout result{header.dataWindow(),
                     header.displayWindow(),
                     *dataWidth,
                     *dataHeight,
                     *displayWidth,
                     *displayHeight,
                     static_cast<std::uint8_t>(firstChannel->type == Imf::HALF ? 16U : 32U),
                     hasA,
                     !rgb,
                     tiled,
                     tileHeight,
                     *alpha,
                     {},
                     {}};
    if (const auto* chromaticities =
            header.findTypedAttribute<Imf::ChromaticitiesAttribute>("chromaticities")) {
        result.colorSpaceTag = isAcesAp0(chromaticities->value())     ? "ACES2065-1"
                               : isAcesAp1(chromaticities->value())   ? "ACEScg"
                               : isRec709D65(chromaticities->value()) ? "lin_rec709_scene"
                                                                      : "exr.chromaticities";
        result.interpretationAssumption =
            result.colorSpaceTag == "ACES2065-1" ? "EXR chromaticities identify ACES AP0"
            : result.colorSpaceTag == "ACEScg"   ? "EXR chromaticities identify ACES AP1"
            : result.colorSpaceTag == "lin_rec709_scene"
                ? "EXR chromaticities identify Rec.709/D65 scene-linear RGB"
                : "EXR chromaticities identify scene-linear RGB outside the supported "
                  "automatic tags";
    } else {
        result.colorSpaceTag = "lin_rec709_scene";
        result.interpretationAssumption =
            "EXR scene-linear convention assumed; chromaticities attribute is absent";
    }
    return result;
}

[[nodiscard]] ImageProbe makeProbe(const ExrLayout& layout, const core::Sha256Digest digest) {
    return ImageProbe{static_cast<std::uint32_t>(layout.width),
                      static_cast<std::uint32_t>(layout.height),
                      layout.bitDepth,
                      digest,
                      ImageFormat::Exr,
                      ImageColorSpace::Linear,
                      layout.alphaAssociation,
                      layout.colorSpaceTag,
                      layout.interpretationAssumption};
}

} // namespace

ImageResult<ImageProbe> probeExr(const std::filesystem::path& path,
                                 const core::Sha256Digest contentDigest,
                                 const CancelImageWork& cancel) {
    if (cancelled(cancel))
        return failure<ImageProbe>(ImageDiagnosticCode::Cancelled, "Image probe cancelled");
    try {
        std::uint32_t version = 0;
        ImageDiagnosticCode versionError = ImageDiagnosticCode::InvalidHeader;
        if (!readVersionField(path, version, versionError))
            return failure<ImageProbe>(versionError, "OpenEXR version field is truncated");
        if (const auto versionFailure = validateVersion(version); versionFailure.has_value())
            return failure<ImageProbe>(*versionFailure, "OpenEXR version or flags are unsupported");
        Imf::InputFile input(utf8Path(path).c_str(), 1);
        ImageDiagnosticCode error = ImageDiagnosticCode::InvalidHeader;
        const auto layout =
            inspectHeader(input.header(), Imf::isTiled(static_cast<int>(version)), error);
        if (!layout)
            return failure<ImageProbe>(error, "OpenEXR header is unsupported or invalid");
        return {makeProbe(*layout, contentDigest), {}};
    } catch (const std::bad_alloc&) {
        return failure<ImageProbe>(ImageDiagnosticCode::AllocationFailure,
                                   "OpenEXR probe allocation failed");
    } catch (...) {
        return failure<ImageProbe>(ImageDiagnosticCode::InvalidHeader,
                                   "OpenEXR header could not be read");
    }
}

ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeExr(const std::filesystem::path& path, const ImageInterpretation& interpretation,
          std::shared_ptr<const color::CpuColorSpaceProcessor> processor,
          const CancelImageWork& cancel, const ImageProgress& progress,
          const std::size_t pixelBudget) {
    if (cancelled(cancel))
        return failure<std::shared_ptr<const render::Rgba32fImage>>(ImageDiagnosticCode::Cancelled,
                                                                    "Image decode cancelled");
    try {
        Imf::InputFile input(utf8Path(path).c_str(), 1);
        ImageDiagnosticCode error = ImageDiagnosticCode::InvalidHeader;
        const auto version = static_cast<std::uint32_t>(input.version());
        if (const auto versionFailure = validateVersion(version); versionFailure.has_value())
            return failure<std::shared_ptr<const render::Rgba32fImage>>(
                *versionFailure, "OpenEXR version or flags are unsupported");
        const auto layout =
            inspectHeader(input.header(), Imf::isTiled(static_cast<int>(version)), error);
        if (!layout)
            return failure<std::shared_ptr<const render::Rgba32fImage>>(
                error, "OpenEXR header is unsupported or invalid");

        const auto dataWindow = render::ImageWindow::create(
            layout->dataWindow.min.x, layout->dataWindow.min.y, layout->width, layout->height);
        const auto displayWindow =
            render::ImageWindow::create(layout->displayWindow.min.x, layout->displayWindow.min.y,
                                        layout->displayWidth, layout->displayHeight);
        if (!dataWindow || !displayWindow)
            return failure<std::shared_ptr<const render::Rgba32fImage>>(
                ImageDiagnosticCode::InvalidDataWindow, "OpenEXR image window is invalid");
        const auto descriptor = render::Rgba32fImageDescriptor::create(
            *dataWindow.value(), *displayWindow.value(), core::PixelAspectRatio::square());
        if (!descriptor)
            return failure<std::shared_ptr<const render::Rgba32fImage>>(
                ImageDiagnosticCode::InvalidDataWindow, "OpenEXR image descriptor is invalid");
        // Peak admission: final RGBA32F storage plus the bounded scanline band and the one-row
        // scratch must fit the caller's explicit budget. This replaces four full-frame planes (peak
        // ~2x the decoded bytes) with a small band, so admission matches real peak memory.
        const auto width = layout->width;
        const auto height = layout->height;
        const std::uint64_t channelCount =
            static_cast<std::uint64_t>(layout->luminance ? 1U : 3U) + (layout->hasAlpha ? 1U : 0U);
        const std::uint64_t finalBytes = descriptor.value()->layout().pixelStorageBytes;
        constexpr std::uint64_t kBandTargetBytes = 1024U * 1024U;
        std::uint64_t chunkRows = 1U;
        if (layout->tiled) {
            chunkRows = std::max<std::uint64_t>(1U, layout->tileHeight);
        } else {
            std::uint64_t bytesPerRow = 0;
            if (!detail::checkedSizeProduct(width, channelCount * sizeof(float), bytesPerRow))
                bytesPerRow = std::numeric_limits<std::uint64_t>::max();
            chunkRows = std::max<std::uint64_t>(1U, kBandTargetBytes /
                                                        std::max<std::uint64_t>(bytesPerRow, 1U));
        }
        chunkRows = std::min(chunkRows, height);
        // Every scratch term is both overflow-checked and admitted by subtraction, so no removed
        // geometry cap can wrap the working-set comparison past the budget.
        std::uint64_t rowBytes = 0;
        std::uint64_t bandRows = 0;
        std::uint64_t bandBytes = 0;
        const bool scratchSized =
            detail::checkedSizeProduct(width, sizeof(std::array<float, 4>), rowBytes) &&
            detail::checkedSizeProduct(chunkRows, width, bandRows) &&
            detail::checkedSizeProduct(bandRows, channelCount * sizeof(float), bandBytes);
        if (!scratchSized ||
            !detail::decodeWorkingSetFits(finalBytes, bandBytes, rowBytes, pixelBudget))
            return failure<std::shared_ptr<const render::Rgba32fImage>>(
                ImageDiagnosticCode::PixelBudgetExceeded,
                "Decoded image exceeds the pixel working-set budget");
        auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), pixelBudget);
        if (!builder)
            return failure<std::shared_ptr<const render::Rgba32fImage>>(
                ImageDiagnosticCode::AllocationFailure, "Process image allocation failed");

        const bool sourcePremultiplied =
            (interpretation.alphaAssociation == ImageAlphaAssociation::Auto
                 ? layout->alphaAssociation
                 : interpretation.alphaAssociation) == ImageAlphaAssociation::Premultiplied;
        const bool convert = !interpretation.inputColorSpaceId.empty() ||
                             interpretation.colorSpace == ImageColorSpace::Srgb ||
                             processor != nullptr;
        std::shared_ptr<const color::CpuInputProcessor> compatibilityProcessor;
        if (convert && !processor) {
            const auto resolved = color::resolveBloomNeutralV1BuiltIn(
                color::OcioConfigLocatorKind::BloomBuiltIn, color::kBloomNeutralV1ConfigUri,
                color::kBloomNeutralV1ConfigDigest);
            if (!resolved)
                return failure<std::shared_ptr<const render::Rgba32fImage>>(
                    ImageDiagnosticCode::DecodeFailed,
                    "Bloom Neutral input config could not be resolved");
            if (interpretation.inputColorSpaceId.empty()) {
                compatibilityProcessor = color::CpuInputProcessor::prepare(*resolved.resolved());
            } else {
                auto prepared = color::CpuColorSpaceProcessor::prepare(
                    *resolved.resolved(), interpretation.inputColorSpaceId,
                    resolved.resolved()->processColorSpaceId());
                if (!prepared)
                    return failure<std::shared_ptr<const render::Rgba32fImage>>(
                        ImageDiagnosticCode::ColorSpaceUnavailable,
                        "Input colour space is unavailable in the selected config");
                processor = std::move(prepared).takeProcessor();
            }
        }
        if (convert && !processor && !compatibilityProcessor)
            return failure<std::shared_ptr<const render::Rgba32fImage>>(
                ImageDiagnosticCode::ColorSpaceUnavailable,
                "Input colour processor could not be prepared");

        const auto bandSamples =
            static_cast<std::size_t>(chunkRows) * static_cast<std::size_t>(width);
        std::array<std::vector<float>, 4> band;
        const std::size_t rgbPlanes = layout->luminance ? 1U : 3U;
        for (std::size_t index = 0; index < rgbPlanes; ++index)
            band[index].assign(bandSamples, 0.0F);
        if (layout->hasAlpha)
            band[3].assign(bandSamples, 1.0F);
        std::vector<std::array<float, 4>> row(static_cast<std::size_t>(width));
        const auto rowStride = static_cast<std::size_t>(width) * sizeof(float);

        std::uint64_t completed = 0;
        if (progress)
            progress(0, height);
        while (completed < height) {
            if (cancelled(cancel))
                return failure<std::shared_ptr<const render::Rgba32fImage>>(
                    ImageDiagnosticCode::Cancelled, "Image decode cancelled");
            // A tiled read can touch a whole tile row, so tiled chunks are aligned to the tile
            // grid; scanline chunks are a bounded run of consecutive rows.
            const auto first = layout->tiled ? (completed / chunkRows) * chunkRows : completed;
            const auto count = std::min(chunkRows, height - first);
            const auto firstY = static_cast<std::int64_t>(layout->dataWindow.min.y) +
                                static_cast<std::int64_t>(first);
            const auto lastY = firstY + static_cast<std::int64_t>(count) - 1;
            Imf::FrameBuffer frameBuffer;
            const auto origin = Imath::V2i(layout->dataWindow.min.x, static_cast<int>(firstY));
            const auto insert = [&](const char* name, std::vector<float>& plane) {
                frameBuffer.insert(name, Imf::Slice::Make(Imf::FLOAT, plane.data(), origin,
                                                          static_cast<std::int64_t>(width),
                                                          static_cast<std::int64_t>(count),
                                                          sizeof(float), rowStride));
            };
            if (layout->luminance)
                insert("Y", band[0]);
            else {
                insert("R", band[0]);
                insert("G", band[1]);
                insert("B", band[2]);
            }
            if (layout->hasAlpha)
                insert("A", band[3]);
            input.setFrameBuffer(frameBuffer);
            input.readPixels(static_cast<int>(firstY), static_cast<int>(lastY));

            for (std::uint64_t local = 0; local < count; ++local) {
                if (cancelled(cancel))
                    return failure<std::shared_ptr<const render::Rgba32fImage>>(
                        ImageDiagnosticCode::Cancelled, "Image decode cancelled");
                const auto bandOffset =
                    static_cast<std::size_t>(local) * static_cast<std::size_t>(width);
                for (std::uint64_t x = 0; x < width; ++x) {
                    const auto offset = bandOffset + static_cast<std::size_t>(x);
                    const auto alpha = layout->hasAlpha ? band[3][offset] : 1.0F;
                    const auto luminance = layout->luminance ? band[0][offset] : 0.0F;
                    auto& target = row[static_cast<std::size_t>(x)];
                    target = layout->luminance
                                 ? std::array<float, 4>{luminance, luminance, luminance, alpha}
                                 : std::array<float, 4>{band[0][offset], band[1][offset],
                                                        band[2][offset], alpha};
                    if (sourcePremultiplied && convert) {
                        for (std::size_t channel = 0; channel < 3; ++channel)
                            target[channel] = target[3] > 0.0F ? target[channel] / target[3] : 0.0F;
                    }
                }
                if (convert && ((processor && !processor->apply(row)) ||
                                (compatibilityProcessor && !compatibilityProcessor->apply(row))))
                    return failure<std::shared_ptr<const render::Rgba32fImage>>(
                        ImageDiagnosticCode::ColorSpaceUnavailable,
                        "Input colour conversion failed");
                auto outputRow = builder.value()->row(
                    static_cast<std::int64_t>(layout->dataWindow.min.y) +
                    static_cast<std::int64_t>(first) + static_cast<std::int64_t>(local));
                if (!outputRow)
                    return failure<std::shared_ptr<const render::Rgba32fImage>>(
                        ImageDiagnosticCode::InvalidDataWindow, "OpenEXR output row is invalid");
                for (std::uint64_t x = 0; x < width; ++x) {
                    const auto& sample = row[static_cast<std::size_t>(x)];
                    const auto red =
                        sourcePremultiplied && !convert ? sample[0] : sample[0] * sample[3];
                    const auto green =
                        sourcePremultiplied && !convert ? sample[1] : sample[1] * sample[3];
                    const auto blue =
                        sourcePremultiplied && !convert ? sample[2] : sample[2] * sample[3];
                    const auto pixel =
                        render::Rgba32f::fromPremultiplied(red, green, blue, sample[3]);
                    if (!pixel)
                        return failure<std::shared_ptr<const render::Rgba32fImage>>(
                            ImageDiagnosticCode::DecodeFailed, "OpenEXR contains an invalid pixel");
                    (*outputRow.value())[static_cast<std::size_t>(x)] = *pixel.value();
                }
                ++completed;
                if (progress)
                    progress(completed, height);
            }
        }
        if (!input.isComplete())
            return failure<std::shared_ptr<const render::Rgba32fImage>>(
                ImageDiagnosticCode::Truncated, "OpenEXR pixel data is truncated");
        auto image = std::move(*builder.value()).freeze();
        if (!image)
            return failure<std::shared_ptr<const render::Rgba32fImage>>(
                ImageDiagnosticCode::DecodeFailed, "Process image validation failed");
        return {std::make_shared<const render::Rgba32fImage>(std::move(*image.value())), {}};
    } catch (const std::bad_alloc&) {
        return failure<std::shared_ptr<const render::Rgba32fImage>>(
            ImageDiagnosticCode::AllocationFailure, "OpenEXR decode allocation failed");
    } catch (...) {
        return failure<std::shared_ptr<const render::Rgba32fImage>>(
            ImageDiagnosticCode::Truncated, "OpenEXR pixel data could not be decoded");
    }
}

} // namespace bloom::media::detail
