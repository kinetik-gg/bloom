#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QString>

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

void testEveryColorRoleResolvesToItsSpecifiedValue(Expectations& expectations) {
    const auto expectHex = [&expectations](const kit::Color token, const char* expected) {
        expectations.expect(kit::hex(token) == QString::fromLatin1(expected),
                            std::string{"color role resolves to "} + expected + " (got " +
                                kit::hex(token).toStdString() + ')');
        expectations.expect(kit::color(token).isValid(), "color role resolves to a valid QColor");
        expectations.expect(kit::color(token).alpha() == 255, "color roles are fully opaque");
    };

    expectHex(kit::Color::Background, "#111111");
    // The widened ladder: each rung perceptibly above the one below it.
    expectHex(kit::Color::Surface, "#181818");
    expectHex(kit::Color::SurfaceRaised, "#222222");
    expectHex(kit::Color::Field, "#2b2b2b");
    expectHex(kit::Color::Foreground, "#ffffff");
    expectHex(kit::Color::Muted, "#999999");
    expectHex(kit::Color::Faint, "#666666");
    expectHex(kit::Color::Border, "#2e2e2e");
    expectHex(kit::Color::BorderHover, "#4a4a4a");
    // BorderActive is one clear step above BorderHover: hover and drag are two states.
    expectHex(kit::Color::BorderActive, "#626262");
    expectHex(kit::Color::Accent, "#0a78c8");
    expectHex(kit::Color::AccentHover, "#3f96d5");
    expectHex(kit::Color::AccentPressed, "#0863a6");
    expectHex(kit::Color::CheckerLight, "#999999");
    expectHex(kit::Color::CheckerDark, "#666666");
    expectHex(kit::Color::ViewerCheckerLight, "#3a3a3a");
    expectHex(kit::Color::ViewerCheckerDark, "#2a2a2a");
    expectHex(kit::Color::CanvasBlack, "#000000");
    expectHex(kit::Color::CanvasGrey, "#808080");
    expectHex(kit::Color::CanvasWhite, "#ffffff");
    expectHex(kit::Color::Keyframe, "#f5c542");
    expectHex(kit::Color::Ok, "#3fbf6b");
    expectHex(kit::Color::Warn, "#e6a23c");
    expectHex(kit::Color::Error, "#e0554e");
    expectHex(kit::Color::DataSequence, "#e0554e");
    expectHex(kit::Color::DataClip, "#3fbf6b");
    expectHex(kit::Color::DataComposition, "#8b5cf6");
    expectHex(kit::Color::DataImage, "#3aa5f0");
    expectHex(kit::Color::DataAudio, "#7c5cff");
    // task S1, item 6: the socket/link transport palette. Seven separated hues, none of them Accent
    // or AccentHover -- Image had been reading in exactly AccentHover's blue.
    expectHex(kit::Color::SocketImage, "#2fc8a0");
    expectHex(kit::Color::SocketAudio, "#7c5cff");
    expectHex(kit::Color::SocketColor, "#f2713c");
    expectHex(kit::Color::SocketScalar, "#8fd44a");
    expectHex(kit::Color::SocketInteger, "#4ac8d4");
    expectHex(kit::Color::SocketVector, "#c87af0");
    expectHex(kit::Color::SocketString, "#f0c93c");
    expectHex(kit::Color::SocketChannelSet, "#4cd964");

    const std::array sockets{kit::Color::SocketImage,   kit::Color::SocketAudio,
                             kit::Color::SocketColor,   kit::Color::SocketScalar,
                             kit::Color::SocketInteger, kit::Color::SocketVector,
                             kit::Color::SocketString,  kit::Color::SocketBoolean,
                             kit::Color::SocketChannelSet};
    for (std::size_t index = 0; index < sockets.size(); ++index) {
        expectations.expect(kit::color(sockets.at(index)) != kit::color(kit::Color::Accent) &&
                                kit::color(sockets.at(index)) !=
                                    kit::color(kit::Color::AccentHover),
                            "a socket role is never Accent or AccentHover");
        for (std::size_t other = index + 1; other < sockets.size(); ++other) {
            expectations.expect(kit::color(sockets.at(index)) != kit::color(sockets.at(other)),
                                "and no two socket roles share a value");
            expectations.expect(std::abs(kit::color(sockets.at(index)).hslHue() -
                                         kit::color(sockets.at(other)).hslHue()) >= 20,
                                "socket roles are separated by hue, not only by value");
        }
    }
}

