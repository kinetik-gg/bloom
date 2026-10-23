#include <algorithm>
#include <array>
#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/color/video_colour.hpp>
#include <bloom/media/video/colour.hpp>
#include <cmath>
#include <ranges>
#include <string>
#include <utility>
#include <vector>
namespace bloom::media::video {
namespace {
using namespace media::provider;

[[nodiscard]] std::string_view
colourSpaceName(const bloom::color::ResolvedBloomNeutralConfig& config, const std::string_view id) {
    const auto found = std::ranges::find(config.colorSpaces(), id, &color::OcioColorSpaceInfo::id);
    return found == config.colorSpaces().end() ? id : found->id;
}

[[nodiscard]] VideoInputColorSpaceResolution
selected(const bloom::color::ResolvedBloomNeutralConfig& config, std::string id,
         const bool automatic, std::string warning = {}) {
    VideoInputColorSpaceResolution result{std::move(id), {}, std::move(warning), false, automatic};
    result.name = std::string(colourSpaceName(config, result.id));
    return result;
}

// H.273 code points. Unspecified (2 for primaries/transfer/matrix, 0 for range) and any value a
// stream did not carry are "missing"; Bloom never infers them from resolution or codec.
constexpr std::int32_t kMatrixBt709 = 1, kMatrixBt470bg = 5, kMatrixSmpte170m = 6,
                       kMatrixBt2020Ncl = 9, kMatrixBt2020Cl = 10;
constexpr std::int32_t kTransferPq = 16, kTransferHlg = 18;
constexpr std::int32_t kPrimariesBt2020 = 9;
// The interpretation that resolves a refused stream, named in every diagnostic.
constexpr std::string_view kOverride =
    "; choose an input colour space for the asset (Assets, Input Colour...) or its Video node";
[[nodiscard]] bool knownMatrix(const std::int32_t matrix) {
    return matrix == kMatrixBt709 || matrix == kMatrixBt470bg || matrix == kMatrixSmpte170m ||
           matrix == kMatrixBt2020Ncl;
}
[[nodiscard]] bool knownRange(const std::int32_t range) { return range == 1 || range == 2; }
// Names every tag a conversion needs but the stream lacks, for diagnostics.
[[nodiscard]] std::string missingTags(const ColourTags& tags, const bool needTransfer) {
    std::vector<std::string_view> missing;
    if (tags.primaries <= 0 || tags.primaries == 2)
        missing.emplace_back("colour primaries");
    if (needTransfer && (tags.transfer <= 0 || tags.transfer == 2))
        missing.emplace_back("transfer characteristics");
    if (!knownMatrix(tags.matrix) && tags.matrix != kMatrixBt2020Cl)
        missing.emplace_back("matrix coefficients");
    if (!knownRange(tags.range))
        missing.emplace_back("range");
    std::string result;
    for (std::size_t i = 0; i < missing.size(); ++i)
        result += std::string(i == 0                    ? ""
                              : i + 1 == missing.size() ? " and "
                                                        : ", ") +
                  std::string(missing[i]);
    return result;
}
// Kr/Kb of the YUV matrix used for the conversion. A config-managed (explicit) interpretation of
// a stream without a matrix tag takes the matrix of the chosen space's gamut family.
[[nodiscard]] std::int32_t effectiveMatrix(const ColourTags& tags, const std::string_view inputId) {
    if (knownMatrix(tags.matrix))
        return tags.matrix;
    if (tags.primaries == kPrimariesBt2020 || inputId.find("2020") != std::string_view::npos ||
        inputId.find("2100") != std::string_view::npos)
        return kMatrixBt2020Ncl;
    return kMatrixBt709;
}

float component(const CpuPlane& plane, std::uint32_t x, std::uint32_t y, bool wide) {
    const auto offset =
        static_cast<std::size_t>(y) * plane.stride + static_cast<std::size_t>(x) * (wide ? 2U : 1U);
    auto value = std::to_integer<unsigned>(plane.bytes[offset]);
    if (wide)
        value |= std::to_integer<unsigned>(plane.bytes[offset + 1]) << 8U;
    return static_cast<float>(value);
}

// Filtered 4:2:0 reconstruction for left-sited chroma (the H.273 default the worker guarantees for
// this layout): horizontally co-sited with even luma columns, vertically between luma rows.
float chroma(const CpuPlane& plane, const std::uint32_t x, const std::uint32_t y) {
    const auto at = [&](std::int64_t cx, std::int64_t cy) {
        cx = std::clamp<std::int64_t>(cx, 0, plane.width - 1);
        cy = std::clamp<std::int64_t>(cy, 0, plane.height - 1);
        return component(plane, static_cast<std::uint32_t>(cx), static_cast<std::uint32_t>(cy),
                         false);
    };
    // Chroma-grid coordinates: horizontal x/2, vertical y/2 - 1/4.
    const auto cx = static_cast<std::int64_t>(x / 2U);
    const float fx = (x & 1U) != 0 ? 0.5F : 0.0F;
    const auto vy = static_cast<std::int64_t>(4 * static_cast<std::int64_t>(y) - 1);
    const auto cy = vy < 0 ? std::int64_t{-1} : vy / 8;
    const float fy = static_cast<float>((vy - cy * 8)) / 8.0F;
    const auto top = at(cx, cy) * (1.0F - fx) + at(cx + 1, cy) * fx;
    const auto bottom = at(cx, cy + 1) * (1.0F - fx) + at(cx + 1, cy + 1) * fx;
    return top * (1.0F - fy) + bottom * fy;
}

[[nodiscard]] Result<render::Rgba32fImage>
convertVideo(const FrameProduct& frame, const std::uint32_t overrideTransfer,
             const std::string_view inputColorSpaceId,
             const std::shared_ptr<const bloom::color::CpuColorSpaceProcessor>& processor,
             render::Rgba32fImageDescriptor composition, const double horizontalScale,
             const double verticalScale, const std::size_t byteBudget,
             const std::function<bool()>& cancel, const bool configManaged,
             const bool applyProcessor) {
    // The frame was digest-verified when it crossed the worker boundary and is immutable since.
    if (!validLayout(frame) || overrideTransfer > 3 || !std::isfinite(horizontalScale) ||
        !std::isfinite(verticalScale) || horizontalScale <= 0 || horizontalScale > 1 ||
        verticalScale <= 0 || verticalScale > 1)
        return Unavailable{Error::InvalidValue, "Invalid video colour request"};
    if (configManaged && applyProcessor && processor == nullptr)
        return Unavailable{Error::InvalidValue,
                           "A config-managed video conversion needs a colour processor"};
    auto tags = frame.colour;
    if (tags.matrix == kMatrixBt2020Cl)
        return Unavailable{Error::Unavailable,
                           "BT.2020 constant-luminance video (matrix coefficients 10) is not "
                           "supported; transcode it to the non-constant-luminance matrix"};
    const auto transfer = configManaged           ? 8
                          : overrideTransfer == 1 ? 13
                          : overrideTransfer >= 2 ? 8
                                                  : tags.transfer;
    if (configManaged) {
        // The config input space supplies primaries and transfer (including PQ and HLG), so only
        // the YUV matrix and range come from the stream. An explicit interpretation of an
        // untagged stream uses the chosen space's matrix family and video (limited) range.
        if (inputColorSpaceId.empty())
            return Unavailable{Error::InvalidValue,
                               "A config-managed video input needs a colour-space id"};
        tags.matrix = effectiveMatrix(tags, inputColorSpaceId);
        if (!knownRange(tags.range))
            tags.range = 1;
    } else if (tags.transfer == kTransferPq || tags.transfer == kTransferHlg ||
               tags.primaries == kPrimariesBt2020 || tags.matrix == kMatrixBt2020Ncl) {
        // Legacy transfer overrides never reinterpret wide-gamut or HDR streams.
        return Unavailable{Error::Unavailable,
                           "Rec.2020, PQ and HLG video is interpreted only through the colour "
                           "config" +
                               std::string(kOverride)};
    } else if (tags.primaries != 1 || tags.matrix != kMatrixBt709 || !knownRange(tags.range) ||
               (transfer != 1 && transfer != 13 && transfer != 8)) {
        const auto missing = missingTags(tags, overrideTransfer == 0);
        return Unavailable{
            Error::Unavailable,
            missing.empty()
                ? "This Rec.2020, PQ, HLG or non-Rec.709 video is interpreted only through the "
                  "colour config" +
                      std::string(kOverride)
                : "Video stream has no " + missing + " tag" + std::string(kOverride)};
    }
    const bool wide = frame.format == PixelFormat::Yuva444p16;
    if (!wide && frame.format != PixelFormat::Yuv420p8)
        return Unavailable{Error::Unavailable, "Video pixel layout is not qualified"};
    const auto width = frame.planes[0].width, height = frame.planes[0].height;
    const auto w = static_cast<std::uint32_t>(std::max(1.0, std::ceil(width * horizontalScale)));
    const auto h = static_cast<std::uint32_t>(std::max(1.0, std::ceil(height * verticalScale)));
    const auto window = render::ImageWindow::create(0, 0, w, h);
    if (!window)
        return Unavailable{Error::Oversized, "Video output extent exceeds limits"};
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *window.value(), composition.displayWindow(), composition.pixelAspect());
    if (!descriptor)
        return Unavailable{Error::InvalidValue, "Video descriptor is invalid"};
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), byteBudget);
    if (!builder)
        return Unavailable{Error::Oversized, "Video frame exceeds the image budget"};
    for (std::uint32_t y = 0; y < h; ++y) {
        if (cancel && cancel())
            return Unavailable{Error::Cancelled, "Video colour conversion cancelled"};
        auto row = builder.value()->row(y);
        const auto sy = std::min(height - 1, static_cast<std::uint32_t>(y / verticalScale));
        std::vector<std::array<float, 4>> convertedRow;
        if (configManaged)
            convertedRow.resize(w);
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto sx = std::min(width - 1, static_cast<std::uint32_t>(x / horizontalScale));
            const auto yCode = component(frame.planes[0], sx, sy, wide);
            const auto uCode =
                wide ? component(frame.planes[1], sx, sy, true) : chroma(frame.planes[1], sx, sy);
            const auto vCode =
                wide ? component(frame.planes[2], sx, sy, true) : chroma(frame.planes[2], sx, sy);
            const bool fullWide = wide && tags.range == 2;
            const auto divisor = wide ? 256.0F : 1.0F;
            const auto luma = fullWide ? yCode * (255.0F / 65535.0F) : yCode / divisor;
            const auto cb =
                fullWide ? 128.0F + (uCode - 32768.0F) * (255.0F / 65535.0F) : uCode / divisor;
            const auto cr =
                fullWide ? 128.0F + (vCode - 32768.0F) * (255.0F / 65535.0F) : vCode / divisor;
            const auto rgb =
                color::yuvToSceneLinear(luma, cb, cr, tags.range == 1, transfer, tags.matrix);
            const auto alpha = wide ? component(frame.planes[3], sx, sy, true) / 65535.0F : 1.0F;
            if (configManaged) {
                convertedRow[x] = {rgb[0], rgb[1], rgb[2], alpha};
            } else {
                const auto pixel = render::Rgba32f::fromPremultiplied(
                    rgb[0] * alpha, rgb[1] * alpha, rgb[2] * alpha, alpha);
                if (!pixel)
                    return Unavailable{Error::InvalidValue, "Video pixel is invalid"};
                (*row.value())[x] = *pixel.value();
            }
        }
        if (configManaged && applyProcessor && !processor->apply(convertedRow))
            return Unavailable{Error::Unavailable, "Input colour conversion failed"};
        if (configManaged) {
            for (std::uint32_t x = 0; x < w; ++x) {
                const auto& sample = convertedRow[x];
                const auto pixel = render::Rgba32f::fromPremultiplied(
                    sample[0] * sample[3], sample[1] * sample[3], sample[2] * sample[3], sample[3]);
                if (!pixel)
                    return Unavailable{Error::InvalidValue, "Video pixel is invalid"};
                (*row.value())[x] = *pixel.value();
            }
        }
    }
    auto image = std::move(*builder.value()).freeze();
    if (!image)
        return Unavailable{Error::InvalidValue, "Video produced non-finite pixels"};
    return std::move(*image.value());
}
} // namespace

