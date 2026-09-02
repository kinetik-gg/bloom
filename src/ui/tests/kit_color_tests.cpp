#include <bloom/ui/kit/color.hpp>

#include <QApplication>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

using namespace bloom::ui;

[[nodiscard]] bool nearlyEqual(const float a, const float b, const float epsilon = 1e-4F) {
    return std::abs(a - b) <= epsilon;
}

void testQuantizationIsRoundHalfUpAndClamped(Expectations& expectations) {
    expectations.expect(kit::quantizeChannel8(0.0F) == 0, "zero quantizes to 0");
    expectations.expect(kit::quantizeChannel8(1.0F) == 255, "one quantizes to 255");
    // 0.5 * 255 = 127.5 -- round-half-up lands on 128, not 127 (round-half-to-even) or truncation.
    expectations.expect(kit::quantizeChannel8(0.5F) == 128,
                        "a half-channel value rounds half-up to 128");
    expectations.expect(kit::quantizeChannel8(-1.0F) == 0, "a negative value clamps to 0");
    expectations.expect(kit::quantizeChannel8(2.0F) == 255, "a value past 1.0 clamps to 255");
    expectations.expect(kit::dequantizeChannel8(255) == 1.0F, "255 dequantizes to exactly 1.0");
    expectations.expect(kit::dequantizeChannel8(0) == 0.0F, "0 dequantizes to exactly 0.0");
}

void testKnownHuesRoundTripThroughHsv(Expectations& expectations) {
    struct Case {
        const char* name;
        float r, g, b;
        float expectedHue;
    };
    constexpr std::array<Case, 6> cases{{
        {"red", 1.0F, 0.0F, 0.0F, 0.0F},
        {"yellow", 1.0F, 1.0F, 0.0F, 60.0F},
        {"green", 0.0F, 1.0F, 0.0F, 120.0F},
        {"cyan", 0.0F, 1.0F, 1.0F, 180.0F},
        {"blue", 0.0F, 0.0F, 1.0F, 240.0F},
        {"magenta", 1.0F, 0.0F, 1.0F, 300.0F},
    }};
    for (const auto& c : cases) {
        const kit::KColor color = kit::KColor::fromRgba(c.r, c.g, c.b);
        const auto hsva = color.toHsva();
        expectations.expect(nearlyEqual(hsva[0], c.expectedHue),
                            std::string(c.name) + "'s hue matches the known value");
        expectations.expect(nearlyEqual(hsva[1], 1.0F),
                            std::string(c.name) + " is fully saturated");
        expectations.expect(nearlyEqual(hsva[2], 1.0F), std::string(c.name) + " is at full value");

        const kit::KColor rebuilt = kit::KColor::fromHsva(hsva[0], hsva[1], hsva[2], hsva[3]);
        expectations.expect(nearlyEqual(rebuilt.red, c.r) && nearlyEqual(rebuilt.green, c.g) &&
                                nearlyEqual(rebuilt.blue, c.b),
                            std::string(c.name) + " round-trips exactly through HSVA");
    }
}

void testHue0And360AreTheSameColor(Expectations& expectations) {
    const kit::KColor atZero = kit::KColor::fromHsva(0.0F, 1.0F, 1.0F);
    const kit::KColor at360 = kit::KColor::fromHsva(360.0F, 1.0F, 1.0F);
    expectations.expect(atZero == at360, "hue 0 and hue 360 name the identical color");

    const kit::KColor pastFull = kit::KColor::fromHsva(720.0F, 0.5F, 0.5F);
    const kit::KColor normalized = kit::KColor::fromHsva(0.0F, 0.5F, 0.5F);
    expectations.expect(pastFull == normalized, "a hue past 360 wraps rather than clamping");
}

void testBlackAndWhiteSaturationDegeneracy(Expectations& expectations) {
    const auto blackHsv = kit::KColor::fromRgba(0.0F, 0.0F, 0.0F).toHsva();
    expectations.expect(nearlyEqual(blackHsv[1], 0.0F), "black reports zero saturation, not NaN");
    expectations.expect(nearlyEqual(blackHsv[2], 0.0F), "black reports zero value");

    const auto whiteHsv = kit::KColor::fromRgba(1.0F, 1.0F, 1.0F).toHsva();
    expectations.expect(nearlyEqual(whiteHsv[1], 0.0F), "white reports zero saturation");
    expectations.expect(nearlyEqual(whiteHsv[2], 1.0F), "white reports full value");

    const auto whiteHsl = kit::KColor::fromRgba(1.0F, 1.0F, 1.0F).toHsla();
    expectations.expect(nearlyEqual(whiteHsl[1], 0.0F),
                        "white reports zero saturation in HSL too (the 1-|2l-1| term is zero)");
    expectations.expect(nearlyEqual(whiteHsl[2], 1.0F), "white is full lightness");

    const auto blackHsl = kit::KColor::fromRgba(0.0F, 0.0F, 0.0F).toHsla();
    expectations.expect(nearlyEqual(blackHsl[1], 0.0F), "black reports zero saturation in HSL");
    expectations.expect(nearlyEqual(blackHsl[2], 0.0F), "black is zero lightness");

    const kit::KColor grayFromHsl = kit::KColor::fromHsla(200.0F, 0.0F, 0.5F);
    expectations.expect(nearlyEqual(grayFromHsl.red, 0.5F) &&
                            nearlyEqual(grayFromHsl.green, 0.5F) &&
                            nearlyEqual(grayFromHsl.blue, 0.5F),
                        "zero HSL saturation ignores hue and produces an exact gray");
}

