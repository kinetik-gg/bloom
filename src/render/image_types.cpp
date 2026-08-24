#include <bloom/render/image_types.hpp>

#include <cmath>
#include <limits>
#include <numeric>

namespace {

using bloom::render::ImageError;
using bloom::render::ImageErrorCode;
using bloom::render::ImageExtent;
using bloom::render::ImageResult;
using bloom::render::PackedImageLayout;

[[nodiscard]] ImageResult<PackedImageLayout>
checkedPackedLayout(const ImageExtent extent, const std::size_t bytesPerPixel) noexcept {
    const auto width = static_cast<std::size_t>(extent.width());
    const auto height = static_cast<std::size_t>(extent.height());
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();

    if (width > maximum / bytesPerPixel) {
        return ImageResult<PackedImageLayout>::failure(
            ImageError::codeOnly(ImageErrorCode::ArithmeticOverflow));
    }
    const auto rowStrideBytes = width * bytesPerPixel;
    if (height > maximum / width) {
        return ImageResult<PackedImageLayout>::failure(
            ImageError::codeOnly(ImageErrorCode::ArithmeticOverflow));
    }
    const auto pixelCount = width * height;
    if (pixelCount > maximum / bytesPerPixel) {
        return ImageResult<PackedImageLayout>::failure(
            ImageError::codeOnly(ImageErrorCode::ArithmeticOverflow));
    }
    return ImageResult<PackedImageLayout>::success(
        PackedImageLayout{pixelCount, rowStrideBytes, pixelCount * bytesPerPixel});
}

[[nodiscard]] bool additionOverflows(const std::int64_t origin,
                                     const std::uint32_t extent) noexcept {
    return origin > std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(extent);
}

} // namespace

namespace bloom::render {

ImageResult<ImageExtent> ImageExtent::create(const std::uint64_t width,
                                             const std::uint64_t height) noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    if (width == 0 || height == 0 || width > maximum || height > maximum) {
        return ImageResult<ImageExtent>::failure(
            ImageError::codeOnly(ImageErrorCode::InvalidExtent));
    }
    return ImageResult<ImageExtent>::success(
        ImageExtent(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)));
}

ImageResult<ImageWindow> ImageWindow::create(const std::int64_t originX, const std::int64_t originY,
                                             const std::uint64_t width,
                                             const std::uint64_t height) noexcept {
    const auto extentResult = ImageExtent::create(width, height);
    if (!extentResult) {
        return ImageResult<ImageWindow>::failure(
            ImageError::codeOnly(ImageErrorCode::InvalidWindow));
    }
    const auto extent = *extentResult.value();
    if (additionOverflows(originX, extent.width()) || additionOverflows(originY, extent.height())) {
        return ImageResult<ImageWindow>::failure(
            ImageError::codeOnly(ImageErrorCode::ArithmeticOverflow));
    }
    return ImageResult<ImageWindow>::success(ImageWindow(originX, originY, extent));
}

ImageResult<PixelAspectRatio> PixelAspectRatio::create(const std::uint64_t numerator,
                                                       const std::uint64_t denominator) noexcept {
    if (numerator == 0 || denominator == 0) {
        return ImageResult<PixelAspectRatio>::failure(
            ImageError::codeOnly(ImageErrorCode::InvalidPixelAspect));
    }
    const auto divisor = std::gcd(numerator, denominator);
    const auto reducedNumerator = numerator / divisor;
    const auto reducedDenominator = denominator / divisor;
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    if (reducedNumerator > maximum || reducedDenominator > maximum) {
        return ImageResult<PixelAspectRatio>::failure(
            ImageError::codeOnly(ImageErrorCode::InvalidPixelAspect));
    }
    return ImageResult<PixelAspectRatio>::success(
        PixelAspectRatio(static_cast<std::uint32_t>(reducedNumerator),
                         static_cast<std::uint32_t>(reducedDenominator)));
}

ImageResult<Rgba32f> Rgba32f::fromPremultiplied(const float red, const float green,
                                                const float blue, const float alpha) noexcept {
    if (!std::isfinite(red) || !std::isfinite(green) || !std::isfinite(blue) ||
        !std::isfinite(alpha) || alpha < 0.0F || alpha > 1.0F) {
        return ImageResult<Rgba32f>::failure(ImageError::codeOnly(ImageErrorCode::InvalidPixel));
    }
    if (alpha == 0.0F) {
        return ImageResult<Rgba32f>::success(transparent());
    }
    return ImageResult<Rgba32f>::success(Rgba32f(red, green, blue, alpha));
}

ImageResult<PackedImageLayout> checkedRgba32fLayout(const ImageExtent extent) noexcept {
    return checkedPackedLayout(extent, sizeof(Rgba32f));
}

ImageResult<PackedImageLayout> checkedRgba8Layout(const ImageExtent extent) noexcept {
    return checkedPackedLayout(extent, sizeof(Rgba8));
}

} // namespace bloom::render