void testSurfaceLadderStepsAndClamps(Expectations& expectations) {
    expectations.expect(kit::surfaceStep(kit::Color::Surface, 1) == kit::Color::SurfaceRaised,
                        "hover steps one surface level up");
    expectations.expect(kit::surfaceStep(kit::Color::Surface, -1) == kit::Color::Background,
                        "pressed steps one surface level down");
    expectations.expect(kit::surfaceStep(kit::Color::Field, 3) == kit::Color::Field,
                        "stepping past the top of the ladder clamps");
    expectations.expect(kit::surfaceStep(kit::Color::Background, -3) == kit::Color::Background,
                        "stepping past the bottom of the ladder clamps");
    expectations.expect(kit::surfaceStep(kit::Color::Accent, 1) == kit::Color::Accent,
                        "a non-surface role is not on the ladder and is returned unchanged");
}

void testGeometryTokensCarryTheSpecifiedNumbers(Expectations& expectations) {
    expectations.expect(kit::radiusPx(kit::Radius::Small, 26) == 3, "Radius::Small is 3");
    expectations.expect(kit::radiusPx(kit::Radius::Medium, 26) == 6, "Radius::Medium is 6");
    expectations.expect(kit::radiusPx(kit::Radius::Large, 26) == 12, "Radius::Large is 12");
    expectations.expect(kit::radiusPx(kit::Radius::XLarge, 26) == 16, "Radius::XLarge is 16");
    expectations.expect(kit::radiusPx(kit::Radius::Full, 26) == 13,
                        "Radius::Full is a pill: half the control extent");
    // The panel body's own radius, its own named step. It is 0: panels are square, square corners
    // and all; Radius::Small stays 3 everywhere else.
    expectations.expect(kit::radiusPx(kit::Radius::Panel, 26) == 0, "Radius::Panel is 0");

    expectations.expect(kit::px(kit::Spacing::XXS) == 2, "Spacing::XXS is 2");
    expectations.expect(kit::px(kit::Spacing::XS) == 4, "Spacing::XS is 4");
    expectations.expect(kit::px(kit::Spacing::S) == 8, "Spacing::S is 8");
    expectations.expect(kit::px(kit::Spacing::M) == 12, "Spacing::M is 12");
    expectations.expect(kit::px(kit::Spacing::L) == 16, "Spacing::L is 16");
    expectations.expect(kit::px(kit::Spacing::XL) == 24, "Spacing::XL is 24");
    expectations.expect(kit::px(kit::Spacing::XXL) == 32, "Spacing::XXL is 32");
    expectations.expect(kit::px(kit::Spacing::Gutter) == 6, "Spacing::Gutter is 6");
    // task U8, issue #131, formal amendment 1, A4: deliberately off the base-4 scale, not rounded.
    expectations.expect(kit::px(kit::Spacing::PanelHeader) == 10, "Spacing::PanelHeader is 10");
    expectations.expect(kit::px(kit::Spacing::MenuItemY) == 6, "Spacing::MenuItemY is 6");
    expectations.expect(kit::px(kit::Spacing::MenuItemX) == 10, "Spacing::MenuItemX is 10");

    expectations.expect(kit::px(kit::Size::Control) == 26, "Control is 26");
    expectations.expect(kit::px(kit::Spacing::ControlInset) == 1, "ControlInset is 1");
    expectations.expect(kit::px(kit::Size::Control) - 2 * kit::px(kit::Spacing::ControlInset) ==
                            kit::px(kit::Size::ToggleCell),
                        "a control's painted box is exactly one toggle cell tall");
    expectations.expect(kit::px(kit::Size::ControlRoomy) == 32, "ControlRoomy is 32");
    expectations.expect(kit::px(kit::Size::IconSmall) == 12, "IconSmall is 12");
    expectations.expect(kit::px(kit::Size::IconMedium) == 16, "IconMedium is 16");
    expectations.expect(kit::px(kit::Size::IconLarge) == 20, "IconLarge is 20");
    expectations.expect(kit::px(kit::Size::TitleBar) == 34, "TitleBar is 34");
    expectations.expect(kit::px(kit::Size::NodeGroupTitle) == 30, "NodeGroupTitle is 30");
    expectations.expect(kit::px(kit::Size::HeaderRow) == 32, "HeaderRow is 32");
    expectations.expect(kit::px(kit::Size::ListRow) == 32, "ListRow is 32");
    expectations.expect(kit::px(kit::Size::TimelineBar) == 20, "TimelineBar is 20");
    expectations.expect(kit::px(kit::Size::TimelineToggleColumn) == 136,
                        "TimelineToggleColumn is five 24 px cells plus four XS gaps");
    expectations.expect(kit::px(kit::Size::TimelineSwitchColumn) == 28,
                        "TimelineSwitchColumn is one 24 px cell plus its XS gap");
    expectations.expect(kit::px(kit::Size::TimelineNameMin) == 120, "TimelineNameMin is 120");
    expectations.expect(kit::px(kit::Size::TimelineColumn) == 100, "TimelineColumn is 100");
    expectations.expect(kit::px(kit::Size::TimelineWorkArea) == 10, "TimelineWorkArea is 10");
    expectations.expect(kit::px(kit::Size::TimelineWorkAreaHandle) == 6,
                        "TimelineWorkAreaHandle is 6");
    expectations.expect(kit::px(kit::Size::TimelineNavigatorThumb) == 10,
                        "TimelineNavigatorThumb is 10");
    expectations.expect(kit::px(kit::Size::ScrollBarThumb) == 8, "a resting thumb is 8");
    expectations.expect(kit::px(kit::Size::ScrollBarTrack) == 12,
                        "the scrollbar track is one constant 12");
    expectations.expect(kit::px(kit::Size::RadioDot) % 2 == 0 &&
                            kit::px(kit::Size::RadioMark) % 2 == 0,
                        "the radio dot and mark are even, so the dot centres on the pixel grid");
    // task S1, item 3: the narrowest a menu popup may be.
    expectations.expect(kit::px(kit::Size::MenuMinWidth) == 200, "MenuMinWidth is 200");
}

