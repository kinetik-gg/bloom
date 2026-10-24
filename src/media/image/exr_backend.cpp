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
#include <ImfIO.h>
#include <ImfInputFile.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>
#include <ImfPartType.h>
#include <ImfIntAttribute.h>
#include <ImfPixelType.h>
#include <ImfStandardAttributes.h>
#include <ImfStringAttribute.h>
#include <ImfTileDescriptionAttribute.h>
#include <ImfVersion.h>
#include <cstring>
#include <stdexcept>

#include <algorithm>
#include <cctype>
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

class RevisionStream final : public Imf::IStream {
  public:
    explicit RevisionStream(const std::span<const std::byte> bytes)
        : Imf::IStream("immutable-image-source"), bytes_(bytes) {}
    bool read(char* destination, const int count) override {
        if (count < 0 || static_cast<std::size_t>(count) > bytes_.size() - position_)
            throw std::runtime_error("truncated immutable image source");
        std::memcpy(destination, bytes_.data() + position_, static_cast<std::size_t>(count));
        position_ += static_cast<std::size_t>(count);
        return position_ != bytes_.size();
    }
    std::uint64_t tellg() override { return position_; }
    void seekg(const std::uint64_t position) override {
        if (position > bytes_.size())
            throw std::runtime_error("invalid immutable image seek");
        position_ = static_cast<std::size_t>(position);
    }

  private:
    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
};

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
    // The selected layer carries R/G/B or Y colour channels (ADR 0026); without them the part has
    // no colour image and only its channel set is available.
    bool colour = false;
    bool layerFound = false;
    // Colour channel names in R, G, B, A order (Y in slot 0 for luminance); empty when absent.
    std::array<std::string, 4> colourNames;
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

[[nodiscard]] bool partIsTiled(const Imf::Header& header) {
    return header.hasType() ? Imf::isTiled(header.type()) : header.hasTileDescription();
}

