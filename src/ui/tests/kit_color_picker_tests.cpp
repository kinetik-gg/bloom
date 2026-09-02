#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_picker.hpp>
#include <bloom/ui/kit/color_sampler.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QApplication>
#include <QLineEdit>
#include <QMouseEvent>
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

[[nodiscard]] double widen(const float value) { return static_cast<double>(value); }

void sendMouse(QWidget& widget, const QEvent::Type type, const QPointF& point,
               const Qt::MouseButton button) {
    QMouseEvent event(type, point, widget.mapToGlobal(point), button,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &event);
    QCoreApplication::processEvents();
}

struct Fixture {
    QWidget host;
    kit::KColorPicker* picker = nullptr;

    Fixture() {
        auto* layout = new QVBoxLayout(&host);
        picker = new kit::KColorPicker(&host);
        layout->addWidget(picker);
        host.resize(picker->sizeHint());
        host.show();
        host.activateWindow();
        QCoreApplication::processEvents();
    }
};

void testSvSquareDragSetsSaturationValueAndPreservesHueThroughGray(Expectations& expectations) {
    Fixture fixture;
    auto& picker = *fixture.picker;
    picker.setColor(kit::KColor::fromHsva(300.0F, 0.8F, 0.8F));
    const float startHue = picker.color().toHsva()[0];

    const QRectF square = picker.svSquareRect();
    // First drag to the square's left edge -- zero saturation, a gray, where RGB alone cannot
    // name the hue at all.
    const QPointF grayPoint(square.left(), square.top() + square.height() * 0.5);
    sendMouse(picker, QEvent::MouseButtonPress, grayPoint, Qt::LeftButton);
    const auto grayHsva = picker.color().toHsva();
    expectations.expect(std::abs(widen(grayHsva[1])) < 0.02,
                        "dragging to the left edge zeroes saturation");

    // Then drag back into a saturated region without ever touching the hue bar.
    const QPointF target(square.left() + square.width() * 0.25,
                         square.top() + square.height() * 0.75);
    sendMouse(picker, QEvent::MouseMove, target, Qt::LeftButton);
    const auto hsva = picker.color().toHsva();
    expectations.expect(std::abs(widen(hsva[1]) - 0.25) < 0.02,
                        "saturation follows the pointer's x");
    expectations.expect(std::abs(widen(hsva[2]) - 0.25) < 0.02,
                        "value follows the pointer's y, inverted");
    expectations.expect(std::abs(widen(hsva[0]) - widen(startHue)) < 0.5,
                        "hue survives a pass through zero saturation: the picker holds it live "
                        "instead of re-deriving it from RGB");
    sendMouse(picker, QEvent::MouseButtonRelease, target, Qt::LeftButton);
}

void testHueBarDragMovesOnlyHue(Expectations& expectations) {
    Fixture fixture;
    auto& picker = *fixture.picker;
    picker.setColor(kit::KColor::fromHsva(0.0F, 0.6F, 0.6F, 0.9F));

    const QRectF bar = picker.hueBarRect();
    const QPointF target(bar.center().x(), bar.top() + bar.height() * (120.0 / 360.0));
    sendMouse(picker, QEvent::MouseButtonPress, target, Qt::LeftButton);
    const auto hsva = picker.color().toHsva();
    expectations.expect(std::abs(static_cast<double>(hsva[0]) - 120.0) < 2.0,
                        "the hue bar drag lands near the targeted hue");
    expectations.expect(std::abs(static_cast<double>(hsva[1]) - 0.6) < 0.02 &&
                            std::abs(static_cast<double>(hsva[2]) - 0.6) < 0.02,
                        "saturation and value are untouched by a hue-only drag");
    expectations.expect(std::abs(static_cast<double>(hsva[3]) - 0.9) < 0.02,
                        "alpha is untouched by a hue-only drag");
    sendMouse(picker, QEvent::MouseButtonRelease, target, Qt::LeftButton);
}

void testAlphaBarDragMovesOnlyAlpha(Expectations& expectations) {
    Fixture fixture;
    auto& picker = *fixture.picker;
    picker.setColor(kit::KColor::fromHsva(210.0F, 0.5F, 0.5F, 1.0F));

    const QRectF bar = picker.alphaBarRect();
    const QPointF target(bar.left() + bar.width() * 0.3, bar.center().y());
    sendMouse(picker, QEvent::MouseButtonPress, target, Qt::LeftButton);
    const auto hsva = picker.color().toHsva();
    expectations.expect(std::abs(static_cast<double>(hsva[3]) - 0.3) < 0.02,
                        "the alpha bar drag lands near the targeted alpha");
    expectations.expect(std::abs(static_cast<double>(hsva[0]) - 210.0) < 0.5 &&
                            std::abs(static_cast<double>(hsva[1]) - 0.5) < 0.02 &&
                            std::abs(static_cast<double>(hsva[2]) - 0.5) < 0.02,
                        "hue, saturation, and value are untouched by an alpha-only drag");
    sendMouse(picker, QEvent::MouseButtonRelease, target, Qt::LeftButton);
}

