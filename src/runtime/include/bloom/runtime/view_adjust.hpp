#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bloom::runtime {

// Viewer presentation only. Process identities, thumbnails and file output never carry this value.
struct ViewAdjust final {
    double exposure = 0.0;
    double gamma = 1.0;

    [[nodiscard]] bool valid() const noexcept {
        return std::isfinite(exposure) && exposure >= -32.0 && exposure <= 32.0 &&
               std::isfinite(gamma) && gamma >= 0.01 && gamma <= 10.0;
    }
    [[nodiscard]] bool neutral() const noexcept { return exposure == 0.0 && gamma == 1.0; }
    [[nodiscard]] double linearExposure(double value) const noexcept {
        return value * std::exp2(exposure);
    }
    [[nodiscard]] double encodedGamma(double value) const noexcept {
        const auto clamped = std::clamp(value, 0.0, 1.0);
        return gamma == 1.0 ? clamped : std::pow(clamped, 1.0 / gamma);
    }
    // Bloom Neutral's display encoding is sRGB. Exposure acts on its linear display light,
    // after the view transform; gamma acts on its encoded value before RGBA8 quantization.
    [[nodiscard]] double fromLinear(double value) const noexcept {
        const auto linear = linearExposure(value);
        const auto encoded =
            linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
        return encodedGamma(encoded);
    }
    [[nodiscard]] double fromEncoded(double value) const noexcept {
        if (exposure == 0.0)
            return encodedGamma(value);
        const auto linear =
            value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        return fromLinear(linear);
    }
    [[nodiscard]] static std::uint8_t quantize(double value) noexcept {
        return static_cast<std::uint8_t>(std::floor(std::clamp(value, 0.0, 1.0) * 255.0 + 0.5));
    }
    friend bool operator==(const ViewAdjust&, const ViewAdjust&) = default;
};

} // namespace bloom::runtime
