#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

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

[[nodiscard]] bool near(const QColor& left, const QColor& right, const int tolerance) {
    return std::abs(left.red() - right.red()) <= tolerance &&
           std::abs(left.green() - right.green()) <= tolerance &&
           std::abs(left.blue() - right.blue()) <= tolerance;
}

void testTheFilledRecipesAreDerivedFromTheAccentTriple(Expectations& expectations) {
    // The whole point of deriving hover and press from the accent triple is that every other
    // filled control reproduces the relation the tokens already state. If the derivation does not
    // reproduce the triple it was taken from, it is not a relation, it is a guess.
    const QColor accent = kit::color(kit::Color::Accent);
    expectations.expect(near(kit::hoverFillFor(accent), kit::color(kit::Color::AccentHover), 16),
                        "the derived hover fill reproduces AccentHover from Accent");
    expectations.expect(near(kit::pressedFillFor(accent), kit::color(kit::Color::AccentPressed), 4),
                        "the derived pressed fill reproduces AccentPressed from Accent");
    expectations.expect(kit::filledHoverBlend() > 0.0 && kit::filledHoverBlend() < 1.0,
                        "the hover blend is a real blend toward Foreground");
    expectations.expect(kit::filledPressedScale() > 0.0 && kit::filledPressedScale() < 1.0,
                        "the pressed scale darkens rather than lightens");
}

void testStateRecipesWalkTheSurfaceLadder(Expectations& expectations) {
    expectations.expect(kit::surfaceForState(kit::Color::Surface, kit::State::Hover) ==
                            kit::Color::SurfaceRaised,
                        "hover is one surface step up");
    expectations.expect(kit::surfaceForState(kit::Color::SurfaceRaised, kit::State::Pressed) ==
                            kit::Color::Surface,
                        "pressed is one surface step down");
    expectations.expect(kit::surfaceForState(kit::Color::Surface, kit::State::Disabled) ==
                            kit::Color::Surface,
                        "a disabled control has no hover response and does not move");
    expectations.expect(kit::borderForState(kit::State::Hover) == kit::Color::BorderHover,
                        "hover raises the border");
    expectations.expect(kit::borderForState(kit::State::Focused) == kit::Color::Accent,
                        "focus takes the accent border");
    expectations.expect(kit::inkForState(kit::Color::Muted, kit::State::Disabled).alphaF() <
                            kit::inkForState(kit::Color::Muted, kit::State::Normal).alphaF(),
                        "disabled ink fades");
}

void testTheStateMachineResolvesOneStateAtATime(Expectations& expectations) {
    // Focus needs a shown, active window: an unshown widget can never be the focus widget, so
    // asserting the focus branch without a host would only prove Qt refused the request.
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& button = *new kit::KButton(QStringLiteral("Render"), &host);
    layout->addWidget(&button);
    host.show();
    host.activateWindow();
    // Qt/platform combinations disagree about what a freshly shown window receives: one offscreen
    // build synthesizes an enter, another queues activation focus for the first focusable widget
    // (observed landing only on the SECOND event-loop pass). Rest is this test's baseline, so
    // settle the queue completely, then DRIVE the button to rest synchronously — and pump no
    // further events between the drive and the assertion.
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    button.clearFocus();
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(&button, &leave);
    expectations.expect(button.visualState() == kit::State::Normal, "a fresh button is at rest");

    button.setDown(true);
    expectations.expect(button.visualState() == kit::State::Pressed, "pressing wins over rest");
    button.setDown(false);

    button.setCheckable(true);
    button.setChecked(true);
    expectations.expect(button.visualState() == kit::State::Selected, "checking reads as selected");
    button.setChecked(false);
    button.setCheckable(false);

    button.setEnabled(false);
    button.setDown(true);
    expectations.expect(button.visualState() == kit::State::Disabled,
                        "disabled outranks every other state: a disabled control does not respond");
    button.setDown(false);
    button.setEnabled(true);

    button.setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(button.hasFocus(), "the button can take keyboard focus at all");
    expectations.expect(button.visualState() == kit::State::Focused, "focus is its own state");
    button.clearFocus();
    expectations.expect(button.visualState() == kit::State::Normal, "losing focus returns to rest");
}