void testHairlinesLandOnWholePhysicalPixels(Expectations& expectations) {
    struct Case {
        qreal ratio;
        qreal expected;
    };
    // At 1.25x and 1.5x a plain 1.0-logical-pixel pen straddles two physical pixels; the snapped
    // width is the logical width whose physical width is a whole number of pixels.
    for (const auto& [ratio, expected] :
         {Case{1.0, 1.0}, Case{1.25, 0.8}, Case{1.5, 4.0 / 3.0}, Case{2.0, 1.0}, Case{3.0, 1.0}}) {
        const qreal snapped = kit::snappedHairlineWidth(ratio);
        expectations.expect(std::abs(snapped - expected) < 1e-9,
                            "snapped hairline width at the requested device pixel ratio");
        const qreal physical = snapped * ratio;
        expectations.expect(std::abs(physical - std::round(physical)) < 1e-9,
                            "the snapped hairline is a whole number of physical pixels");
        expectations.expect(physical >= 1.0, "a hairline never vanishes below one physical pixel");
    }
    expectations.expect(kit::snappedHairlineWidth(0.0) == kit::kHairlineWidth,
                        "a nonsensical device pixel ratio falls back to the design hairline");
}

void testElevationsCarryTheSpecifiedShadows(Expectations& expectations) {
    expectations.expect(kit::shadow(kit::Elevation::Flat).isFlat(), "Flat casts no shadow");

    const auto popup = kit::shadow(kit::Elevation::Popup);
    // Small and symmetric (owner, 2026-10-29): an offset, bottom-heavy shadow read as a chin under
    // the last row of every menu and dropdown.
    expectations.expect(popup.offsetX == 0 && popup.offsetY == 0 && popup.blurRadius == 8 &&
                            popup.color.alpha() == 115,
                        "Popup is 0 0 8 rgba(0,0,0,.45)");
    const auto dialog = kit::shadow(kit::Elevation::Dialog);
    expectations.expect(dialog.offsetX == 0 && dialog.offsetY == 12 && dialog.blurRadius == 40 &&
                            dialog.color.alpha() == 153,
                        "Dialog is 0 12 40 rgba(0,0,0,.6)");
    const auto drag = kit::shadow(kit::Elevation::Drag);
    expectations.expect(drag.offsetX == 0 && drag.offsetY == 18 && drag.blurRadius == 48 &&
                            drag.color.alpha() == 140,
                        "Drag is 0 18 48 rgba(0,0,0,.55)");
}

