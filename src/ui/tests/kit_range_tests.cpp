#include <bloom/ui/kit/range_selector.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QKeyEvent>
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

void sendMouse(QWidget& widget, const QEvent::Type type, const QPointF& point,
               const Qt::MouseButton button) {
    QMouseEvent event(type, point, widget.mapToGlobal(point), button,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &event);
    QCoreApplication::processEvents();
}

void testTheSliderMapsValuesToGeometry(Expectations& expectations) {
    kit::KSlider slider;
    slider.setRange(0.0, 100.0);
    slider.resize(220, slider.sizeHint().height());
    QCoreApplication::processEvents();

    slider.setValue(0.0);
    expectations.expect(slider.normalizedValue() == 0.0, "the minimum normalizes to zero");
    expectations.expect(slider.fillRect().width() == 0.0,
                        "at the minimum the accent fill is empty, not a stub");

    slider.setValue(50.0);
    expectations.expect(std::abs(slider.normalizedValue() - 0.5) < 1e-9, "the midpoint normalizes");
    expectations.expect(std::abs(slider.fillRect().width() - slider.trackRect().width() / 2.0) <
                            1.0,
                        "the accent fill runs from the start of the track to the handle");
    expectations.expect(std::abs(slider.handleRect().center().x() - slider.fillRect().right()) <
                            1.0,
                        "the handle sits exactly where the fill ends");

    slider.setValue(100.0);
    expectations.expect(std::abs(slider.fillRect().width() - slider.trackRect().width()) < 1e-9,
                        "at the maximum the fill spans the whole track");

    slider.setValue(1000.0);
    expectations.expect(slider.value() == 100.0, "a value past the maximum clamps");
    slider.setValue(-5.0);
    expectations.expect(slider.value() == 0.0, "a value below the minimum clamps");
}

void testTheSliderTakesTheValueThePointerNames(Expectations& expectations) {
    kit::KSlider slider;
    slider.setRange(0.0, 100.0);
    slider.resize(220, slider.sizeHint().height());
    QCoreApplication::processEvents();

    QSignalSpy changed(&slider, &kit::KSlider::valueChanged);
    const QPointF quarter(slider.trackRect().left() + slider.trackRect().width() * 0.25,
                          slider.height() / 2.0);
    sendMouse(slider, QEvent::MouseButtonPress, quarter, Qt::LeftButton);
    expectations.expect(std::abs(slider.value() - 25.0) < 1.0,
                        "pressing the track takes the value there rather than nudging toward it");
    expectations.expect(slider.isDragging(), "the press begins a drag");
    expectations.expect(slider.visualState() == kit::State::Pressed,
                        "a dragging slider is pressed");

    const QPointF threeQuarters(slider.trackRect().left() + slider.trackRect().width() * 0.75,
                                slider.height() / 2.0);
    sendMouse(slider, QEvent::MouseMove, threeQuarters, Qt::LeftButton);
    expectations.expect(std::abs(slider.value() - 75.0) < 1.0, "dragging follows the pointer");

    sendMouse(slider, QEvent::MouseButtonRelease, threeQuarters, Qt::LeftButton);
    expectations.expect(!slider.isDragging(), "releasing ends the drag");
    expectations.expect(changed.count() >= 2, "each committed value was reported");

    // Direct editor feedback is never eased: the value is where the pointer is at the moment the
    // event arrives, with no animation to catch up.
    expectations.expect(kit::durationMs(kit::Motion::None) == 0,
                        "the scrub motion token is genuinely instantaneous");
}

