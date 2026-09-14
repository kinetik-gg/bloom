#pragma once

#include <cmath>

namespace bloom::core {

// Straight/unassociated authoring RGBA. The owning parameter schema defines the RGB encoding;
// Color4d itself deliberately carries no color-space or alpha-association conversion behavior.
struct Color4d {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double alpha = 1.0;

    [[nodiscard]] bool isValid() const noexcept {
        return std::isfinite(red) && std::isfinite(green) && std::isfinite(blue) &&
               std::isfinite(alpha) && alpha >= 0.0 && alpha <= 1.0;
    }

    friend bool operator==(const Color4d&, const Color4d&) = default;
};

// Rec.709 relative luminance of a straight RGB triple (task UTIL-1). These are the weights BT.709
// gives a linear-light triple, and they live HERE rather than being retyped in a node kernel for
// the same reason blend modes live in bloom_core: a number an artist keys against must be the same
// number in every surface that computes it, and three magic constants copied into an evaluator are
// three constants that can drift.
//
// It reads the channels AS AUTHORED and implies no transfer function of its own: the owning
// parameter schema defines the encoding, exactly as Color4d's own comment says. Alpha is not a
// colour and takes no part.
inline constexpr double kRec709RedLuminanceWeight = 0.2126;
inline constexpr double kRec709GreenLuminanceWeight = 0.7152;
inline constexpr double kRec709BlueLuminanceWeight = 0.0722;

[[nodiscard]] inline double rec709Luminance(const Color4d& color) noexcept {
    return kRec709RedLuminanceWeight * color.red + kRec709GreenLuminanceWeight * color.green +
           kRec709BlueLuminanceWeight * color.blue;
}

// HSV is a reparameterization of the SAME triple, not a colour-space conversion: no primaries, no
// white point and no transfer function change, which is why it belongs beside Color4d while an
// OCIO transform does not. Hue is in DEGREES over [0, 360) because degrees are the unit a Hue Shift
// operand is authored in; saturation and value are unit-ranged.
struct Hsva final {
    double hue = 0.0;
    double saturation = 0.0;
    double value = 0.0;
    double alpha = 1.0;

    friend bool operator==(const Hsva&, const Hsva&) = default;
};

// A grey has no hue, so an achromatic input answers hue 0 rather than an arbitrary angle -- the
// same choice Normalize makes for a zero-length vector, for the same reason. Channels outside
// [0, 1] are read as authored; the conversion is total and never fails.
[[nodiscard]] inline Hsva toHsva(const Color4d& color) noexcept {
    const double maximum = std::fmax(color.red, std::fmax(color.green, color.blue));
    const double minimum = std::fmin(color.red, std::fmin(color.green, color.blue));
    const double chroma = maximum - minimum;
    double hue = 0.0;
    if (chroma > 0.0) {
        if (maximum == color.red) {
            hue = std::fmod((color.green - color.blue) / chroma, 6.0);
        } else if (maximum == color.green) {
            hue = (color.blue - color.red) / chroma + 2.0;
        } else {
            hue = (color.red - color.green) / chroma + 4.0;
        }
        hue *= 60.0;
        if (hue < 0.0) {
            hue += 360.0;
        }
    }
    const double saturation = maximum == 0.0 ? 0.0 : chroma / maximum;
    return {hue, saturation, maximum, color.alpha};
}

[[nodiscard]] inline Color4d fromHsva(const Hsva& hsva) noexcept {
    const double value = hsva.value;
    const double saturation =
        hsva.saturation < 0.0 ? 0.0 : (hsva.saturation > 1.0 ? 1.0 : hsva.saturation);
    // Hue WRAPS rather than clamping: an angle is periodic, so 400 degrees is 40 and -30 is 330. A
    // Hue Shift that ran into a clamp would stop moving instead of going round.
    double hue = std::fmod(hsva.hue, 360.0);
    if (hue < 0.0) {
        hue += 360.0;
    }
    const double chroma = value * saturation;
    const double sector = hue / 60.0;
    const double secondary = chroma * (1.0 - std::fabs(std::fmod(sector, 2.0) - 1.0));
    const double offset = value - chroma;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    if (sector < 1.0) {
        red = chroma;
        green = secondary;
    } else if (sector < 2.0) {
        red = secondary;
        green = chroma;
    } else if (sector < 3.0) {
        green = chroma;
        blue = secondary;
    } else if (sector < 4.0) {
        green = secondary;
        blue = chroma;
    } else if (sector < 5.0) {
        red = secondary;
        blue = chroma;
    } else {
        red = chroma;
        blue = secondary;
    }
    return {red + offset, green + offset, blue + offset, hsva.alpha};
}

} // namespace bloom::core
