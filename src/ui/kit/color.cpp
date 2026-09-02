#include <bloom/ui/kit/color.hpp>

#include <QLatin1Char>
#include <QLatin1StringView>

#include <algorithm>
#include <cmath>

namespace bloom::ui::kit {
namespace {

constexpr float kEpsilon = 1e-6F;

[[nodiscard]] float normalizedHue(const float hue) noexcept {
    float normalized = std::fmod(hue, 360.0F);
    if (normalized < 0.0F) {
        normalized += 360.0F;
    }
    // fmod(360, 360) is already 0, but guard the case a caller hands in exactly -0.0F worth of
    // drift so 360 and 0 are always bit-for-bit the same normalized value (the edge-hue pin the
    // tests exercise).
    return normalized >= 360.0F ? 0.0F : normalized;
}

[[nodiscard]] float clamp01(const float value) noexcept { return std::clamp(value, 0.0F, 1.0F); }

// The hue formula shared by RGB->HSV and RGB->HSL: given the channel max/delta, which channel is
// the max decides the formula branch, and the result is identical in both spaces.
[[nodiscard]] float hueFromRgb(const float r, const float g, const float b, const float max,
                               const float delta) noexcept {
    if (delta <= kEpsilon) {
        // Achromatic: black, white, or any pure gray. Hue is mathematically undefined from RGB
        // alone here, so this pure function reports 0 by convention -- a picker that wants a
        // sticky prior hue while dragging across this degeneracy keeps that hue in its own
        // editing state rather than asking KColor to remember it (KColor is stateless on purpose).
        return 0.0F;
    }
    float hue;
    if (max == r) {
        hue = 60.0F * std::fmod((g - b) / delta, 6.0F);
    } else if (max == g) {
        hue = 60.0F * (((b - r) / delta) + 2.0F);
    } else {
        hue = 60.0F * (((r - g) / delta) + 4.0F);
    }
    return normalizedHue(hue);
}

// Shared HSV/HSL "chroma at this hue" decomposition: given chroma `c` and the hue's position
// within its 60-degree segment, returns the (r, g, b) triple before the lightness/value offset
// `m` is added back in.
[[nodiscard]] std::array<float, 3> chromaTriple(const float hue, const float c) noexcept {
    const float hPrime = hue / 60.0F;
    const float x = c * (1.0F - std::abs(std::fmod(hPrime, 2.0F) - 1.0F));
    const auto segment = static_cast<int>(std::floor(hPrime)) % 6;
    switch (segment) {
    case 0:
        return {c, x, 0.0F};
    case 1:
        return {x, c, 0.0F};
    case 2:
        return {0.0F, c, x};
    case 3:
        return {0.0F, x, c};
    case 4:
        return {x, 0.0F, c};
    default:
        return {c, 0.0F, x};
    }
}

[[nodiscard]] std::optional<int> hexNibble(const QChar ch) noexcept {
    const char16_t unicode = ch.unicode();
    if (unicode >= u'0' && unicode <= u'9') {
        return unicode - u'0';
    }
    if (unicode >= u'a' && unicode <= u'f') {
        return 10 + (unicode - u'a');
    }
    if (unicode >= u'A' && unicode <= u'F') {
        return 10 + (unicode - u'A');
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<int> hexByte(const QStringView text, const qsizetype offset) noexcept {
    const auto high = hexNibble(text[offset]);
    const auto low = hexNibble(text[offset + 1]);
    if (!high.has_value() || !low.has_value()) {
        return std::nullopt;
    }
    return (*high << 4) | *low;
}

} // namespace

int quantizeChannel8(const float value) noexcept {
    // Round-half-up, not round-half-to-even: floor(x*255 + 0.5) lands 0.5 away from zero exactly
    // the way an 8-bit color picker's hex field is expected to.
    const float scaled = std::floor(value * 255.0F + 0.5F);
    return std::clamp(static_cast<int>(scaled), 0, 255);
}

KColor KColor::fromHsva(const float hue, const float saturation, const float value,
                        const float alpha) noexcept {
    const float h = normalizedHue(hue);
    const float s = clamp01(saturation);
    const float v = clamp01(value);
    const float c = v * s;
    const float m = v - c;
    const auto triple = chromaTriple(h, c);
    return KColor{triple[0] + m, triple[1] + m, triple[2] + m, clamp01(alpha)};
}

KColor KColor::fromHsla(const float hue, const float saturation, const float lightness,
                        const float alpha) noexcept {
    const float h = normalizedHue(hue);
    const float s = clamp01(saturation);
    const float l = clamp01(lightness);
    const float c = (1.0F - std::abs(2.0F * l - 1.0F)) * s;
    const float m = l - c / 2.0F;
    const auto triple = chromaTriple(h, c);
    return KColor{triple[0] + m, triple[1] + m, triple[2] + m, clamp01(alpha)};
}

std::optional<KColor> KColor::fromHex(const QStringView text, const float fallbackAlpha) {
    QStringView digits = text;
    if (!digits.isEmpty() && digits.front() == QLatin1Char('#')) {
        digits = digits.mid(1);
    }
    if (digits.size() != 6 && digits.size() != 8) {
        return std::nullopt;
    }
    const auto red = hexByte(digits, 0);
    const auto green = hexByte(digits, 2);
    const auto blue = hexByte(digits, 4);
    if (!red.has_value() || !green.has_value() || !blue.has_value()) {
        return std::nullopt;
    }
    float alpha = clamp01(fallbackAlpha);
    if (digits.size() == 8) {
        const auto alphaByte = hexByte(digits, 6);
        if (!alphaByte.has_value()) {
            return std::nullopt;
        }
        alpha = dequantizeChannel8(*alphaByte);
    }
    return KColor{dequantizeChannel8(*red), dequantizeChannel8(*green), dequantizeChannel8(*blue),
                 alpha};
}

KColor KColor::fromQColor(const QColor& value) noexcept {
    return KColor{dequantizeChannel8(value.red()), dequantizeChannel8(value.green()),
                 dequantizeChannel8(value.blue()), dequantizeChannel8(value.alpha())};
}

std::array<float, 4> KColor::toHsva() const noexcept {
    const float max = std::max({red, green, blue});
    const float min = std::min({red, green, blue});
    const float delta = max - min;
    const float h = hueFromRgb(red, green, blue, max, delta);
    const float s = max <= kEpsilon ? 0.0F : delta / max;
    return {h, clamp01(s), clamp01(max), clamp01(alpha)};
}

std::array<float, 4> KColor::toHsla() const noexcept {
    const float max = std::max({red, green, blue});
    const float min = std::min({red, green, blue});
    const float delta = max - min;
    const float l = (max + min) / 2.0F;
    const float h = hueFromRgb(red, green, blue, max, delta);
    const float lSpan = 1.0F - std::abs(2.0F * l - 1.0F);
    const float s = (delta <= kEpsilon || lSpan <= kEpsilon) ? 0.0F : delta / lSpan;
    return {h, clamp01(s), clamp01(l), clamp01(alpha)};
}

QString KColor::toHex(const bool includeAlpha) const {
    QString rgb = QStringLiteral("#%1%2%3")
                            .arg(quantizeChannel8(red), 2, 16, QLatin1Char('0'))
                            .arg(quantizeChannel8(green), 2, 16, QLatin1Char('0'))
                            .arg(quantizeChannel8(blue), 2, 16, QLatin1Char('0'));
    if (!includeAlpha) {
        return rgb;
    }
    return rgb + QStringLiteral("%1").arg(quantizeChannel8(alpha), 2, 16, QLatin1Char('0'));
}

QColor KColor::toQColor() const noexcept {
    return QColor(quantizeChannel8(red), quantizeChannel8(green), quantizeChannel8(blue),
                 quantizeChannel8(alpha));
}

QString colorModelLabel(const ColorModel model) {
    switch (model) {
    case ColorModel::Hsva:
        return QStringLiteral("HSVA");
    case ColorModel::Hsla:
        return QStringLiteral("HSLA");
    case ColorModel::Rgba:
        return QStringLiteral("RGBA");
    }
    return QStringLiteral("HSVA");
}

QString pickerFormLabel(const PickerForm form) {
    switch (form) {
    case PickerForm::Square:
        return QStringLiteral("Square");
    case PickerForm::Wheel:
        return QStringLiteral("Wheel");
    case PickerForm::Triangle:
        return QStringLiteral("Triangle");
    case PickerForm::Spectrum:
        return QStringLiteral("Spectrum");
    case PickerForm::SliderStack:
        return QStringLiteral("Sliders");
    }
    return QStringLiteral("Square");
}

std::array<PickerForm, 5> pickerForms() {
    return {PickerForm::Square, PickerForm::Wheel, PickerForm::Triangle, PickerForm::Spectrum,
           PickerForm::SliderStack};
}

} // namespace bloom::ui::kit