void testEveryVariantHasADistinctRecipe(Expectations& expectations) {
    kit::KButton button(QStringLiteral("Render"));

    button.setVariant(kit::KButton::Variant::Primary);
    expectations.expect(button.fillForState(kit::State::Normal) == kit::color(kit::Color::Accent),
                        "Primary rests on Accent");
    expectations.expect(button.fillForState(kit::State::Hover) ==
                            kit::color(kit::Color::AccentHover),
                        "Primary hovers to AccentHover");
    expectations.expect(button.fillForState(kit::State::Pressed) ==
                            kit::color(kit::Color::AccentPressed),
                        "Primary presses to AccentPressed");

    button.setVariant(kit::KButton::Variant::Secondary);
    expectations.expect(button.fillForState(kit::State::Normal) ==
                            kit::color(kit::Color::SurfaceRaised),
                        "Secondary rests on a raised surface");
    expectations.expect(button.fillForState(kit::State::Hover) == kit::color(kit::Color::Field),
                        "Secondary hovers one surface step up");

    button.setVariant(kit::KButton::Variant::Ghost);
    expectations.expect(!button.fillForState(kit::State::Normal).isValid(),
                        "Ghost draws no surface at rest");
    expectations.expect(button.fillForState(kit::State::Hover).isValid(),
                        "Ghost gains a surface on hover");
    expectations.expect(button.inkForVisualState(kit::State::Normal) ==
                            kit::color(kit::Color::Muted),
                        "Ghost rests in muted ink");
    expectations.expect(button.inkForVisualState(kit::State::Hover) ==
                            kit::color(kit::Color::Foreground),
                        "Ghost takes full ink on hover");

    button.setVariant(kit::KButton::Variant::Danger);
    expectations.expect(button.inkForVisualState(kit::State::Normal) ==
                            kit::color(kit::Color::Error),
                        "Danger rests in error ink");
    expectations.expect(button.fillForState(kit::State::Hover) ==
                            kit::hoverFillFor(kit::color(kit::Color::Error)),
                        "Danger fills with error once the pointer commits");
    expectations.expect(button.inkForVisualState(kit::State::Hover) ==
                            kit::color(kit::Color::Foreground),
                        "Danger's committed ink is legible on its fill");
}

void testDisabledInkIsFortyPercentInEveryVariant(Expectations& expectations) {
    kit::KButton button(QStringLiteral("Render"));
    for (const auto variant : {kit::KButton::Variant::Primary, kit::KButton::Variant::Secondary,
                               kit::KButton::Variant::Ghost, kit::KButton::Variant::Danger}) {
        button.setVariant(variant);
        const QColor disabled = button.inkForVisualState(kit::State::Disabled);
        expectations.expect(
            std::abs(static_cast<double>(disabled.alphaF()) - kit::kDisabledOpacity) < 1e-3,
            "disabled ink is 40% in every variant");
    }
}

void testControlSizesAndTheOutsideFocusRing(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& button = *new kit::KButton(QStringLiteral("Render"), &host);
    layout->addWidget(&button);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();
    // The widget reserves one whole pixel per side for the 1.5-wide ring plus its own half-pen
    // overhang, which is why the reservation rounds up rather than truncating.
    const auto ringMargin = static_cast<int>(std::lround(kit::kFocusRingWidth)) * 2;

    button.setControlSize(kit::KButton::ControlSize::Compact);
    const int compact = button.sizeHint().height();
    button.setControlSize(kit::KButton::ControlSize::Default);
    const int normal = button.sizeHint().height();
    button.setControlSize(kit::KButton::ControlSize::Roomy);
    const int roomy = button.sizeHint().height();

    expectations.expect(compact == kit::px(kit::Size::ControlCompact) + ringMargin,
                        "the compact size is the compact control token plus the focus-ring margin");
    expectations.expect(normal == kit::px(kit::Size::Control) + ringMargin,
                        "the default size is the control token plus the focus-ring margin");
    expectations.expect(roomy == kit::px(kit::Size::ControlRoomy) + ringMargin,
                        "the roomy size is the roomy control token plus the focus-ring margin");

    // The ring is drawn in that reserved margin, so taking focus cannot change the size hint --
    // and therefore cannot shift anything laid out beside the button.
    const QSize unfocused = button.sizeHint();
    button.setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(button.hasFocus(), "the button really holds focus for this assertion");
    expectations.expect(button.sizeHint() == unfocused,
                        "focusing does not change the size hint: the ring is drawn outside");
    button.clearFocus();
}