VideoInputColorSpaceResolution resolveVideoInputColorSpace(
    const bloom::color::ResolvedBloomNeutralConfig& config, const media::provider::ColourTags& tags,
    const std::uint32_t legacyColorSpace, const std::string_view explicitId) {
    if (!explicitId.empty())
        return selected(config, std::string(explicitId), false);
    if (legacyColorSpace == 3)
        return VideoInputColorSpaceResolution{"", "", "", true, false};
    if (legacyColorSpace == 2)
        return selected(config, std::string(config.processColorSpaceId()), false);
    if (legacyColorSpace == 1)
        return selected(config, std::string(config.sRgbTextureColorSpaceId()), false);
    const auto refuse = [](std::string warning) {
        return VideoInputColorSpaceResolution{"", "", std::move(warning), false, true};
    };
    if (tags.matrix == kMatrixBt2020Cl)
        return refuse("BT.2020 constant-luminance video (matrix coefficients 10) is not supported");
    if (tags.transfer == kTransferPq || tags.transfer == kTransferHlg) {
        const bool pq = tags.transfer == kTransferPq;
        // Rec.2100 needs its matrix and range to decode YUV; the config supplies the curve.
        const auto missing = missingTags(tags, false);
        if (!missing.empty())
            return refuse("Rec.2100 video has no " + missing + " tag" + std::string(kOverride));
        auto space =
            std::string(pq ? config.rec2100PqColorSpaceId() : config.rec2100HlgColorSpaceId());
        if (space.empty())
            return refuse(std::string("The OCIO config has no Rec.2100 ") + (pq ? "PQ" : "HLG") +
                          " colour space" + std::string(kOverride));
        return selected(config, std::move(space), true);
    }
    if (const auto missing = missingTags(tags, true); !missing.empty())
        return refuse("Video stream has no " + missing + " tag" + std::string(kOverride));
    if (tags.primaries != 1 || tags.matrix != kMatrixBt709)
        return refuse("Rec.2020 or other non-Rec.709 video without a PQ or HLG transfer has no "
                      "automatic config space" +
                      std::string(kOverride));
    if (tags.transfer == 1) {
        if (config.rec709VideoColorSpaceId().empty())
            return VideoInputColorSpaceResolution{
                "", "", "The OCIO config has no Rec.709 camera/video colour space", false, true};
        return selected(config, std::string(config.rec709VideoColorSpaceId()), true);
    }
    if (tags.transfer == 13)
        return selected(config, std::string(config.sRgbTextureColorSpaceId()), true);
    if (tags.transfer == 8)
        return selected(config, std::string(config.processColorSpaceId()), true);
    return VideoInputColorSpaceResolution{
        "", "", "Video transfer tags are unsupported; choose an explicit config colour space",
        false, true};
}

