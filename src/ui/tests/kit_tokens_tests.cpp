#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QString>

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
    expectHex(kit::Color::Surface, "#161616");
    expectHex(kit::Color::SurfaceRaised, "#1b1b1b");
    expectHex(kit::Color::Field, "#202020");
    expectHex(kit::Color::Foreground, "#ffffff");
    expectHex(kit::Color::Muted, "#999999");
    expectHex(kit::Color::Faint, "#666666");
    expectHex(kit::Color::Border, "#222222");
    expectHex(kit::Color::BorderHover, "#454545");
    expectHex(kit::Color::Accent, "#0c8ce9");
    expectHex(kit::Color::AccentHover, "#3aa5f0");
    expectHex(kit::Color::AccentPressed, "#0a73c2");
    expectHex(kit::Color::Keyframe, "#f5c542");
    expectHex(kit::Color::Ok, "#3fbf6b");
    expectHex(kit::Color::Warn, "#e6a23c");
    expectHex(kit::Color::Error, "#e0554e");
    expectHex(kit::Color::Brand, "#e879ab");
    expectHex(kit::Color::DataSequence, "#e0554e");
    expectHex(kit::Color::DataClip, "#3fbf6b");
    expectHex(kit::Color::DataComposition, "#8b5cf6");
    expectHex(kit::Color::DataImage, "#3aa5f0");
    expectHex(kit::Color::DataAudio, "#7c5cff");
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

    expectations.expect(kit::px(kit::Spacing::XXS) == 2, "Spacing::XXS is 2");
    expectations.expect(kit::px(kit::Spacing::XS) == 4, "Spacing::XS is 4");
    expectations.expect(kit::px(kit::Spacing::S) == 8, "Spacing::S is 8");
    expectations.expect(kit::px(kit::Spacing::M) == 12, "Spacing::M is 12");
    expectations.expect(kit::px(kit::Spacing::L) == 16, "Spacing::L is 16");
    expectations.expect(kit::px(kit::Spacing::XL) == 24, "Spacing::XL is 24");
    expectations.expect(kit::px(kit::Spacing::XXL) == 32, "Spacing::XXL is 32");
    expectations.expect(kit::px(kit::Spacing::Gutter) == 6, "Spacing::Gutter is 6");

    expectations.expect(kit::px(kit::Size::ControlCompact) == 22, "ControlCompact is 22");
    expectations.expect(kit::px(kit::Size::Control) == 26, "Control is 26");
    expectations.expect(kit::px(kit::Size::ControlRoomy) == 32, "ControlRoomy is 32");
    expectations.expect(kit::px(kit::Size::IconSmall) == 12, "IconSmall is 12");
    expectations.expect(kit::px(kit::Size::IconMedium) == 16, "IconMedium is 16");
    expectations.expect(kit::px(kit::Size::IconLarge) == 20, "IconLarge is 20");
    expectations.expect(kit::px(kit::Size::TitleBar) == 34, "TitleBar is 34");
    expectations.expect(kit::px(kit::Size::PanelHeader) == 30, "PanelHeader is 30");
    expectations.expect(kit::px(kit::Size::TimelineRow) == 34, "TimelineRow is 34");
    expectations.expect(kit::px(kit::Size::ScrollBar) == 8, "ScrollBar is 8");
    expectations.expect(kit::px(kit::Size::ScrollBarHover) == 12, "ScrollBar hover width is 12");
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
    expectations.expect(popup.offsetX == 0 && popup.offsetY == 4 && popup.blurRadius == 16 &&
                            popup.color.alpha() == 128,
                        "Popup is 0 4 16 rgba(0,0,0,.5)");
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
                        "the UI role asks for Plus Jakarta Sans");
    expectations.expect(ui.weight() == QFont::Medium, "the UI role is weight 500");

    const auto small = kit::font(kit::TypeRole::UiSmall);
    expectations.expect(small.capitalization() == QFont::AllUppercase,
                        "the small UI role is uppercase");
    expectations.expect(small.letterSpacingType() == QFont::PercentageSpacing &&
                            std::abs(small.letterSpacing() - 107.0) < 1e-9,
                        "the small UI role tracks +0.07em");
    expectations.expect(small.weight() == QFont::Medium, "the small UI role is weight 500");

    const auto value = kit::font(kit::TypeRole::Value);
    expectations.expect(value.families().contains(kit::monospaceFontFamily()),
                        "the value role asks for Geist Mono");
    expectations.expect(value.fixedPitch(), "the value role is fixed pitch");
    expectations.expect(value.weight() == QFont::Medium, "the value role is weight 500");

    const auto title = kit::font(kit::TypeRole::Title);
    expectations.expect(title.weight() == QFont::DemiBold, "the title role is weight 600");

    // 1 design pixel is 1 Qt logical pixel: the point sizes are the design pixel sizes converted
    // through the screen's own logical DPI, so their ratios are exactly the design ratios.
    expectations.expect(std::abs(ui.pointSizeF() / small.pointSizeF() - 12.5 / 11.0) < 1e-6,
                        "UI 12.5 and UISmall 11 keep their design ratio");
    expectations.expect(std::abs(title.pointSizeF() / value.pointSizeF() - 13.0 / 11.5) < 1e-6,
                        "Title 13 and Value 11.5 keep their design ratio");
}

void testMotionDurationsAndTheReducedMotionKillSwitch(Expectations& expectations) {
    expectations.expect(kit::motionEnabled(), "motion is enabled without the kill switch");
    expectations.expect(kit::durationMs(kit::Motion::Fast) == 80, "Fast is 80ms");
    expectations.expect(kit::durationMs(kit::Motion::Pop) == 140, "Pop is 140ms");
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

void testDisabledInkIsFortyPercent(Expectations& expectations) {
    const QColor ink = kit::color(kit::Color::Foreground);
    const QColor disabled = kit::withOpacity(ink, kit::kDisabledOpacity);
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
    testDisabledInkIsFortyPercent(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