void testModelSwitchPreservesTheColorExactly(Expectations& expectations) {
    Fixture fixture;
    auto& picker = *fixture.picker;
    picker.setColor(kit::KColor::fromHsva(45.0F, 0.4F, 0.7F, 0.8F));
    const kit::KColor before = picker.color();

    picker.setColorModel(kit::ColorModel::Rgba);
    expectations.expect(picker.color() == before, "switching to RGBA does not alter the color");
    picker.setColorModel(kit::ColorModel::Hsla);
    expectations.expect(picker.color() == before, "switching to HSLA does not alter the color");
    picker.setColorModel(kit::ColorModel::Hsva);
    expectations.expect(picker.color() == before,
                        "switching back to HSVA does not alter the color either");
}

void testHexEntryRoundTrips(Expectations& expectations) {
    Fixture fixture;
    auto& picker = *fixture.picker;
    picker.hexField()->setText(QStringLiteral("#3399FFCC"));
    picker.hexField()->editingFinished();
    QCoreApplication::processEvents();

    expectations.expect(picker.hexField()->text() == QStringLiteral("#3399ffcc"),
                        "the hex field redisplays its own lowercase canonical text");
    const QColor rgba = picker.color().toQColor();
    expectations.expect(rgba == QColor(0x33, 0x99, 0xFF, 0xCC),
                        "the parsed hex value committed exactly, alpha included");

    picker.hexField()->setText(QStringLiteral("not-a-color"));
    picker.hexField()->editingFinished();
    QCoreApplication::processEvents();
    expectations.expect(picker.color().toQColor() == QColor(0x33, 0x99, 0xFF, 0xCC),
                        "an unparsable hex entry is refused, not silently applied");
    expectations.expect(picker.hexField()->text() == QStringLiteral("#3399ffcc"),
                        "a refused entry reverts the field to the last valid text");
}

void testChannelFieldEditUpdatesTheColorAndSwatch(Expectations& expectations) {
    Fixture fixture;
    auto& picker = *fixture.picker;
    picker.setColorModel(kit::ColorModel::Rgba);
    picker.setColor(kit::KColor::fromRgba(0.0F, 0.0F, 0.0F, 1.0F));

    kit::KValueField* redField = picker.channelField(0);
    expectations.expect(redField != nullptr, "the RGBA model exposes a red channel field");
    redField->setValue(200.0);

    const QColor rgba = picker.color().toQColor();
    expectations.expect(rgba.red() == 200, "editing the R channel field updates the color");
    expectations.expect(picker.swatchRow()->currentColor().toQColor().red() == 200,
                        "...and the swatch row's current-color preview along with it");
}

void testFormSelectorListsOnlyTheAvailableForm(Expectations& expectations) {
    Fixture fixture;
    auto& picker = *fixture.picker;
    expectations.expect(picker.formSelector()->count() == 1,
                        "only the implemented picker form is listed -- the rest are absent, not "
                        "shown disabled");
    expectations.expect(picker.formSelector()->currentText() == QStringLiteral("Square"),
                        "the one listed form is Square");
    expectations.expect(picker.pickerForm() == kit::PickerForm::Square,
                        "the picker itself reports the Square form");
}

void testEyedropperTogglesTheSamplerAndAppliesAPick(Expectations& expectations) {
    Fixture fixture;
    auto& picker = *fixture.picker;
    expectations.expect(picker.eyedropperButton() != nullptr,
                        "decision 5's UI affordance is present on the picker");
    expectations.expect(picker.eyedropperButton()->toolTip() ==
                            QString::fromUtf8(kit::kSamplerScopeTooltip),
                        "the eyedropper discloses its app-only scope in its own tooltip");
    expectations.expect(!picker.sampler()->isActive(), "the sampler starts inactive");

    picker.eyedropperButton()->setChecked(true);
    expectations.expect(picker.sampler()->isActive(), "checking the eyedropper begins sampling");

    // Emitted directly rather than driven through a simulated global mouse click: colorPicked is a
    // public Qt signal (Q_SIGNALS expands to `public`), and this proves the picker's own wiring --
    // KColorSampler's real click-to-globalPos path is covered in kit_color_sampler_tests.cpp.
    const kit::KColor sampled = kit::KColor::fromRgba(0.1F, 0.9F, 0.3F);
    Q_EMIT picker.sampler()->colorPicked(sampled);

    const kit::KColor pickedColor = picker.color();
    const bool closeEnough = std::abs(pickedColor.red - sampled.red) < 0.01F &&
                             std::abs(pickedColor.green - sampled.green) < 0.01F &&
                             std::abs(pickedColor.blue - sampled.blue) < 0.01F;
    expectations.expect(closeEnough, "a completed sample commits into the picker's color");
    expectations.expect(!picker.eyedropperButton()->isChecked(),
                        "a completed sample releases the eyedropper toggle");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    Expectations expectations;
    testSvSquareDragSetsSaturationValueAndPreservesHueThroughGray(expectations);
    testHueBarDragMovesOnlyHue(expectations);
    testAlphaBarDragMovesOnlyAlpha(expectations);
    testModelSwitchPreservesTheColorExactly(expectations);
    testHexEntryRoundTrips(expectations);
    testChannelFieldEditUpdatesTheColorAndSwatch(expectations);
    testFormSelectorListsOnlyTheAvailableForm(expectations);
    testEyedropperTogglesTheSamplerAndAppliesAPick(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