void testTypeRolesCarryTheSpecifiedFamiliesWeightsAndSizes(Expectations& expectations) {
    const auto ui = kit::font(kit::TypeRole::Ui);
    expectations.expect(ui.families().contains(kit::interfaceFontFamily()),
                        "the UI role asks for Inter");
    expectations.expect(ui.weight() == QFont::Medium, "the UI role is weight 500");

    const auto small = kit::font(kit::TypeRole::UiSmall);
    expectations.expect(small.capitalization() == QFont::MixedCase,
                        "the small UI role preserves Title Case");
    expectations.expect(small.letterSpacingType() == QFont::PercentageSpacing &&
                            std::abs(small.letterSpacing() - 100.0) < 1e-9,
                        "the small UI role uses natural tracking");
    expectations.expect(small.weight() == QFont::Medium, "the small UI role is weight 500");

    const auto value = kit::font(kit::TypeRole::Value);
    expectations.expect(value.families().contains(kit::monospaceFontFamily()),
                        "the value role asks for Geist Mono");
    expectations.expect(value.fixedPitch(), "the value role is fixed pitch");
    expectations.expect(value.weight() == QFont::Medium, "the value role is weight 500");

    const auto title = kit::font(kit::TypeRole::Title);
    expectations.expect(title.weight() == QFont::DemiBold, "the title role is weight 600");

    // Small emphasis is a role, never a call-site setWeight(): it names the SemiBold face, which a
    // DemiBold weight on the Medium family could never reach.
    const auto strong = kit::font(kit::TypeRole::UiSmallStrong);
    expectations.expect(strong.weight() == QFont::DemiBold, "UiSmallStrong is weight 600");
    expectations.expect(std::abs(strong.pointSizeF() - small.pointSizeF()) < 1e-9,
                        "UiSmallStrong is UiSmall's size");
    expectations.expect(strong.families().value(0) == title.families().value(0),
                        "UiSmallStrong asks for the same SemiBold face Title does");

    // 1 design pixel is 1 Qt logical pixel: the point sizes are the design pixel sizes converted
    // through the screen's own logical DPI, so their ratios are exactly the design ratios.
    expectations.expect(std::abs(ui.pointSizeF() / small.pointSizeF() - 12.0 / 10.5) < 1e-6,
                        "UI 12 and UISmall 10.5 keep their design ratio");
    expectations.expect(std::abs(title.pointSizeF() / value.pointSizeF() - 13.0 / 11.5) < 1e-6,
                        "Title 13 and Value 11.5 keep their design ratio");
}

void testMotionDurationsAndTheReducedMotionKillSwitch(Expectations& expectations) {
    expectations.expect(kit::motionEnabled(), "motion is enabled without the kill switch");
    expectations.expect(kit::durationMs(kit::Motion::Fast) == 80, "Fast is 80ms");
    expectations.expect(kit::durationMs(kit::Motion::Pop) == 140, "Pop is 140ms");
    expectations.expect(kit::durationMs(kit::Motion::Sweep) == 1600, "Sweep is 1600ms");
    expectations.expect(kit::durationMs(kit::Motion::None) == 0,
                        "editor feedback is never eased: None is 0ms");
    expectations.expect(kit::easing(kit::Motion::Fast).type() == QEasingCurve::OutCubic,
                        "Fast eases out");
    expectations.expect(kit::easing(kit::Motion::Pop).type() == QEasingCurve::OutCubic,
                        "Pop eases out");

    qputenv("BLOOM_REDUCED_MOTION", "1");
    expectations.expect(!kit::motionEnabled(), "the reduced-motion kill switch disables motion");
    expectations.expect(kit::durationMs(kit::Motion::Fast) == 0,
                        "every duration collapses to zero under reduced motion");
    expectations.expect(kit::durationMs(kit::Motion::Pop) == 0,
                        "every duration collapses to zero under reduced motion");
    qputenv("BLOOM_REDUCED_MOTION", "0");
    expectations.expect(kit::motionEnabled(), "an explicit 0 leaves motion enabled");
    qunsetenv("BLOOM_REDUCED_MOTION");
}

