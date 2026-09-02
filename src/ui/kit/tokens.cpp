#include <bloom/ui/kit/tokens.hpp>

#include <QByteArray>
#include <QGuiApplication>
#include <QRgb>
#include <QScreen>

#include <algorithm>
#include <array>
#include <cmath>

namespace bloom::ui::kit {
namespace {

struct ColorEntry {
    Color token;
    std::uint32_t rgb;
};

// The one place a Kinetik hex value is written down. Everything else -- painters, the generated
// application stylesheet, and the QPalette the theme installer builds -- resolves through
// color()/hex() so a rule and a painter can never drift apart.
constexpr auto kColors = std::to_array<ColorEntry>({
    {Color::Background, 0x111111U},    {Color::Surface, 0x161616U},
    {Color::SurfaceRaised, 0x1B1B1BU}, {Color::Field, 0x202020U},
    {Color::Foreground, 0xFFFFFFU},    {Color::Muted, 0x999999U},
    {Color::Faint, 0x666666U},         {Color::Border, 0x222222U},
    {Color::BorderHover, 0x454545U},   {Color::Accent, 0x0C8CE9U},
    {Color::AccentHover, 0x3AA5F0U},   {Color::AccentPressed, 0x0A73C2U},
    {Color::Keyframe, 0xF5C542U},      {Color::Ok, 0x3FBF6BU},
    {Color::Warn, 0xE6A23CU},          {Color::Error, 0xE0554EU},
    {Color::Brand, 0xE879ABU},         {Color::DataSequence, 0xE0554EU},
    {Color::DataClip, 0x3FBF6BU},      {Color::DataComposition, 0x8B5CF6U},
    {Color::DataImage, 0x3AA5F0U},     {Color::DataAudio, 0x7C5CFFU},
});

// Darkest first. surfaceStep() walks exactly this ladder and nothing else.
constexpr auto kSurfaceLadder =
    std::to_array<Color>({Color::Background, Color::Surface, Color::SurfaceRaised, Color::Field});

static_assert(kSurfaceLadder.size() == static_cast<std::size_t>(kSurfaceLevelCount));

[[nodiscard]] qreal pointSizeForDesignPixels(const qreal pixels) {
    // A design pixel is a Qt logical pixel, but QFont only carries a fractional size in points --
    // setPixelSize() is integral, and 12.5 and 11.5 are real tokens. Convert through the screen's
    // own logical DPI so the result is exactly `pixels` logical pixels tall on platforms that
    // report 96 (Linux, Windows) and on platforms that report 72 (macOS) alike.
    qreal dotsPerInch = 96.0;
    if (const auto* screen = QGuiApplication::primaryScreen(); screen != nullptr) {
        dotsPerInch = screen->logicalDotsPerInch();
    }
    if (!(dotsPerInch > 0.0)) {
        dotsPerInch = 96.0;
    }
    return pixels * 72.0 / dotsPerInch;
}

} // namespace

QColor color(const Color token) {
    const auto* const entry = std::ranges::find(kColors, token, &ColorEntry::token);
    if (entry == kColors.end()) {
        return {};
    }
    return QColor::fromRgb(static_cast<QRgb>(entry->rgb));
}

QString hex(const Color token) { return color(token).name(QColor::HexRgb); }

Color surfaceStep(const Color token, const int steps) {
    const auto* const level = std::ranges::find(kSurfaceLadder, token);
    if (level == kSurfaceLadder.end()) {
        return token;
    }
    const auto index = static_cast<int>(std::distance(kSurfaceLadder.begin(), level));
    const int stepped = std::clamp(index + steps, 0, kSurfaceLevelCount - 1);
    return kSurfaceLadder.at(static_cast<std::size_t>(stepped));
}

int radiusPx(const Radius token, const int extentPx) {
    if (token == Radius::Full) {
        return std::max(0, extentPx) / 2;
    }
    return static_cast<int>(token);
}

qreal snappedHairlineWidth(const qreal devicePixelRatio) {
    if (!(devicePixelRatio > 0.0)) {
        return kHairlineWidth;
    }
    const qreal physical = std::max(1.0, std::round(kHairlineWidth * devicePixelRatio));
    return physical / devicePixelRatio;
}

Shadow shadow(const Elevation token) {
    switch (token) {
    case Elevation::Flat:
        return {};
    case Elevation::Popup:
        return {0, 4, 16, QColor(0, 0, 0, 128)};
    case Elevation::Dialog:
        return {0, 12, 40, QColor(0, 0, 0, 153)};
    case Elevation::Drag:
        return {0, 18, 48, QColor(0, 0, 0, 140)};
    }
    return {};
}

QString interfaceFontFamily() { return QStringLiteral("Plus Jakarta Sans"); }

QString monospaceFontFamily() { return QStringLiteral("Geist Mono"); }

QFont font(const TypeRole role) {
    QFont value;
    // A families list rather than a single family: if the bundled face never registered, Qt walks
    // straight on to the platform's own sans-serif or monospace family instead of rendering a
    // wrong or missing face. The interface never depends on the bundled asset being present.
    switch (role) {
    case TypeRole::Ui:
        value.setFamilies({interfaceFontFamily()});
        value.setStyleHint(QFont::SansSerif);
        value.setPointSizeF(pointSizeForDesignPixels(12.5));
        value.setWeight(QFont::Medium);
        break;
    case TypeRole::UiSmall:
        value.setFamilies({interfaceFontFamily()});
        value.setStyleHint(QFont::SansSerif);
        value.setPointSizeF(pointSizeForDesignPixels(11.0));
        value.setWeight(QFont::Medium);
        value.setCapitalization(QFont::AllUppercase);
        value.setLetterSpacing(QFont::PercentageSpacing, 107.0);
        break;
    case TypeRole::Value:
        value.setFamilies({monospaceFontFamily()});
        value.setStyleHint(QFont::Monospace);
        value.setFixedPitch(true);
        value.setPointSizeF(pointSizeForDesignPixels(11.5));
        value.setWeight(QFont::Medium);
        break;
    case TypeRole::Title:
        value.setFamilies({interfaceFontFamily()});
        value.setStyleHint(QFont::SansSerif);
        value.setPointSizeF(pointSizeForDesignPixels(13.0));
        value.setWeight(QFont::DemiBold);
        break;
    }
    return value;
}

QColor withOpacity(const QColor& value, const qreal opacity) {
    QColor faded = value;
    faded.setAlphaF(static_cast<float>(std::clamp(opacity, 0.0, 1.0)) * value.alphaF());
    return faded;
}

int durationMs(const Motion token) {
    if (!motionEnabled()) {
        return 0;
    }
    switch (token) {
    case Motion::Fast:
        return 80;
    case Motion::Pop:
        return 140;
    case Motion::None:
        return 0;
    }
    return 0;
}

QEasingCurve easing(const Motion token) {
    switch (token) {
    case Motion::Fast:
    case Motion::Pop:
        // Bloom's "ease-out": decelerating, no overshoot, no anticipation.
        return QEasingCurve(QEasingCurve::OutCubic);
    case Motion::None:
        return QEasingCurve(QEasingCurve::Linear);
    }
    return QEasingCurve(QEasingCurve::Linear);
}

bool motionEnabled() {
    // Qt 6.8 exposes no cross-platform reduced-motion style hint (QStyleHints has none through
    // 6.11), so the contract is an explicit kill switch: BLOOM_REDUCED_MOTION set to anything
    // other than "0" disables every kit animation, which then jumps straight to its end state.
    const QByteArray requested = qgetenv("BLOOM_REDUCED_MOTION");
    return requested.isEmpty() || requested == "0";
}

} // namespace bloom::ui::kit