void testHexRoundTrips(Expectations& expectations) {
    const auto parsed = kit::KColor::fromHex(QStringLiteral("#3399FF"));
    expectations.expect(parsed.has_value(), "a well-formed 6-digit hex parses");
    const kit::KColor parsedColor = parsed.value_or(kit::KColor{});
    expectations.expect(parsedColor.toHex(false) == QStringLiteral("#3399ff"),
                        "a 6-digit hex round-trips to its own lowercase text");
    expectations.expect(nearlyEqual(parsedColor.alpha, 1.0F),
                        "a 6-digit hex with no explicit fallback defaults to opaque");

    const auto withFallbackAlpha = kit::KColor::fromHex(QStringLiteral("3399ff"), 0.5F);
    expectations.expect(withFallbackAlpha.has_value(), "the leading # is optional");
    expectations.expect(nearlyEqual(withFallbackAlpha.value_or(kit::KColor{}).alpha, 0.5F),
                        "a 6-digit hex takes the caller's fallback alpha rather than forcing 1.0");

    const auto withAlpha = kit::KColor::fromHex(QStringLiteral("#3399FF80"));
    expectations.expect(withAlpha.has_value(), "an 8-digit hex parses");
    expectations.expect(withAlpha.value_or(kit::KColor{}).toHex(true) ==
                            QStringLiteral("#3399ff80"),
                        "an 8-digit hex round-trips including its alpha byte");

    const auto alphaZero = kit::KColor::fromHex(QStringLiteral("#00000000"));
    expectations.expect(alphaZero.has_value() &&
                            kit::quantizeChannel8(alphaZero.value_or(kit::KColor{}).alpha) == 0,
                        "an 8-digit hex with alpha 00 parses to fully transparent");
    const auto alphaFull = kit::KColor::fromHex(QStringLiteral("#000000FF"));
    expectations.expect(alphaFull.has_value() &&
                            kit::quantizeChannel8(alphaFull.value_or(kit::KColor{}).alpha) == 255,
                        "an 8-digit hex with alpha FF parses to fully opaque");

    expectations.expect(!kit::KColor::fromHex(QStringLiteral("#ABC")).has_value(),
                        "a 3-digit hex is refused rather than guessed at");
    expectations.expect(!kit::KColor::fromHex(QStringLiteral("#ZZZZZZ")).has_value(),
                        "non-hex characters are refused");
    expectations.expect(!kit::KColor::fromHex(QStringLiteral("")).has_value(),
                        "an empty string is refused");
}

void testQColorRoundTrip(Expectations& expectations) {
    const QColor original(51, 153, 255, 128);
    const kit::KColor color = kit::KColor::fromQColor(original);
    const QColor rebuilt = color.toQColor();
    expectations.expect(rebuilt == original, "an 8-bit QColor round-trips exactly through KColor");
}

void testColorModelAndPickerFormVocabulary(Expectations& expectations) {
    expectations.expect(kit::colorModelLabel(kit::ColorModel::Hsva) == QStringLiteral("HSVA"),
                        "HSVA labels itself");
    expectations.expect(kit::colorModelLabel(kit::ColorModel::Hsla) == QStringLiteral("HSLA"),
                        "HSLA labels itself");
    expectations.expect(kit::colorModelLabel(kit::ColorModel::Rgba) == QStringLiteral("RGBA"),
                        "RGBA labels itself");

    expectations.expect(kit::pickerFormAvailable(kit::PickerForm::Square),
                        "Square is the one available picker form this slice");
    for (const kit::PickerForm form : kit::pickerForms()) {
        if (form == kit::PickerForm::Square) {
            continue;
        }
        expectations.expect(!kit::pickerFormAvailable(form),
                            QString("form %1 is typed but not yet available")
                                .arg(kit::pickerFormLabel(form))
                                .toStdString());
    }
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testQuantizationIsRoundHalfUpAndClamped(expectations);
    testKnownHuesRoundTripThroughHsv(expectations);
    testHue0And360AreTheSameColor(expectations);
    testBlackAndWhiteSaturationDegeneracy(expectations);
    testHexRoundTrips(expectations);
    testQColorRoundTrip(expectations);
    testColorModelAndPickerFormVocabulary(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