[[nodiscard]] std::optional<ExrLayout> inspectHeader(const Imf::Header& header,
                                                     const std::string_view layer,
                                                     ImageDiagnosticCode& error) {
    if (header.hasType() && Imf::isDeepData(header.type())) {
        error = ImageDiagnosticCode::UnsupportedDeepImage;
        return std::nullopt;
    }
    const bool tiled = partIsTiled(header);
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

    // Every channel of the part must be a supported, full-resolution sample kind, whichever layer
    // is selected; the colour image is formed from the selected layer's R/G/B[/A] or Y[/A].
    const auto& channels = header.channels();
    std::size_t channelCount = 0;
    bool layerFound = false;
    std::array<const Imf::Channel*, 5> colour{}; // R, G, B, A, Y
    std::array<std::string, 5> colourNames;
    std::uint8_t firstBitDepth = 0;
    for (auto it = channels.begin(); it != channels.end(); ++it) {
        ++channelCount;
        const auto& channel = it.channel();
        if (channel.type != Imf::HALF && channel.type != Imf::FLOAT && channel.type != Imf::UINT) {
            error = ImageDiagnosticCode::UnsupportedChannelType;
            return std::nullopt;
        }
        if (channel.xSampling != 1 || channel.ySampling != 1) {
            error = ImageDiagnosticCode::UnsupportedChannels;
            return std::nullopt;
        }
        if (firstBitDepth == 0)
            firstBitDepth = channel.type == Imf::HALF ? 16U : 32U;
        const std::string_view name = it.name();
        if (render::channelLayerName(name) != layer)
            continue;
        layerFound = true;
        if (channel.type == Imf::UINT)
            continue;
        const auto suffix = name.substr(layer.empty() ? 0 : layer.size() + 1);
        constexpr std::array<std::string_view, 5> kSuffixes{"R", "G", "B", "A", "Y"};
        for (std::size_t slot = 0; slot < kSuffixes.size(); ++slot)
            if (suffix == kSuffixes[slot]) {
                colour[slot] = &channel;
                colourNames[slot] = std::string(name);
            }
    }
    if (channelCount == 0) {
        error = ImageDiagnosticCode::UnsupportedChannels;
        return std::nullopt;
    }
    const bool rgb = colour[0] != nullptr && colour[1] != nullptr && colour[2] != nullptr;
    const bool luminance = !rgb && colour[4] != nullptr;
    const bool hasColour = rgb || luminance;
    const bool hasAlpha = hasColour && colour[3] != nullptr;
    std::uint8_t bitDepth = hasColour ? 16U : firstBitDepth;
    for (const auto* channel : colour)
        if (hasColour && channel != nullptr && channel->type == Imf::FLOAT)
            bitDepth = 32U;
    const auto alpha = alphaAssociation(header);
    if (!alpha) {
        error = ImageDiagnosticCode::InvalidHeader;
        return std::nullopt;
    }

    ExrLayout result;
    result.dataWindow = header.dataWindow();
    result.displayWindow = header.displayWindow();
    result.width = *dataWidth;
    result.height = *dataHeight;
    result.displayWidth = *displayWidth;
    result.displayHeight = *displayHeight;
    result.bitDepth = bitDepth;
    result.hasAlpha = hasAlpha;
    result.luminance = luminance;
    result.colour = hasColour;
    result.layerFound = layerFound;
    if (rgb) {
        result.colourNames = {colourNames[0], colourNames[1], colourNames[2],
                              hasAlpha ? colourNames[3] : std::string{}};
    } else if (luminance) {
        result.colourNames = {colourNames[4], {}, {}, hasAlpha ? colourNames[3] : std::string{}};
    }
    result.tiled = tiled;
    result.tileHeight = tileHeight;
    result.alphaAssociation = *alpha;
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

[[nodiscard]] ImageChannelSampleKind sampleKind(const Imf::PixelType type) {
    return type == Imf::HALF    ? ImageChannelSampleKind::Half
           : type == Imf::FLOAT ? ImageChannelSampleKind::Float
                                : ImageChannelSampleKind::Uint;
}

[[nodiscard]] ImagePartInfo makePartInfo(const Imf::Header& header, const ExrLayout& layout) {
    ImagePartInfo part;
    if (header.hasName())
        part.name = header.name();
    if (header.hasView())
        part.view = header.view();
    part.tiled = layout.tiled;
    part.dataX = layout.dataWindow.min.x;
    part.dataY = layout.dataWindow.min.y;
    part.dataWidth = static_cast<std::uint32_t>(layout.width);
    part.dataHeight = static_cast<std::uint32_t>(layout.height);
    part.displayX = layout.displayWindow.min.x;
    part.displayY = layout.displayWindow.min.y;
    part.displayWidth = static_cast<std::uint32_t>(layout.displayWidth);
    part.displayHeight = static_cast<std::uint32_t>(layout.displayHeight);
    part.alphaAssociation = layout.alphaAssociation;
    part.colorSpaceTag = layout.colorSpaceTag;
    for (auto it = header.channels().begin(); it != header.channels().end(); ++it) {
        part.channels.push_back({it.name(), sampleKind(it.channel().type)});
        const auto layer = std::string(render::channelLayerName(it.name()));
        if (std::ranges::find(part.layers, layer) == part.layers.end())
            part.layers.push_back(layer);
    }
    // A Cryptomatte rank layer (`<type>NN`, named by a `cryptomatte/<key>/name` attribute) carries
    // ID bits and coverage in its R/G/B/A channels: data, never a colour layer (#364).
    std::vector<std::string> cryptomatteTypes;
    for (auto attribute = header.begin(); attribute != header.end(); ++attribute) {
        const std::string_view name = attribute.name();
        if (!name.starts_with("cryptomatte/") || !name.ends_with("/name"))
            continue;
        if (const auto* text = dynamic_cast<const Imf::StringAttribute*>(&attribute.attribute()))
            cryptomatteTypes.push_back(text->value());
    }
    const auto isCryptomatteRank = [&](const std::string_view layer) {
        return layer.size() > 2 && std::isdigit(static_cast<unsigned char>(layer.back())) &&
               std::isdigit(static_cast<unsigned char>(layer[layer.size() - 2])) &&
               std::ranges::find(cryptomatteTypes, layer.substr(0, layer.size() - 2)) !=
                   cryptomatteTypes.end();
    };
    for (const auto& layer : part.layers) {
        if (isCryptomatteRank(layer))
            continue;
        ImageDiagnosticCode ignored = ImageDiagnosticCode::None;
        if (const auto selected = inspectHeader(header, layer, ignored); selected && selected->colour)
            part.colourLayers.push_back(layer);
    }
    return part;
}

[[nodiscard]] ImageProbe makeProbe(const ExrLayout& layout, const core::Sha256Digest digest) {
    auto probe = ImageProbe{static_cast<std::uint32_t>(layout.width),
                            static_cast<std::uint32_t>(layout.height),
                            layout.bitDepth,
                            digest,
                            ImageFormat::Exr,
                            ImageColorSpace::Linear,
                            layout.alphaAssociation,
                            layout.colorSpaceTag,
                            layout.interpretationAssumption};
    probe.dataOffsetX = static_cast<std::int64_t>(layout.dataWindow.min.x) -
                        static_cast<std::int64_t>(layout.displayWindow.min.x);
    probe.dataOffsetY = static_cast<std::int64_t>(layout.dataWindow.min.y) -
                        static_cast<std::int64_t>(layout.displayWindow.min.y);
    probe.frameWidth = static_cast<std::uint32_t>(layout.displayWidth);
    probe.frameHeight = static_cast<std::uint32_t>(layout.displayHeight);
    return probe;
}

// Every part is validated; the scalar probe fields describe the first part's root layer.
[[nodiscard]] ImageResult<ImageProbe> probeParts(const Imf::MultiPartInputFile& input,
                                                 const core::Sha256Digest digest,
                                                 const CancelImageWork& cancel) {
    if (const auto error = validateVersion(static_cast<std::uint32_t>(input.version())))
        return failure<ImageProbe>(*error, "OpenEXR version or flags are unsupported");
    if (input.parts() < 1)
        return failure<ImageProbe>(ImageDiagnosticCode::InvalidHeader, "OpenEXR has no parts");
    std::vector<ImagePartInfo> parts;
    std::optional<ExrLayout> first;
    for (int index = 0; index < input.parts(); ++index) {
        if (cancelled(cancel))
            return {{}, "Image probe cancelled", true, ImageDiagnosticCode::Cancelled};
        ImageDiagnosticCode error = ImageDiagnosticCode::InvalidHeader;
        const auto layout = inspectHeader(input.header(index), {}, error);
        if (!layout)
            return failure<ImageProbe>(error, "OpenEXR header is unsupported or invalid");
        parts.push_back(makePartInfo(input.header(index), *layout));
        if (index == 0)
            first = layout;
    }
    auto probe = makeProbe(*first, digest);
    probe.parts = std::move(parts);
    return {std::move(probe), {}};
}

[[nodiscard]] std::optional<int> findPartIndex(const Imf::MultiPartInputFile& input,
                                               const std::string_view name) {
    if (name.empty())
        return input.parts() > 0 ? std::optional<int>{0} : std::nullopt;
    for (int index = 0; index < input.parts(); ++index)
        if (input.header(index).hasName() && input.header(index).name() == name)
            return index;
    return std::nullopt;
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
        const Imf::MultiPartInputFile input(utf8Path(path).c_str(), 1);
        return probeParts(input, contentDigest, cancel);
    } catch (const std::bad_alloc&) {
        return failure<ImageProbe>(ImageDiagnosticCode::AllocationFailure,
                                   "OpenEXR probe allocation failed");
    } catch (...) {
        return failure<ImageProbe>(ImageDiagnosticCode::InvalidHeader,
                                   "OpenEXR header could not be read");
    }
}
static ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeExrInput(Imf::InputPart& input, const ExrLayout& selected,
               const ImageInterpretation& interpretation,
               std::shared_ptr<const color::CpuColorSpaceProcessor> processor,
               const CancelImageWork& cancel, const ImageProgress& progress,
               const std::size_t pixelBudget) {
    if (cancelled(cancel))
        return failure<std::shared_ptr<const render::Rgba32fImage>>(ImageDiagnosticCode::Cancelled,
                                                                    "Image decode cancelled");
    try {
        const ExrLayout* const layout = &selected;

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
        constexpr std::uint64_t kBandTargetBytes = std::uint64_t{1024} * 1024U;
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
        std::uint64_t repairedPixels = 0;
        std::vector<bool> repairedRow(static_cast<std::size_t>(width));
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
                insert(layout->colourNames[0].c_str(), band[0]);
            else {
                insert(layout->colourNames[0].c_str(), band[0]);
                insert(layout->colourNames[1].c_str(), band[1]);
                insert(layout->colourNames[2].c_str(), band[2]);
            }
            if (layout->hasAlpha)
                insert(layout->colourNames[3].c_str(), band[3]);
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
                    // Non-finite policy, before any colour conversion: a NaN/Inf component
                    // becomes zero and alpha is clamped to [0, 1].
                    bool repaired = false;
                    for (auto& component : target)
                        if (!std::isfinite(component)) {
                            component = 0.0F;
                            repaired = true;
                        }
                    if (target[3] < 0.0F || target[3] > 1.0F) {
                        target[3] = std::clamp(target[3], 0.0F, 1.0F);
                        repaired = true;
                    }
                    // Premultiplied RGB at zero alpha (additive emission) cannot survive the v1
                    // process image; it is dropped and reported.
                    if (sourcePremultiplied && target[3] == 0.0F &&
                        (target[0] != 0.0F || target[1] != 0.0F || target[2] != 0.0F))
                        repaired = true;
                    repairedRow[static_cast<std::size_t>(x)] = repaired;
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
                    auto sample = row[static_cast<std::size_t>(x)];
                    // The same policy after colour conversion, which can itself overflow. The
                    // frame keeps rendering; each repaired pixel is counted once.
                    bool repaired = repairedRow[static_cast<std::size_t>(x)];
                    for (auto& component : sample)
                        if (!std::isfinite(component)) {
                            component = 0.0F;
                            repaired = true;
                        }
                    if (sample[3] < 0.0F || sample[3] > 1.0F) {
                        sample[3] = std::clamp(sample[3], 0.0F, 1.0F);
                        repaired = true;
                    }
                    const auto red =
                        sourcePremultiplied && !convert ? sample[0] : sample[0] * sample[3];
                    const auto green =
                        sourcePremultiplied && !convert ? sample[1] : sample[1] * sample[3];
                    const auto blue =
                        sourcePremultiplied && !convert ? sample[2] : sample[2] * sample[3];
                    // The v1 process image cannot carry premultiplied RGB at zero alpha
                    // (additive emission); such a pixel becomes transparent and is reported.
                    if (sample[3] == 0.0F && (red != 0.0F || green != 0.0F || blue != 0.0F))
                        repaired = true;
                    auto pixel = render::Rgba32f::fromPremultiplied(red, green, blue, sample[3]);
                    if (!pixel) {
                        // A finite input can still overflow when premultiplied.
                        pixel = render::Rgba32f::fromPremultiplied(
                            std::isfinite(red) ? red : 0.0F, std::isfinite(green) ? green : 0.0F,
                            std::isfinite(blue) ? blue : 0.0F, sample[3]);
                        repaired = true;
                    }
                    if (!pixel)
                        return failure<std::shared_ptr<const render::Rgba32fImage>>(
                            ImageDiagnosticCode::DecodeFailed, "OpenEXR contains an invalid pixel");
                    if (repaired)
                        ++repairedPixels;
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
        ImageResult<std::shared_ptr<const render::Rgba32fImage>> decoded{
            std::make_shared<const render::Rgba32fImage>(std::move(*image.value())), {}};
        decoded.repairedPixels = repairedPixels;
        return decoded;
    } catch (const std::bad_alloc&) {
        return failure<std::shared_ptr<const render::Rgba32fImage>>(
            ImageDiagnosticCode::AllocationFailure, "OpenEXR decode allocation failed");
    } catch (...) {
        return failure<std::shared_ptr<const render::Rgba32fImage>>(
            ImageDiagnosticCode::Truncated, "OpenEXR pixel data could not be decoded");
    }
}