media::provider::Result<render::Rgba32fImage>
videoToSceneLinear(const FrameProduct& frame, std::uint32_t overrideTransfer,
                   render::Rgba32fImageDescriptor composition, double horizontalScale,
                   double verticalScale, std::size_t byteBudget,
                   const std::function<bool()>& cancel) {
    return convertVideo(frame, overrideTransfer, {}, {}, composition, horizontalScale,
                        verticalScale, byteBudget, cancel, /*configManaged=*/false,
                        /*applyProcessor=*/false);
}

media::provider::Result<render::Rgba32fImage>
videoToSceneLinear(const FrameProduct& frame, const std::uint32_t overrideTransfer,
                   const std::string_view inputColorSpaceId,
                   const std::shared_ptr<const bloom::color::CpuColorSpaceProcessor>& processor,
                   render::Rgba32fImageDescriptor composition, const double horizontalScale,
                   const double verticalScale, const std::size_t byteBudget,
                   const std::function<bool()>& cancel) {
    const bool configManaged = processor != nullptr;
    return convertVideo(frame, overrideTransfer, inputColorSpaceId, processor, composition,
                        horizontalScale, verticalScale, byteBudget, cancel, configManaged,
                        /*applyProcessor=*/configManaged);
}

media::provider::Result<render::Rgba32fImage>
videoToInputColorSpace(const FrameProduct& frame, const std::uint32_t overrideTransfer,
                       const std::string_view inputColorSpaceId,
                       render::Rgba32fImageDescriptor composition, const double horizontalScale,
                       const double verticalScale, const std::size_t byteBudget,
                       const std::function<bool()>& cancel) {
    return convertVideo(frame, overrideTransfer, inputColorSpaceId, {}, composition,
                        horizontalScale, verticalScale, byteBudget, cancel,
                        /*configManaged=*/!inputColorSpaceId.empty(),
                        /*applyProcessor=*/false);
}
} // namespace bloom::media::video
