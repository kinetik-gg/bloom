#include <bloom/ui/kit/color_sampler.hpp>

#include <QApplication>
#include <QImage>
#include <QPalette>
#include <QWidget>

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

void testSampleImagePixelReadsTheExactPixel(Expectations& expectations) {
    QImage image(4, 4, QImage::Format_ARGB32);
    image.fill(QColor(10, 20, 30, 255));
    image.setPixelColor(2, 1, QColor(200, 100, 50, 128));

    const auto sampled = kit::sampleImagePixel(image, QPoint(2, 1));
    expectations.expect(sampled.has_value() && sampled->toQColor() == QColor(200, 100, 50, 128),
                        "an in-bounds point samples the exact pixel written, alpha included");

    const auto background = kit::sampleImagePixel(image, QPoint(0, 0));
    expectations.expect(background.has_value() && background->toQColor() == QColor(10, 20, 30, 255),
                        "a different point samples a different, still-exact, pixel");

    expectations.expect(!kit::sampleImagePixel(image, QPoint(4, 0)).has_value(),
                        "a point at the image's width (one past the last column) is out of bounds");
    expectations.expect(!kit::sampleImagePixel(image, QPoint(-1, 0)).has_value(),
                        "a negative point is out of bounds");
    expectations.expect(!kit::sampleImagePixel(QImage(), QPoint(0, 0)).has_value(),
                        "a null image samples nothing rather than crashing");
}

void testSampleWidgetPixelGrabsTheWidgetItself(Expectations& expectations) {
    QWidget widget;
    widget.resize(40, 40);
    widget.setAutoFillBackground(true);
    QPalette palette = widget.palette();
    palette.setColor(QPalette::Window, QColor(30, 200, 90));
    widget.setPalette(palette);
    widget.show();
    QCoreApplication::processEvents();

    const auto sampled = kit::sampleWidgetPixel(widget, QPoint(20, 20));
    expectations.expect(sampled.has_value() && sampled->toQColor() == QColor(30, 200, 90),
                        "a point inside the widget samples its own painted background exactly");

    expectations.expect(!kit::sampleWidgetPixel(widget, QPoint(1000, 1000)).has_value(),
                        "a point outside the widget's own rect samples nothing");
}

void testSamplerIsScopedToTheApplicationsOwnWidgets(Expectations& expectations) {
    QWidget widget;
    widget.resize(40, 40);
    widget.setAutoFillBackground(true);
    QPalette palette = widget.palette();
    palette.setColor(QPalette::Window, QColor(240, 10, 10));
    widget.setPalette(palette);
    widget.move(50, 50);
    widget.show();
    widget.activateWindow();
    QCoreApplication::processEvents();

    kit::KColorSampler sampler;
    expectations.expect(!sampler.isActive(), "a fresh sampler is not active");

    const QPoint insideGlobal = widget.mapToGlobal(QPoint(20, 20));
    const auto sampled = sampler.sampleAt(insideGlobal);
    expectations.expect(sampled.has_value() && sampled->toQColor() == QColor(240, 10, 10),
                        "a global position over one of the application's own widgets resolves to "
                        "that widget's own painted color");

    // Nothing Bloom owns lives out here; the honesty requirement (decision 5) is exactly that this
    // returns nothing rather than reaching for whatever the real screen happens to show.
    const auto outside = sampler.sampleAt(QPoint(1'000'000, 1'000'000));
    expectations.expect(!outside.has_value(),
                        "a position with no application widget under it samples nothing");
}

void testBeginAndCancelToggleActiveState(Expectations& expectations) {
    kit::KColorSampler sampler;
    sampler.begin();
    expectations.expect(sampler.isActive(), "begin() activates the sampler");
    sampler.begin();
    expectations.expect(sampler.isActive(),
                        "beginning an already-active sampler is a harmless no-op");
    sampler.cancel();
    expectations.expect(!sampler.isActive(), "cancel() deactivates the sampler");
    sampler.cancel();
    expectations.expect(!sampler.isActive(), "cancelling an inactive sampler is a harmless no-op");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testSampleImagePixelReadsTheExactPixel(expectations);
    testSampleWidgetPixelGrabsTheWidgetItself(expectations);
    testSamplerIsScopedToTheApplicationsOwnWidgets(expectations);
    testBeginAndCancelToggleActiveState(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