// Resolves the interpretation's part and colour layer, then decodes that layer's colour image.
static ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeExrParts(Imf::MultiPartInputFile& input, const ImageInterpretation& interpretation,
               std::shared_ptr<const color::CpuColorSpaceProcessor> processor,
               const CancelImageWork& cancel, const ImageProgress& progress,
               const std::size_t pixelBudget) {
    using Decoded = std::shared_ptr<const render::Rgba32fImage>;
    if (const auto error = validateVersion(static_cast<std::uint32_t>(input.version())))
        return failure<Decoded>(*error, "OpenEXR version or flags are unsupported");
    const auto index = findPartIndex(input, interpretation.part);
    if (!index)
        return failure<Decoded>(ImageDiagnosticCode::PartNotFound,
                                "The selected OpenEXR part is not present");
    ImageDiagnosticCode error = ImageDiagnosticCode::InvalidHeader;
    const auto layout = inspectHeader(input.header(*index), interpretation.layer, error);
    if (!layout)
        return failure<Decoded>(error, "OpenEXR header is unsupported or invalid");
    if (!interpretation.layer.empty() && !layout->layerFound)
        return failure<Decoded>(ImageDiagnosticCode::LayerNotFound,
                                "The selected OpenEXR layer is not present");
    if (!layout->colour)
        return failure<Decoded>(ImageDiagnosticCode::NoColourChannels,
                                "No colour layer — use Channels");
    Imf::InputPart part(input, *index);
    return decodeExrInput(part, *layout, interpretation, std::move(processor), cancel, progress,
                          pixelBudget);
}

