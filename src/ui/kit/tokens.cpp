#include <bloom/ui/kit/tokens.hpp>

#include <bloom/ui/kit/fonts.hpp>

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
    {Color::Background, 0x111111U},
    {Color::Canvas, 0x131313U},
    {Color::CompositionFrame, 0x454545U},
    {Color::SurfaceSunken, 0x0B0B0BU},
    // The ladder is spaced so every step reads on its own: hover, selected and alternating rows
    // and the Secondary button each move one rung, and a rung that sat ~1.02:1 from its neighbour
    // (0x141414 / 0x1B1B1B / 0x202020) made all of those invisible. Still a dark ladder: the
    // brightest rung is an input cell, never a panel.
    {Color::Surface, 0x181818U},
    {Color::SurfaceRaised, 0x222222U},
    {Color::Field, 0x2B2B2BU},
    {Color::Foreground, 0xFFFFFFU},
    {Color::Muted, 0x999999U},
    {Color::Faint, 0x666666U},
    // Border sits one clear step above SurfaceRaised so a raised plate keeps a visible edge;
    // BorderActive is one clear step above BorderHover so hover and drag are two states.
    {Color::Border, 0x2E2E2EU},
    {Color::BorderHover, 0x4A4A4AU},
    {Color::BorderActive, 0x626262U},
    // Accent is deep enough that OnAccent text on it meets WCAG AA at 12 px (4.6:1; the previous
    // 0x0C8CE9 measured 3.5:1). Hover blends toward Foreground and press scales the channels down
    // by the same relation the previous triple stated (kit_button_tests.cpp asserts both).
    {Color::Accent, 0x0A78C8U},
    {Color::OnAccent, 0xFFFFFFU},
    {Color::AccentHover, 0x3F96D5U},
    {Color::AccentPressed, 0x0863A6U},
    {Color::Keyframe, 0xF5C542U},
    {Color::Ok, 0x3FBF6BU},
    {Color::Warn, 0xE6A23CU},
    {Color::Error, 0xE0554EU},
    {Color::CheckerLight, 0x999999U},
    {Color::CheckerDark, 0x666666U},
    {Color::ViewerCheckerLight, 0x3A3A3AU},
    {Color::ViewerCheckerDark, 0x2A2A2AU},
    {Color::CanvasBlack, 0x000000U},
    {Color::CanvasGrey, 0x808080U},
    {Color::CanvasWhite, 0xFFFFFFU},
    {Color::DataSequence, 0xE0554EU},
    {Color::DataClip, 0x3FBF6BU},
    {Color::DataComposition, 0x8B5CF6U},
    {Color::Label1, 0xD97C7CU},
    {Color::Label2, 0xD9A66CU},
    {Color::Label3, 0xC9C76BU},
    {Color::Label4, 0x83BD84U},
    {Color::Label5, 0x68BABAU},
    {Color::Label6, 0x799ED2U},
    {Color::Label7, 0xAA8ACCU},
    {Color::Label8, 0xCE89B4U},
    {Color::DataImage, 0x3AA5F0U},
    {Color::DataAudio, 0x7C5CFFU},
    // task S1, item 6: the socket/link transport palette. Six separated hues -- teal, vermilion,
    // yellow-green, cyan, violet, gold -- and none of them is Accent (0x0C8CE9) or AccentHover
    // (0x3AA5F0), so a socket can never be mistaken for a selection or a hovered accent surface.
    {Color::SocketImage, 0x2FC8A0U},
    {Color::SocketAudio, 0x7C5CFFU},
    {Color::SocketColor, 0xF2713CU},
    {Color::SocketScalar, 0x8FD44AU},
    {Color::SocketInteger, 0x4AC8D4U},
    {Color::SocketVector, 0xC87AF0U},
    {Color::SocketString, 0xF0C93CU},
    {Color::SocketBoolean, 0xE0567BU},
    {Color::SocketChannelSet, 0x4CD964U},
    // GRAPH-1 component curves: red, green, a light sky blue clearly lighter than Accent
    // (0x0C8CE9) and AccentHover (0x3AA5F0), and a neutral for the fourth channel.
    {Color::ComponentX, 0xE06C75U},
    {Color::ComponentY, 0x8CC265U},
    {Color::ComponentZ, 0x7FC4F5U},
    {Color::ComponentW, 0xC9C9C9U},
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
    case Elevation::TimelineBar:
        return {0, 1, 2, QColor(0, 0, 0, 64)};
    case Elevation::Popup:
        return {0, 0, 8, QColor(0, 0, 0, 115)};
    case Elevation::Dialog:
        return {0, 12, 40, QColor(0, 0, 0, 153)};
    case Elevation::Drag:
        return {0, 18, 48, QColor(0, 0, 0, 140)};
    }
    return {};
}

QString interfaceFontFamily() { return QStringLiteral("Inter"); }

QString monospaceFontFamily() { return QStringLiteral("Geist Mono"); }

QFont font(const TypeRole role) {
    QFont value;
    // A families list rather than a single family, resolved by the font module: the exact bundled
    // face for this role's weight first, its base family next, and the platform's own sans-serif
    // or monospace family last. If nothing bundled registered, the interface simply runs on the
    // platform family -- degraded and reported, never missing or crashed.
    value.setFamilies(fontFamiliesForRole(role));
    switch (role) {
    case TypeRole::Ui:
        value.setStyleHint(QFont::SansSerif);
        value.setPointSizeF(pointSizeForDesignPixels(12.0));
        value.setWeight(QFont::Medium);
        break;
    case TypeRole::UiSmall:
    case TypeRole::UiSmallStrong:
        // Small chrome text: mixed case and natural tracking, stated here once so no call site
        // re-applies them.
        value.setStyleHint(QFont::SansSerif);
        value.setPointSizeF(pointSizeForDesignPixels(10.5));
        value.setWeight(role == TypeRole::UiSmallStrong ? QFont::DemiBold : QFont::Medium);
        value.setCapitalization(QFont::MixedCase);
        value.setLetterSpacing(QFont::PercentageSpacing, 100.0);
        break;
    case TypeRole::Value:
        value.setStyleHint(QFont::Monospace);
        value.setFixedPitch(true);
        value.setPointSizeF(pointSizeForDesignPixels(11.5));
        value.setWeight(QFont::Medium);
        break;
    case TypeRole::Title:
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

QColor disabledInk(const QColor& resting) { return withOpacity(resting, kDisabledOpacity); }

QColor selectionWash(const Color surface) {
    const QColor base = color(surface);
    const QColor accent = color(Color::Accent);
    const auto mix = [](const int from, const int to) {
        return static_cast<int>(std::lround(static_cast<qreal>(from) +
                                            kSelectionWashOpacity * static_cast<qreal>(to - from)));
    };
    return {mix(base.red(), accent.red()), mix(base.green(), accent.green()),
            mix(base.blue(), accent.blue())};
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
    case Motion::Sweep:
        return 1600;
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
    case Motion::Sweep:
        return QEasingCurve(QEasingCurve::InOutSine);
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
