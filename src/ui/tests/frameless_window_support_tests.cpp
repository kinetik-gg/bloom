#include <bloom/ui/frameless_window_support.hpp>

#include <QApplication>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPoint>
#include <QPointF>
#include <QWidget>

#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string>

// Task C1, item C1 (owner: "let OS handle the native window chrome for now"): native chrome is
// the only mode MainWindow builds now, so FramelessEdgeResizer is never constructed by the live
// application. It stays compiled for a possible future custom-chrome/CSD return, and this file is
// its own independent test coverage -- exercising the class directly, the same way
// kit_title_bar_tests.cpp already tests kit::TitleBar in isolation from MainWindow. Each test below
// calls the class's own eventFilterForTest() (frameless_window_support.hpp), a thin public
// passthrough to the protected QObject::eventFilter() override -- FramelessEdgeResizer is `final`,
// so a test subclass is not an option here.

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

QMouseEvent makeLeftPress(const QPoint& localPos) {
    return QMouseEvent(QEvent::MouseButtonPress, QPointF(localPos), QPointF(localPos),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
}

void testNonMousePressEventsAreIgnored(Expectations& expectations) {
    QMainWindow window;
    FramelessEdgeResizer resizer(window, 4);
    QEvent moveEvent(QEvent::Move);
    expectations.expect(!resizer.eventFilterForTest(&window, &moveEvent),
                        "a non-mouse-press event is never claimed");
}

void testAWatchedWidgetOutsideTheWindowIsIgnored(Expectations& expectations) {
    QMainWindow window;
    window.resize(400, 300);
    FramelessEdgeResizer resizer(window, 4);

    QWidget unrelated;
    unrelated.resize(50, 50);
    auto press = makeLeftPress(QPoint(1, 1));
    expectations.expect(!resizer.eventFilterForTest(&unrelated, &press),
                        "a widget that is not part of the resizer's own window is never claimed");
}

void testARightButtonPressIsIgnoredEvenNearAnEdge(Expectations& expectations) {
    QMainWindow window;
    window.resize(400, 300);
    window.show();
    FramelessEdgeResizer resizer(window, 4);

    QMouseEvent rightPress(QEvent::MouseButtonPress, QPointF(0, 10), QPointF(0, 10),
                           Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    expectations.expect(!resizer.eventFilterForTest(&window, &rightPress),
                        "only a left-button press is ever eligible for an edge resize");
    window.hide();
}

void testAMaximizedWindowNeverStartsAnEdgeResize(Expectations& expectations) {
    QMainWindow window;
    window.resize(400, 300);
    window.show();
    window.setWindowState(window.windowState() | Qt::WindowMaximized);
    QApplication::sendPostedEvents();
    FramelessEdgeResizer resizer(window, 4);

    auto press = makeLeftPress(QPoint(0, 10));
    expectations.expect(!resizer.eventFilterForTest(&window, &press),
                        "a maximized window has no edge left to grab");
    window.hide();
}

void testAPressWellInsideTheWindowIsIgnored(Expectations& expectations) {
    QMainWindow window;
    window.resize(400, 300);
    window.show();
    FramelessEdgeResizer resizer(window, 4);

    auto press = makeLeftPress(QPoint(200, 150));
    expectations.expect(!resizer.eventFilterForTest(&window, &press),
                        "a press in the interior, far from every edge, is never claimed");
    window.hide();
}

void testAPressWithinTheMarginOfAnEdgeIsClaimed(Expectations& expectations) {
    QMainWindow window;
    window.resize(400, 300);
    window.show();
    QApplication::sendPostedEvents();
    expectations.expect(window.windowHandle() != nullptr,
                        "the shown window really has a platform window handle");
    FramelessEdgeResizer resizer(window, 4);

    // (0, 10): well within the 4px margin of the left edge, away from the top/bottom corners.
    auto press = makeLeftPress(QPoint(0, 10));
    expectations.expect(resizer.eventFilterForTest(&window, &press),
                        "a press within the margin of an unmaximized window's own edge is claimed");
    window.hide();
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testNonMousePressEventsAreIgnored(expectations);
    testAWatchedWidgetOutsideTheWindowIsIgnored(expectations);
    testARightButtonPressIsIgnoredEvenNearAnEdge(expectations);
    testAMaximizedWindowNeverStartsAnEdgeResize(expectations);
    testAPressWellInsideTheWindowIsIgnored(expectations);
    testAPressWithinTheMarginOfAnEdgeIsClaimed(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