ImageResult<ImageProbe> probeExr(const std::span<const std::byte> bytes,
                                 const core::Sha256Digest digest, const CancelImageWork& cancel) {
    if (cancelled(cancel))
        return {{}, "Image probe cancelled", true, ImageDiagnosticCode::Cancelled};
    try {
        if (bytes.size() < 8)
            return failure<ImageProbe>(ImageDiagnosticCode::Truncated,
                                       "OpenEXR revision version field is truncated");
        std::uint32_t encodedVersion = 0;
        for (std::size_t index = 0; index < 4; ++index)
            encodedVersion |= static_cast<std::uint32_t>(bytes[index + 4]) << (8U * index);
        if (const auto error = validateVersion(encodedVersion))
            return failure<ImageProbe>(*error, "OpenEXR version or flags are unsupported");
        RevisionStream stream(bytes);
        const Imf::MultiPartInputFile input(stream, 1);
        return probeParts(input, digest, cancel);
    } catch (const std::bad_alloc&) {
        return failure<ImageProbe>(ImageDiagnosticCode::AllocationFailure,
                                   "OpenEXR probe allocation failed");
    } catch (...) {
        return failure<ImageProbe>(ImageDiagnosticCode::InvalidHeader,
                                   "OpenEXR revision header could not be read");
    }
}

ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeExr(const std::filesystem::path& path, const ImageInterpretation& interpretation,
          std::shared_ptr<const color::CpuColorSpaceProcessor> processor,
          const CancelImageWork& cancel, const ImageProgress& progress, const std::size_t budget) {
    try {
        Imf::MultiPartInputFile input(utf8Path(path).c_str(), 1);
        return decodeExrParts(input, interpretation, std::move(processor), cancel, progress,
                              budget);
    } catch (...) {
        return failure<std::shared_ptr<const render::Rgba32fImage>>(
            ImageDiagnosticCode::Truncated, "OpenEXR pixel data could not be decoded");
    }
}

ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeExr(const std::span<const std::byte> bytes, const ImageInterpretation& interpretation,
          std::shared_ptr<const color::CpuColorSpaceProcessor> processor,
          const CancelImageWork& cancel, const ImageProgress& progress, const std::size_t budget) {
    try {
        RevisionStream stream(bytes);
        Imf::MultiPartInputFile input(stream, 1);
        return decodeExrParts(input, interpretation, std::move(processor), cancel, progress,
                              budget);
    } catch (...) {
        return failure<std::shared_ptr<const render::Rgba32fImage>>(
            ImageDiagnosticCode::Truncated, "OpenEXR revision pixel data could not be decoded");
    }
}

