#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/color_picker.hpp>
#include <bloom/ui/kit/theme.hpp>

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QSignalSpy>
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

struct Fixture {
    QWidget host;
    kit::KColorChip* chip = nullptr;

    Fixture() {
        auto* layout = new QVBoxLayout(&host);
        chip = new kit::KColorChip(&host);
        layout->addWidget(chip);
        host.resize(120, 120);
        host.show();
        host.activateWindow();
        QCoreApplication::processEvents();
    }
};

void testChipCarriesItsColor(Expectations& expectations) {
    Fixture fixture;
    auto& chip = *fixture.chip;
    expectations.expect(chip.color() == kit::KColor{}, "a fresh chip starts at the default color");

    QSignalSpy changed(&chip, &kit::KColorChip::colorChanged);
    const kit::KColor red = kit::KColor::fromRgba(1.0F, 0.0F, 0.0F);
    chip.setColor(red);
    expectations.expect(chip.color() == red, "setColor takes the given color");
    expectations.expect(changed.count() == 1, "changing the color reports it exactly once");
    chip.setColor(red);
    expectations.expect(changed.count() == 1, "re-setting the same color reports nothing");
}

void testClickingTheChipOpensAndClosesThePicker(Expectations& expectations) {
    Fixture fixture;
    auto& chip = *fixture.chip;
    // Showing the host hands focus to its only focusable child (the chip), so resting has to be
    // established rather than assumed -- the same convention kit_dropdown_tests.cpp uses.
    chip.clearFocus();
    QCoreApplication::processEvents();
    expectations.expect(!chip.isPickerOpen(), "the popup starts closed");
    expectations.expect(chip.visualState() == kit::State::Normal, "a closed, unhovered chip rests");

    chip.openPicker();
    QCoreApplication::processEvents();
    expectations.expect(chip.isPickerOpen(), "openPicker() opens the popup");
    expectations.expect(chip.visualState() == kit::State::Pressed, "an open chip reads as pressed");
    expectations.expect(chip.picker()->objectName() == QStringLiteral("kColorPicker"),
                        "the popup is findable by name");

    chip.closePicker();
    QCoreApplication::processEvents();
    expectations.expect(!chip.isPickerOpen(), "closePicker() closes the popup");
}

void testPickingAColorInThePopupUpdatesTheChip(Expectations& expectations) {
    Fixture fixture;
    auto& chip = *fixture.chip;
    chip.setColor(kit::KColor::fromRgba(0.0F, 0.0F, 0.0F));
    chip.openPicker();
    QCoreApplication::processEvents();

    QSignalSpy changed(&chip, &kit::KColorChip::colorChanged);
    const kit::KColor picked = kit::KColor::fromRgba(0.2F, 0.6F, 0.9F, 0.5F);
    chip.picker()->setColor(picked);
    // Not a bit-exact comparison: the picker stores its live state as HSVA and setColor() round-
    // trips an arbitrary RGBA through it, which is only approximately reversible for a float that
    // did not itself come from an 8-bit source (kit_color_tests.cpp pins the exact-round-trip
    // cases). What this test is proving is that the popup's colorChanged signal really is wired
    // back to the chip, not that the value survives to the last float bit.
    const kit::KColor chipColor = chip.color();
    const bool closeEnough = std::abs(chipColor.red - picked.red) < 0.01F &&
                             std::abs(chipColor.green - picked.green) < 0.01F &&
                             std::abs(chipColor.blue - picked.blue) < 0.01F &&
                             std::abs(chipColor.alpha - picked.alpha) < 0.01F;
    expectations.expect(closeEnough,
                        "the popup's colorChanged signal is wired straight back to the chip");
    expectations.expect(changed.count() == 1, "the chip reports the picked color once");
}