void testTheSurfaceStepsAreEachPerceptible(Expectations& expectations) {
    // WCAG relative-luminance contrast between neighbouring rungs. The previous ladder sat near
    // 1.02:1 per step, which is what made hover, selection and alternating rows invisible.
    const auto luminance = [](const QColor& value) {
        const auto channel = [](const double c) {
            return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * channel(static_cast<double>(value.redF())) +
               0.7152 * channel(static_cast<double>(value.greenF())) +
               0.0722 * channel(static_cast<double>(value.blueF()));
    };
    const auto contrast = [&](const kit::Color a, const kit::Color b) {
        const double la = luminance(kit::color(a));
        const double lb = luminance(kit::color(b));
        return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
    };
    expectations.expect(contrast(kit::Color::Background, kit::Color::Surface) >= 1.06,
                        "Surface is a visible step above Background");
    expectations.expect(contrast(kit::Color::Surface, kit::Color::SurfaceRaised) >= 1.1,
                        "SurfaceRaised is a visible step above Surface");
    expectations.expect(contrast(kit::Color::SurfaceRaised, kit::Color::Field) >= 1.1,
                        "Field is a visible step above SurfaceRaised");
    expectations.expect(contrast(kit::Color::BorderHover, kit::Color::BorderActive) >= 1.2,
                        "BorderActive is visibly distinct from BorderHover");
    expectations.expect(contrast(kit::Color::Accent, kit::Color::OnAccent) >= 4.5,
                        "OnAccent text on Accent meets WCAG AA");
    expectations.expect(contrast(kit::Color::CheckerLight, kit::Color::CheckerDark) >= 1.8,
                        "the checker tones are far enough apart to read as a grid");
    // The Viewer's deeper grid still reads as a grid, and its darker tone stays a clear step above
    // the Canvas surround so the composition frame edge reads against it.
    expectations.expect(contrast(kit::Color::ViewerCheckerLight, kit::Color::ViewerCheckerDark) >=
                            1.2,
                        "the Viewer checker tones read as a grid");
    expectations.expect(contrast(kit::Color::ViewerCheckerDark, kit::Color::Canvas) >= 1.2,
                        "the Viewer checker stays distinct from the Canvas surround");
    expectations.expect(kit::color(kit::Color::ViewerCheckerLight).lightness() <
                            kit::color(kit::Color::CheckerDark).lightness(),
                        "the Viewer checker is deeper than the small-swatch checker");

    // The selection wash is a blend toward Accent over its own surface, still a dark row.
    const QColor wash = kit::selectionWash(kit::Color::Surface);
    expectations.expect(wash.blue() > kit::color(kit::Color::Surface).blue() &&
                            wash.blue() < kit::color(kit::Color::Accent).blue(),
                        "the selection wash sits between its surface and Accent");
}

void testTokenValuesDoNotSilentlyDuplicateWithinAFamily(Expectations& expectations) {
    // The ladder and the border steps are each strictly increasing: a later edit cannot collapse
    // two rungs back into one value without failing here.
    const std::array ladder{kit::Color::Background, kit::Color::Surface, kit::Color::SurfaceRaised,
                            kit::Color::Field};
    for (std::size_t index = 1; index < ladder.size(); ++index)
        expectations.expect(kit::color(ladder.at(index)).lightness() >
                                kit::color(ladder.at(index - 1)).lightness(),
                            "the surface ladder strictly brightens");
    expectations.expect(kit::color(kit::Color::Border).lightness() <
                                kit::color(kit::Color::BorderHover).lightness() &&
                            kit::color(kit::Color::BorderHover).lightness() <
                                kit::color(kit::Color::BorderActive).lightness(),
                        "Border, BorderHover and BorderActive strictly brighten");
    expectations.expect(kit::color(kit::Color::Border).lightness() >
                            kit::color(kit::Color::SurfaceRaised).lightness(),
                        "a raised plate keeps a visible hairline");
}

void testDisabledInkIsFortyPercent(Expectations& expectations) {
    const QColor ink = kit::color(kit::Color::Foreground);
    const QColor disabled = kit::disabledInk(ink);
    expectations.expect(std::abs(kit::kDisabledOpacity - 0.40) < 1e-9, "disabled ink is 40%");
    expectations.expect(std::abs(static_cast<double>(disabled.alphaF()) - 0.40) < 1e-3,
                        "withOpacity applies the disabled opacity to the ink alpha");
    expectations.expect(disabled.rgb() == ink.rgb(), "withOpacity does not change the hue");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const QApplication application(argc, argv);
    Expectations expectations;
    testEveryColorRoleResolvesToItsSpecifiedValue(expectations);
    testSurfaceLadderStepsAndClamps(expectations);
    testGeometryTokensCarryTheSpecifiedNumbers(expectations);
    testHairlinesLandOnWholePhysicalPixels(expectations);
    testElevationsCarryTheSpecifiedShadows(expectations);
    testTypeRolesCarryTheSpecifiedFamiliesWeightsAndSizes(expectations);
    testMotionDurationsAndTheReducedMotionKillSwitch(expectations);
    testTheSurfaceStepsAreEachPerceptible(expectations);
    testTokenValuesDoNotSilentlyDuplicateWithinAFamily(expectations);
    testDisabledInkIsFortyPercent(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