ImageResult<std::shared_ptr<const render::ChannelSetImage>>
decodeExrChannels(const std::span<const std::byte> bytes, const ImageChannelSelection& selection,
                  const CancelImageWork& cancel, const ImageProgress& progress,
                  const std::size_t budget) {
    using Decoded = std::shared_ptr<const render::ChannelSetImage>;
    if (cancelled(cancel))
        return {{}, "Image decode cancelled", true, ImageDiagnosticCode::Cancelled};
    try {
        RevisionStream stream(bytes);
        Imf::MultiPartInputFile input(stream, 1);
        if (const auto error = validateVersion(static_cast<std::uint32_t>(input.version())))
            return failure<Decoded>(*error, "OpenEXR version or flags are unsupported");
        const auto index = findPartIndex(input, selection.part);
        if (!index)
            return failure<Decoded>(ImageDiagnosticCode::PartNotFound,
                                    "The selected OpenEXR part is not present");
        const auto& header = input.header(*index);
        ImageDiagnosticCode error = ImageDiagnosticCode::InvalidHeader;
        const auto layout = inspectHeader(header, {}, error);
        if (!layout)
            return failure<Decoded>(error, "OpenEXR header is unsupported or invalid");

        struct Selected final {
            std::string name;
            Imf::PixelType type;
        };
        std::vector<Selected> selected;
        if (selection.channels.empty()) {
            for (auto it = header.channels().begin(); it != header.channels().end(); ++it)
                selected.push_back({it.name(), it.channel().type});
        } else {
            for (const auto& name : selection.channels) {
                const auto* channel = header.channels().findChannel(name);
                if (channel == nullptr)
                    return failure<Decoded>(ImageDiagnosticCode::ChannelNotFound,
                                            "A selected OpenEXR channel is not present");
                if (std::ranges::any_of(selected,
                                        [&](const Selected& item) { return item.name == name; }))
                    return failure<Decoded>(ImageDiagnosticCode::UnsupportedChannels,
                                            "A channel is selected more than once");
                selected.push_back({name, channel->type});
            }
        }

        // Admission: every selected plane (channels x pixels x 4), the part's string metadata and
        // one bounded band of scratch rows.
        const auto width = layout->width;
        const auto height = layout->height;
        const auto count = static_cast<std::uint64_t>(selected.size());
        render::ChannelMetadata metadata;
        std::uint64_t metadataBytes = 0;
        for (auto it = header.begin(); it != header.end(); ++it)
            if (const auto* text = dynamic_cast<const Imf::StringAttribute*>(&it.attribute())) {
                metadataBytes += std::strlen(it.name()) + text->value().size();
                metadata.emplace_back(it.name(), text->value());
            }
        std::uint64_t pixels = 0;
        std::uint64_t planeBytes = 0;
        std::uint64_t bytesPerRow = 0;
        if (!checkedSizeProduct(width, height, pixels) ||
            !checkedSizeProduct(pixels, count * sizeof(float), planeBytes) ||
            !checkedSizeProduct(width, count * sizeof(float), bytesPerRow) ||
            planeBytes > std::numeric_limits<std::uint64_t>::max() - metadataBytes)
            return failure<Decoded>(ImageDiagnosticCode::PixelBudgetExceeded,
                                    "Decoded channel set exceeds the pixel working-set budget");
        constexpr std::uint64_t kBandTargetBytes = std::uint64_t{1024} * 1024U;
        std::uint64_t chunkRows =
            layout->tiled ? std::max<std::uint64_t>(1U, layout->tileHeight)
                          : std::max<std::uint64_t>(1U, kBandTargetBytes /
                                                            std::max<std::uint64_t>(bytesPerRow, 1U));
        chunkRows = std::min(chunkRows, height);
        std::uint64_t bandBytes = 0;
        if (!checkedSizeProduct(chunkRows, bytesPerRow, bandBytes) ||
            !decodeWorkingSetFits(planeBytes + metadataBytes, bandBytes, 0U, budget))
            return failure<Decoded>(ImageDiagnosticCode::PixelBudgetExceeded,
                                    "Decoded channel set exceeds the pixel working-set budget");

        std::vector<std::vector<float>> planes(selected.size());
        for (auto& plane : planes)
            plane.assign(static_cast<std::size_t>(pixels), 0.0F);
        const auto bandSamples = static_cast<std::size_t>(chunkRows * width);
        std::vector<float> band(bandSamples * selected.size());
        const auto rowStride = static_cast<std::size_t>(width) * sizeof(float);
        Imf::InputPart part(input, *index);
        std::uint64_t completed = 0;
        if (progress)
            progress(0, height);
        while (completed < height) {
            if (cancelled(cancel))
                return {{}, "Image decode cancelled", true, ImageDiagnosticCode::Cancelled};
            const auto first = layout->tiled ? (completed / chunkRows) * chunkRows : completed;
            const auto rows = std::min(chunkRows, height - first);
            const auto firstY = static_cast<std::int64_t>(layout->dataWindow.min.y) +
                                static_cast<std::int64_t>(first);
            const auto origin = Imath::V2i(layout->dataWindow.min.x, static_cast<int>(firstY));
            Imf::FrameBuffer frameBuffer;
            for (std::size_t channel = 0; channel < selected.size(); ++channel) {
                // Half and float widen to Float32; uint keeps its 32-bit pattern in the lane.
                const auto readType = selected[channel].type == Imf::UINT ? Imf::UINT : Imf::FLOAT;
                frameBuffer.insert(selected[channel].name,
                                   Imf::Slice::Make(readType, band.data() + channel * bandSamples,
                                                    origin, static_cast<std::int64_t>(width),
                                                    static_cast<std::int64_t>(rows),
                                                    sizeof(float), rowStride));
            }
            part.setFrameBuffer(frameBuffer);
            part.readPixels(static_cast<int>(firstY), static_cast<int>(firstY) +
                                                          static_cast<int>(rows) - 1);
            for (std::size_t channel = 0; channel < selected.size(); ++channel)
                std::memcpy(planes[channel].data() + static_cast<std::size_t>(first * width),
                            band.data() + channel * bandSamples,
                            static_cast<std::size_t>(rows * width) * sizeof(float));
            completed = first + rows;
            if (progress)
                progress(completed, height);
        }
        if (!part.isComplete())
            return failure<Decoded>(ImageDiagnosticCode::Truncated,
                                    "OpenEXR pixel data is truncated");

        std::vector<render::ChannelDescriptor> descriptors;
        descriptors.reserve(selected.size());
        for (const auto& item : selected)
            descriptors.push_back(render::describeChannel(item.name, sampleKind(item.type)));
        const auto dataWindow = render::ImageWindow::create(
            layout->dataWindow.min.x, layout->dataWindow.min.y, width, height);
        const auto displayWindow =
            render::ImageWindow::create(layout->displayWindow.min.x, layout->displayWindow.min.y,
                                        layout->displayWidth, layout->displayHeight);
        if (!dataWindow || !displayWindow)
            return failure<Decoded>(ImageDiagnosticCode::InvalidDataWindow,
                                    "OpenEXR image window is invalid");
        auto created = render::ChannelSetImage::create(
            *dataWindow.value(), *displayWindow.value(), core::PixelAspectRatio::square(),
            std::move(descriptors), std::move(planes), std::move(metadata),
            std::numeric_limits<std::size_t>::max(),
            selection.inputColorSpaceId.empty() ? layout->colorSpaceTag
                                                : selection.inputColorSpaceId);
        if (!created)
            return failure<Decoded>(ImageDiagnosticCode::DecodeFailed,
                                    "Channel set validation failed");
        return {std::make_shared<const render::ChannelSetImage>(std::move(*created.value())), {}};
    } catch (const std::bad_alloc&) {
        return failure<Decoded>(ImageDiagnosticCode::AllocationFailure,
                                "OpenEXR channel decode allocation failed");
    } catch (...) {
        return failure<Decoded>(ImageDiagnosticCode::Truncated,
                                "OpenEXR channel data could not be decoded");
    }
}

} // namespace bloom::media::detail