void testChipRendersCheckerboardUnderAlpha(Expectations& expectations) {
    Fixture fixture;
    auto& chip = *fixture.chip;
    chip.resize(chip.sizeHint());
    QCoreApplication::processEvents();

    chip.setColor(kit::KColor::fromRgba(1.0F, 0.0F, 0.0F, 1.0F));
    QCoreApplication::processEvents();
    const QImage opaque = chip.grab().toImage();
    const QColor opaqueA = opaque.pixelColor(opaque.width() / 2, opaque.height() / 2);
    expectations.expect(opaqueA == QColor(255, 0, 0),
                        "a fully opaque chip paints a flat swatch, no checker showing through");

    chip.setColor(kit::KColor::fromRgba(1.0F, 0.0F, 0.0F, 0.4F));
    QCoreApplication::processEvents();
    const QImage translucent = chip.grab().toImage();
    // The checker cell is Spacing::S (8 logical px); at the chip's device pixel ratio, sampling
    // one cell in from the top-left interior and again a cell further right lands in two
    // different checker squares, which sit on different Surface/SurfaceRaised grounds under the
    // same translucent red -- so the two composited pixels must differ if the checker is really
    // being drawn, not just a flat fill.
    const qreal dpr = translucent.devicePixelRatio();
    const auto sampleAt = [&](const int logicalX, const int logicalY) {
        return translucent.pixelColor(static_cast<int>(logicalX * dpr),
                                      static_cast<int>(logicalY * dpr));
    };
    const QColor cellOne = sampleAt(4, 4);
    const QColor cellTwo = sampleAt(12, 4);
    expectations.expect(cellOne != cellTwo,
                        "a translucent chip's two neighboring checker cells composite to "
                        "different colors");
    expectations.expect(cellOne != QColor(255, 0, 0) && cellTwo != QColor(255, 0, 0),
                        "neither sampled pixel is the fully-opaque swatch color");
}

void testCircleShapeIsRoundNotSquare(Expectations& expectations) {
    Fixture fixture;
    auto& chip = *fixture.chip;
    chip.setShape(kit::KColorChip::Shape::Circle);
    chip.setColor(kit::KColor::fromRgba(0.0F, 1.0F, 0.0F));
    chip.resize(chip.sizeHint());
    QCoreApplication::processEvents();
    const QImage image = chip.grab().toImage();
    const qreal dpr = image.devicePixelRatio();
    const QColor corner = image.pixelColor(static_cast<int>(1 * dpr), static_cast<int>(1 * dpr));
    const QColor center =
        image.pixelColor(static_cast<int>(image.width() / 2), static_cast<int>(image.height() / 2));
    expectations.expect(center == QColor(0, 255, 0), "the circle's center is the swatch color");
    expectations.expect(corner != QColor(0, 255, 0),
                        "a circle chip's corner is not the swatch color -- it is round, not "
                        "square");
}

void testABPairSwapsTheTwoColors(Expectations& expectations) {
    kit::KColorABPair pair;
    const kit::KColor a = kit::KColor::fromRgba(1.0F, 0.0F, 0.0F);
    const kit::KColor b = kit::KColor::fromRgba(0.0F, 0.0F, 1.0F);
    pair.setColorA(a);
    pair.setColorB(b);
    QSignalSpy swapped(&pair, &kit::KColorABPair::swapped);
    pair.swap();
    expectations.expect(pair.colorA() == b, "A takes what B had");
    expectations.expect(pair.colorB() == a, "B takes what A had");
    expectations.expect(swapped.count() == 1, "swapping reports itself exactly once");
    expectations.expect(pair.chipA()->color() == pair.colorA(),
                        "chipA and colorA() agree -- the pair is not tracking a separate copy");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    Expectations expectations;
    testChipCarriesItsColor(expectations);
    testClickingTheChipOpensAndClosesThePicker(expectations);
    testPickingAColorInThePopupUpdatesTheChip(expectations);
    testChipRendersCheckerboardUnderAlpha(expectations);
    testCircleShapeIsRoundNotSquare(expectations);
    testABPairSwapsTheTwoColors(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