void testAnIconButtonRendersBothGlyphAndLabel(Expectations& expectations) {
    kit::KButton labelled(kit::IconId::Play, QStringLiteral("Play"));
    expectations.expect(labelled.iconId().has_value(), "the icon is carried by id, not by pixmap");
    const QSize withIcon = labelled.sizeHint();
    labelled.setIconId(std::nullopt);
    expectations.expect(labelled.sizeHint().width() < withIcon.width(),
                        "the icon actually claims layout width");

    kit::KButton painted(kit::IconId::Play, QStringLiteral("Play"));
    painted.setVariant(kit::KButton::Variant::Primary);
    painted.resize(painted.sizeHint());
    const QPixmap rendered = painted.grab();
    expectations.expect(!rendered.isNull(), "the button renders offscreen");

    const QImage image = rendered.toImage();
    bool sawAccent = false;
    for (int y = 0; y < image.height() && !sawAccent; ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (near(image.pixelColor(x, y), kit::color(kit::Color::Accent), 2)) {
                sawAccent = true;
                break;
            }
        }
    }
    expectations.expect(sawAccent, "a Primary button really paints its accent fill");
}

void testGhostDangerOnHoverStaysGhostAtRest(Expectations& expectations) {
    // The title bar / panel-header close button convention (task U2, issue #118): flush with its
    // ghost siblings at rest, Error fill only once the pointer commits.
    kit::KButton button(kit::IconId::Close, QString{});
    button.setVariant(kit::KButton::Variant::Ghost);
    expectations.expect(!button.dangerOnHover(), "dangerOnHover defaults to false");

    button.setDangerOnHover(true);
    expectations.expect(button.dangerOnHover(), "the flag round-trips");

    expectations.expect(!button.fillForState(kit::State::Normal).isValid(),
                        "still no surface of its own at rest, exactly like plain Ghost");
    expectations.expect(button.inkForVisualState(kit::State::Normal) ==
                            kit::color(kit::Color::Muted),
                        "resting ink is still the ordinary ghost muted ink, not Error");

    expectations.expect(button.fillForState(kit::State::Hover) ==
                            kit::hoverFillFor(kit::color(kit::Color::Error)),
                        "hover fills with the Error recipe once dangerOnHover is set");
    expectations.expect(button.inkForVisualState(kit::State::Hover) ==
                            kit::color(kit::Color::Foreground),
                        "hover ink is legible on the Error fill");
    expectations.expect(button.fillForState(kit::State::Pressed) ==
                            kit::pressedFillFor(kit::color(kit::Color::Error)),
                        "pressed deepens the same Error recipe Danger itself uses");

    // Every other variant ignores the flag entirely: it is a Ghost-only opt-in.
    button.setVariant(kit::KButton::Variant::Secondary);
    expectations.expect(button.fillForState(kit::State::Hover) !=
                            kit::hoverFillFor(kit::color(kit::Color::Error)),
                        "dangerOnHover has no effect outside Variant::Ghost");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    Expectations expectations;
    testTheFilledRecipesAreDerivedFromTheAccentTriple(expectations);
    testStateRecipesWalkTheSurfaceLadder(expectations);
    testTheStateMachineResolvesOneStateAtATime(expectations);
    testEveryVariantHasADistinctRecipe(expectations);
    testDisabledInkIsFortyPercentInEveryVariant(expectations);
    testControlSizesAndTheOutsideFocusRing(expectations);
    testAnIconButtonRendersBothGlyphAndLabel(expectations);
    testGhostDangerOnHoverStaysGhostAtRest(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
