#pragma once

#include <QColor>
#include <QString>
#include <QStringView>

#include <array>
#include <cstdint>
#include <optional>

namespace bloom::ui::kit {

// Task U6 (issue #121): the color picker family's value model, kept deliberately independent of
// bloom::core::Color4d. bloom_ui_kit is the design system -- CMakeLists.txt says "nothing about a
// panel, a session, or a document may leak into it" -- and Color4d is a document-authoring type
// owned by the parameter schema it lives inside (see src/core/include/bloom/core/color.hpp). KColor
// is the kit's own straight-alpha RGBA working value, self-contained on purpose: the eventual
// consumer (composition_editors.cpp's Solid Source row) converts between the two at its own
// boundary, in a later slice this one does not wire.
//
// Every channel is float in [0, 1] except hue, which is degrees in [0, 360). Alpha is straight
// (unassociated), matching Color4d's own convention so a future bridge is a plain field copy plus a
// float cast, not a re-derivation.
struct KColor final {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    float alpha = 1.0F;

    [[nodiscard]] static constexpr KColor fromRgba(const float r, const float g, const float b,
                                                    const float a = 1.0F) noexcept {
        return KColor{r, g, b, a};
    }

    // `hue` in degrees, any finite value (normalized modulo 360 internally so 360 and 0 name the
    // same color); `saturation`, `value`, `alpha` in [0, 1].
    [[nodiscard]] static KColor fromHsva(float hue, float saturation, float value,
                                         float alpha = 1.0F) noexcept;

    [[nodiscard]] static KColor fromHsla(float hue, float saturation, float lightness,
                                         float alpha = 1.0F) noexcept;

    // Parses a "#rrggbb" or "#rrggbbaa" string (the leading '#' is optional, case-insensitive).
    // A 6-digit string carries no alpha, so `fallbackAlpha` fills it in -- this is what lets a
    // picker preserve the alpha channel it already has when the artist types a plain RGB hex value
    // rather than silently forcing full opacity. Returns nullopt for anything else: wrong length,
    // non-hex digits, or empty.
    [[nodiscard]] static std::optional<KColor> fromHex(QStringView text,
                                                       float fallbackAlpha = 1.0F);

    [[nodiscard]] static KColor fromQColor(const QColor& value) noexcept;

    // h in [0, 360), s/v in [0, 1], a passthrough.
    [[nodiscard]] std::array<float, 4> toHsva() const noexcept;

    // h in [0, 360), s/l in [0, 1], a passthrough.
    [[nodiscard]] std::array<float, 4> toHsla() const noexcept;

    // Lowercase "#rrggbb", or "#rrggbbaa" when `includeAlpha` is set. Each channel is quantized to
    // 8 bits with round-half-up and a 0..255 clamp (see quantizeChannel8()) -- the same rule
    // toQColor() uses, so the hex text and the swatch pixel can never disagree.
    [[nodiscard]] QString toHex(bool includeAlpha) const;

    // 8-bit quantized, for painting. Round-half-up, clamped.
    [[nodiscard]] QColor toQColor() const noexcept;

    [[nodiscard]] bool isOpaque() const noexcept { return alpha >= 1.0F; }

    friend bool operator==(const KColor&, const KColor&) noexcept = default;
};

// The float-to-8-bit quantization rule every KColor conversion uses: round-half-up (0.5 rounds
// away from zero, not to even), then clamp to [0, 255]. Exposed so a test can pin the rule
// directly rather than inferring it from a round-tripped color.
[[nodiscard]] int quantizeChannel8(float value) noexcept;
[[nodiscard]] constexpr float dequantizeChannel8(const int value) noexcept {
    return static_cast<float>(value) / 255.0F;
}

// The channel layout KColorPicker's numeric field rows switch between (decision 3). Each model
// names the same underlying color through a different set of KValueField rows and ranges.
enum class ColorModel : std::uint8_t {
    Hsva,
    Hsla,
    Rgba,
};

[[nodiscard]] QString colorModelLabel(ColorModel model);

// The picker shapes issue #121 scopes across the whole widget family. Square is the only one this
// slice implements; the rest are architecture-present so a later drop-in is a new case in
// pickerFormAvailable() plus a paint/drag implementation, never a new enum member or a call-site
// rewrite. A form that is not available is not merely disabled -- it is absent from the form
// selector entirely (visual-language.md: no dead UI), which is why callers must consult
// pickerFormAvailable() before listing a form rather than showing every enumerator.
enum class PickerForm : std::uint8_t {
    Square,
    Wheel,
    Triangle,
    Spectrum,
    SliderStack,
};

[[nodiscard]] constexpr bool pickerFormAvailable(const PickerForm form) noexcept {
    return form == PickerForm::Square;
}

[[nodiscard]] QString pickerFormLabel(PickerForm form);

// Every form, in declaration order -- for a test to prove the whole enum is accounted for rather
// than spot-checking the one member that happens to be implemented.
[[nodiscard]] std::array<PickerForm, 5> pickerForms();

} // namespace bloom::ui::kit
