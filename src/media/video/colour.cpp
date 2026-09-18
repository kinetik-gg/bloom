#include <algorithm>
#include <bloom/color/video_colour.hpp>
#include <bloom/media/video/colour.hpp>
#include <cmath>
namespace bloom::media::video {
namespace {
using namespace media::provider;
float component(const CpuPlane& plane, std::uint32_t x, std::uint32_t y, bool wide) {
    const auto offset =
        static_cast<std::size_t>(y) * plane.stride + static_cast<std::size_t>(x) * (wide ? 2U : 1U);
    auto value = std::to_integer<unsigned>(plane.bytes[offset]);
    if (wide)
        value |= std::to_integer<unsigned>(plane.bytes[offset + 1]) << 8U;
    return static_cast<float>(value);
}
} // namespace
media::provider::Result<render::Rgba32fImage>
videoToSceneLinear(const FrameProduct& frame, std::uint32_t overrideTransfer,
                   render::Rgba32fImageDescriptor composition, double horizontalScale,
                   double verticalScale, std::size_t byteBudget,
                   const std::function<bool()>& cancel) {
    if (!valid(frame) || overrideTransfer > 3 || !std::isfinite(horizontalScale) ||
        !std::isfinite(verticalScale) || horizontalScale <= 0 || horizontalScale > 1 ||
        verticalScale <= 0 || verticalScale > 1)
        return Unavailable{Error::InvalidValue, "Invalid video colour request"};
    const auto& tags = frame.colour;
    if (tags.primaries == 9 || tags.transfer == 16 || tags.transfer == 18 || tags.matrix == 9 ||
        tags.matrix == 10)
        return Unavailable{
            Error::Unavailable,
            "Rec.2020, HLG and PQ video are not supported by this preview colour path"};
    const auto transfer = overrideTransfer == 1 ? 13 : overrideTransfer >= 2 ? 8 : tags.transfer;
    if (tags.primaries != 1 || tags.matrix != 1 || (tags.range != 1 && tags.range != 2) ||
        (transfer != 1 && transfer != 13 && transfer != 8))
        return Unavailable{
            Error::Unavailable,
            "Video requires explicit Rec.709 matrix, primaries, range and supported transfer tags"};
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
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto sx = std::min(width - 1, static_cast<std::uint32_t>(x / horizontalScale));
            const auto yCode = component(frame.planes[0], sx, sy, wide);
            const auto uCode =
                component(frame.planes[1], wide ? sx : sx / 2, wide ? sy : sy / 2, wide);
            const auto vCode =
                component(frame.planes[2], wide ? sx : sx / 2, wide ? sy : sy / 2, wide);
            const bool fullWide = wide && tags.range == 2;
            const auto divisor = wide ? 256.0F : 1.0F;
            const auto luma = fullWide ? yCode * (255.0F / 65535.0F) : yCode / divisor;
            const auto cb =
                fullWide ? 128.0F + (uCode - 32768.0F) * (255.0F / 65535.0F) : uCode / divisor;
            const auto cr =
                fullWide ? 128.0F + (vCode - 32768.0F) * (255.0F / 65535.0F) : vCode / divisor;
            const auto rgb = color::rec709YuvToSceneLinear(luma, cb, cr, tags.range == 1, transfer);
            const auto alpha = wide ? component(frame.planes[3], sx, sy, true) / 65535.0F : 1.0F;
            const auto pixel = render::Rgba32f::fromPremultiplied(rgb[0] * alpha, rgb[1] * alpha,
                                                                  rgb[2] * alpha, alpha);
            if (!pixel)
                return Unavailable{Error::InvalidValue, "Video pixel is invalid"};
            (*row.value())[x] = *pixel.value();
        }
    }
    auto image = std::move(*builder.value()).freeze();
    if (!image)
        return Unavailable{Error::InvalidValue, "Video produced non-finite pixels"};
    return std::move(*image.value());
}
} // namespace bloom::media::video
