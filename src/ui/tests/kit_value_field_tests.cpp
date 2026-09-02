#include <bloom/ui/kit/fonts.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QApplication>
#include <QFontInfo>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QVBoxLayout>
#include <QWheelEvent>
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

void clickAt(QWidget& widget, const QPointF& point) {
    QMouseEvent press(QEvent::MouseButtonPress, point, widget.mapToGlobal(point), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &press);
    QCoreApplication::processEvents();
}

void testTheCellCarriesAMonospacedNumberAndAUnit(Expectations& expectations) {
    kit::KValueField field;
    field.setLabel(QStringLiteral("Opacity"));
    field.setUnit(QStringLiteral("%"));
    field.setRange(0.0, 100.0);
    field.setDecimals(1);
    field.setValue(42.5);
    field.resize(field.sizeHint());
    QCoreApplication::processEvents();

    expectations.expect(field.label() == QStringLiteral("Opacity"), "the label survives");
    expectations.expect(field.unit() == QStringLiteral("%"), "the unit survives");
    expectations.expect(field.displayedValue() == QStringLiteral("42.5"),
                        "the number prints at the configured precision, with no unit mixed in");

    field.setDecimals(3);
    expectations.expect(field.displayedValue() == QStringLiteral("42.500"),
                        "precision is the field's, not the value's");

    // Every numeric surface in Bloom uses the monospaced Value role, so a column of them stays
    // aligned and a changing digit does not reflow the text beside it.
    expectations.expect(QFontInfo(kit::font(kit::TypeRole::Value)).fixedPitch(),
                        "the value role really is a fixed-pitch face");
    expectations.expect(
        QFontInfo(kit::font(kit::TypeRole::Value)).family().startsWith(kit::monospaceFontFamily()),
        "and it is the bundled monospaced family");
}

void testTheFieldDoesNotResizeAsDigitsChange(Expectations& expectations) {
    kit::KValueField field;
    field.setRange(0.0, 1000.0);
    field.setDecimals(2);
    field.setValue(0.0);
    const QSize narrowValue = field.sizeHint();
    field.setValue(999.99);
    expectations.expect(field.sizeHint() == narrowValue,
                        "the field is sized for the widest number its range can produce, so it "
                        "does not resize as digits change");
}

void testGeometryPlacesTheLabelCellAndSteppers(Expectations& expectations) {
    kit::KValueField field;
    field.setLabel(QStringLiteral("Opacity"));
    field.resize(field.sizeHint());
    QCoreApplication::processEvents();

    expectations.expect(field.labelRect().isValid() && field.labelRect().left() == 0.0,
                        "the label claims the leading column");
    expectations.expect(field.cellRect().left() > field.labelRect().right(),
                        "the cell begins after the label, not on top of it");
    expectations.expect(field.stepUpRect().right() <= field.cellRect().right() + 0.01,
                        "the steppers live inside the cell");
    expectations.expect(field.stepUpRect().bottom() <= field.stepDownRect().top() + 0.01,
                        "the up stepper is above the down stepper");
    expectations.expect(!field.stepUpRect().intersects(field.stepDownRect()),
                        "the two steppers do not overlap, so a click means one thing");

    kit::KValueField unlabelled;
    unlabelled.resize(unlabelled.sizeHint());
    expectations.expect(unlabelled.labelRect().isNull(),
                        "a field with no label claims no label column");
    expectations.expect(unlabelled.cellRect().left() == 0.0,
                        "and its cell starts at the control's edge");
}

void testSteppersAndKeysStepTheValue(Expectations& expectations) {
    kit::KValueField field;
    field.setRange(0.0, 100.0);
    field.setSingleStep(0.5);
    field.setValue(10.0);
    field.resize(field.sizeHint());
    QCoreApplication::processEvents();

    QSignalSpy changed(&field, &kit::KValueField::valueChanged);

    clickAt(field, field.stepUpRect().center());
    expectations.expect(field.value() == 10.5, "the up stepper adds one step");
    clickAt(field, field.stepDownRect().center());
    expectations.expect(field.value() == 10.0, "the down stepper removes one step");
    expectations.expect(changed.count() == 2, "each step was reported once");

    QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QCoreApplication::sendEvent(&field, &up);
    expectations.expect(field.value() == 10.5, "the up arrow steps too");
    QKeyEvent pageUp(QEvent::KeyPress, Qt::Key_PageUp, Qt::NoModifier);
    QCoreApplication::sendEvent(&field, &pageUp);
    expectations.expect(field.value() == 15.5, "a page key steps ten at a time");

    field.setValue(100.0);
    field.stepBy(1);
    expectations.expect(field.value() == 100.0, "stepping past the maximum clamps");
    field.setValue(0.0);
    field.stepBy(-1);
    expectations.expect(field.value() == 0.0, "stepping past the minimum clamps");
}

void testTheWheelIsIgnoredUntilTheFieldIsFocused(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& field = *new kit::KValueField(&host);
    layout->addWidget(&field);
    field.setRange(0.0, 100.0);
    field.setSingleStep(1.0);
    field.setValue(50.0);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();
    field.clearFocus();
    QCoreApplication::processEvents();

    const auto sendWheel = [&field] {
        QWheelEvent wheel(QPointF(field.width() / 2.0, field.height() / 2.0),
                          field.mapToGlobal(QPointF(field.width() / 2.0, field.height() / 2.0)),
                          QPoint(0, 0), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&field, &wheel);
        QCoreApplication::processEvents();
    };

    sendWheel();
    expectations.expect(field.value() == 50.0,
                        "a wheel over an unfocused field belongs to whatever is scrolling behind "
                        "it, and must not silently change a parameter");

    field.setFocus(Qt::MouseFocusReason);
    QCoreApplication::processEvents();
    sendWheel();
    expectations.expect(field.value() == 51.0, "once focused the wheel steps the value");
}

void testTheStateMachineAndDisabledField(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& field = *new kit::KValueField(&host);
    layout->addWidget(&field);
    field.setRange(0.0, 100.0);
    field.setValue(10.0);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();

    field.clearFocus();
    QCoreApplication::processEvents();
    expectations.expect(field.visualState() == kit::State::Normal,
                        "an unfocused, unhovered field rests");
    field.setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(field.visualState() == kit::State::Focused, "focus is its own state");

    field.setEnabled(false);
    expectations.expect(field.visualState() == kit::State::Disabled, "disabled outranks the rest");
    clickAt(field, field.stepUpRect().center());
    expectations.expect(field.value() == 10.0, "a disabled field's steppers do not respond at all");
    field.setEnabled(true);
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    Expectations expectations;
    testTheCellCarriesAMonospacedNumberAndAUnit(expectations);
    testTheFieldDoesNotResizeAsDigitsChange(expectations);
    testGeometryPlacesTheLabelCellAndSteppers(expectations);
    testSteppersAndKeysStepTheValue(expectations);
    testTheWheelIsIgnoredUntilTheFieldIsFocused(expectations);
    testTheStateMachineAndDisabledField(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
