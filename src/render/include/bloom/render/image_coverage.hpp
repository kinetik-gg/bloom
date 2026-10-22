#pragma once

#include <bloom/render/image_types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace bloom::render {

// Flat RGBA matte extraction in scene-linear light. RGB selectors read straight colour;
// transparent pixels have zero coverage. The result is always a finite unit coverage,
// including for negative and HDR source colour. These values also define the resident
// GPU kernel's channel selector and are deliberately independent of UI labels.
enum class ImageCoverageChannel : std::uint8_t { Alpha, Red, Green, Blue, Luma };

[[nodiscard]] inline float imageCoverage(const Rgba32f pixel,
                                         const ImageCoverageChannel channel) noexcept {
    if (channel == ImageCoverageChannel::Alpha)
        return pixel.alpha();
    if (pixel.alpha() <= 0.0F)
        return 0.0F;
    const double inverseAlpha = 1.0 / static_cast<double>(pixel.alpha());
    double value = 0.0;
    switch (channel) {
    case ImageCoverageChannel::Red:
        value = static_cast<double>(pixel.red()) * inverseAlpha;
        break;
    case ImageCoverageChannel::Green:
        value = static_cast<double>(pixel.green()) * inverseAlpha;
        break;
    case ImageCoverageChannel::Blue:
        value = static_cast<double>(pixel.blue()) * inverseAlpha;
        break;
    case ImageCoverageChannel::Luma:
        value = (0.2126 * static_cast<double>(pixel.red()) +
                 0.7152 * static_cast<double>(pixel.green()) +
                 0.0722 * static_cast<double>(pixel.blue())) * inverseAlpha;
        break;
    case ImageCoverageChannel::Alpha:
        return pixel.alpha();
    }
    return static_cast<float>(std::clamp(value, 0.0, 1.0));
}

// All operations use exact composition-space integer pixel centres. A matte tap outside its
// data window is transparent black and thus has zero coverage. The caller performs the window
// lookup; this helper never guesses alignment from array indices or resamples a matte.
[[nodiscard]] inline ImageResult<Rgba32f>
setImageAlpha(const Rgba32f image, const float coverage) noexcept {
    if (!std::isfinite(coverage) || coverage < 0.0F || coverage > 1.0F)
        return ImageResult<Rgba32f>::failure(ImageError::codeOnly(ImageErrorCode::InvalidParameter));
    if (coverage == 0.0F)
        return ImageResult<Rgba32f>::success(Rgba32f::transparent());
    if (image.alpha() == 0.0F)
        return Rgba32f::fromPremultiplied(0.0F, 0.0F, 0.0F, coverage);
    const double scale = static_cast<double>(coverage) / static_cast<double>(image.alpha());
    const double red = static_cast<double>(image.red()) * scale;
    const double green = static_cast<double>(image.green()) * scale;
    const double blue = static_cast<double>(image.blue()) * scale;
    if (!std::isfinite(red) || !std::isfinite(green) || !std::isfinite(blue) ||
        std::abs(red) > static_cast<double>(std::numeric_limits<float>::max()) ||
        std::abs(green) > static_cast<double>(std::numeric_limits<float>::max()) ||
        std::abs(blue) > static_cast<double>(std::numeric_limits<float>::max()))
        return ImageResult<Rgba32f>::failure(ImageError::codeOnly(ImageErrorCode::NonFiniteResult));
    return Rgba32f::fromPremultiplied(static_cast<float>(red), static_cast<float>(green),
                                     static_cast<float>(blue), coverage);
}

} // namespace bloom::render