void testTheSliderKeyboardSteps(Expectations& expectations) {
    kit::KSlider slider;
    slider.setRange(0.0, 100.0);
    slider.setValue(50.0);
    slider.resize(220, slider.sizeHint().height());

    QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QCoreApplication::sendEvent(&slider, &right);
    expectations.expect(std::abs(slider.value() - 51.0) < 1e-9, "an arrow key steps one percent");

    QKeyEvent pageUp(QEvent::KeyPress, Qt::Key_PageUp, Qt::NoModifier);
    QCoreApplication::sendEvent(&slider, &pageUp);
    expectations.expect(std::abs(slider.value() - 61.0) < 1e-9,
                        "a page key steps ten times as far");

    QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
    QCoreApplication::sendEvent(&slider, &home);
    expectations.expect(slider.value() == 0.0, "Home goes to the minimum");

    QKeyEvent end(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
    QCoreApplication::sendEvent(&slider, &end);
    expectations.expect(slider.value() == 100.0, "End goes to the maximum");
}

void testTheSliderStateMachine(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& slider = *new kit::KSlider(&host);
    layout->addWidget(&slider);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();

    slider.setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(slider.visualState() == kit::State::Focused, "focus is its own state");
    slider.clearFocus();

    slider.setEnabled(false);
    expectations.expect(slider.visualState() == kit::State::Disabled, "disabled outranks the rest");
    const double before = slider.value();
    sendMouse(slider, QEvent::MouseButtonPress,
              QPointF(slider.width() - 2.0, slider.height() / 2.0), Qt::LeftButton);
    expectations.expect(slider.value() == before,
                        "a disabled slider does not respond to a press at all");
    slider.setEnabled(true);
}

void testTheRangeHandlesNeverCross(Expectations& expectations) {
    kit::KRangeSelector range;
    range.setRange(0.0, 100.0);
    range.setValues(20.0, 80.0);
    range.resize(320, range.sizeHint().height());
    QCoreApplication::processEvents();

    expectations.expect(range.lowerValue() == 20.0 && range.upperValue() == 80.0,
                        "the range takes the span it was given");

    range.setLowerValue(95.0);
    expectations.expect(range.lowerValue() == 80.0 && range.upperValue() == 80.0,
                        "pushing the lower handle past the upper clamps it to the upper rather "
                        "than swapping them");
    range.setValues(20.0, 80.0);
    range.setUpperValue(5.0);
    expectations.expect(range.upperValue() == 20.0 && range.lowerValue() == 20.0,
                        "pushing the upper handle past the lower clamps it too");

    range.setValues(20.0, 80.0);
    range.setLowerValue(-10.0);
    expectations.expect(range.lowerValue() == 0.0, "the lower handle clamps at the minimum");
    range.setUpperValue(200.0);
    expectations.expect(range.upperValue() == 100.0, "the upper handle clamps at the maximum");
}

void testTheRangeSpanAndHandleGeometry(Expectations& expectations) {
    kit::KRangeSelector range;
    range.setRange(0.0, 100.0);
    range.setValues(25.0, 75.0);
    range.resize(320, range.sizeHint().height());
    QCoreApplication::processEvents();

    const QRectF track = range.trackRect();
    const QRectF span = range.spanRect();
    expectations.expect(std::abs(span.width() - track.width() * 0.5) < 1.0,
                        "the accent span covers exactly the selected half of the range");
    expectations.expect(span.left() > track.left() && span.right() < track.right(),
                        "the span sits inside the track, with field on both sides");
    expectations.expect(std::abs(range.handleRect(kit::KRangeSelector::Handle::Lower).center().x() -
                                 span.left()) < 1.0,
                        "the lower handle marks the start of the span");
    expectations.expect(std::abs(range.handleRect(kit::KRangeSelector::Handle::Upper).center().x() -
                                 span.right()) < 1.0,
                        "the upper handle marks the end of the span");
    expectations.expect(range.handleRect(kit::KRangeSelector::Handle::None).isNull(),
                        "there is no rectangle for no handle");
}

void testDraggingARangeHandleGrabsTheNearerOne(Expectations& expectations) {
    kit::KRangeSelector range;
    range.setRange(0.0, 100.0);
    range.setValues(20.0, 80.0);
    range.resize(320, range.sizeHint().height());
    QCoreApplication::processEvents();

    QSignalSpy lowerChanged(&range, &kit::KRangeSelector::lowerValueChanged);
    QSignalSpy upperChanged(&range, &kit::KRangeSelector::upperValueChanged);

    const QRectF track = range.trackRect();
    // A press on bare track near the lower handle moves the lower handle, not the upper: the
    // artist pointed at the boundary they want moved.
    const QPointF nearLower(track.left() + track.width() * 0.30, range.height() / 2.0);
    sendMouse(range, QEvent::MouseButtonPress, nearLower, Qt::LeftButton);
    expectations.expect(range.grabbedHandle() == kit::KRangeSelector::Handle::Lower,
                        "the nearer handle is the one grabbed");
    expectations.expect(std::abs(range.lowerValue() - 30.0) < 1.5, "and it moves to the pointer");
    expectations.expect(range.upperValue() == 80.0, "the other handle stays where it was");
    expectations.expect(range.visualState() == kit::State::Pressed,
                        "a range with a grabbed handle reads as pressed");

    sendMouse(range, QEvent::MouseMove,
              QPointF(track.left() + track.width() * 0.10, range.height() / 2.0), Qt::LeftButton);
    expectations.expect(std::abs(range.lowerValue() - 10.0) < 1.5,
                        "the grabbed handle follows the pointer");
    sendMouse(range, QEvent::MouseButtonRelease,
              QPointF(track.left() + track.width() * 0.10, range.height() / 2.0), Qt::LeftButton);
    expectations.expect(range.grabbedHandle() == kit::KRangeSelector::Handle::None,
                        "releasing lets the handle go");

    expectations.expect(lowerChanged.count() >= 2, "every lower move was reported");
    expectations.expect(upperChanged.count() == 0, "the untouched handle reported nothing at all");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    Expectations expectations;
    testTheSliderMapsValuesToGeometry(expectations);
    testTheSliderTakesTheValueThePointerNames(expectations);
    testTheSliderKeyboardSteps(expectations);
    testTheSliderStateMachine(expectations);
    testTheRangeHandlesNeverCross(expectations);
    testTheRangeSpanAndHandleGeometry(expectations);
    testDraggingARangeHandleGrabsTheNearerOne(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
