// Lane B: the value field's gesture engine, typed entry and presentation -- modifier re-basing,
// the threshold dead zone, shortest formatting, transport-key pass-through, wheel grouping, Tab
// traversal in a plain widget tree, int64 exactness, narrow-width display, the mixed state,
// default/reset, grab loss and accessibility -- and the Qt-free expression parser behind typing.

#include <bloom/ui/kit/inline_edit.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_expression.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QAccessible>
#include <QApplication>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>
#include <QWidget>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <source_location>
#include <string>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

using namespace bloom::ui;
using kit::ValueExpressionError;

void mouse(QWidget& widget, const QEvent::Type type, const QPointF& point,
           const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const auto buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    const auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, point, widget.mapToGlobal(point), button, buttons, modifiers);
    QCoreApplication::sendEvent(&widget, &event);
    QCoreApplication::processEvents();
}

void key(QWidget& widget, const Qt::Key code, const Qt::KeyboardModifiers modifiers = {},
         const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, code, modifiers, text);
    QCoreApplication::sendEvent(&widget, &press);
    QCoreApplication::processEvents();
}

void wheel(kit::KValueField& field, const int deltaY, const Qt::KeyboardModifiers modifiers = {},
           const int deltaX = 0) {
    const QPointF centre = field.cellRect().center();
    QWheelEvent event(centre, field.mapToGlobal(centre), QPoint(), QPoint(deltaX, deltaY),
                      Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&field, &event);
    QCoreApplication::processEvents();
}

kit::ValueExpressionResult evaluate(const char* text, const char* unit = "", double current = 0) {
    kit::ValueExpressionContext context;
    context.unit = unit;
    context.current = current;
    return kit::evaluateValueExpression(text, context);
}

kit::ValueExpressionResult integer(const char* text, std::int64_t current = 0) {
    kit::ValueExpressionContext context;
    context.currentInteger = current;
    return kit::evaluateIntegerExpression(text, context);
}

void testTheExpressionParser(Expectations& expectations) {
    expectations.expect(evaluate("42").value == 42 && evaluate("-3.5").value == -3.5,
                        "plain numbers parse");
    expectations.expect(evaluate("0,5").value == 0.5 && evaluate(".25").value == 0.25,
                        "either decimal separator is accepted");
    expectations.expect(evaluate("0.1").value == 0.1, "a literal is the nearest double");
    expectations.expect(evaluate("1e3").value == 1000 && evaluate("2.5E-1").value == 0.25,
                        "exponents parse");
    expectations.expect(evaluate("50%", "%").value == 50 && evaluate("10 px", "px").value == 10 &&
                            evaluate("10PX", "px").value == 10,
                        "the field's own unit is accepted, ignoring ASCII case");
    expectations.expect(evaluate("90\xc2\xb0", "\xc2\xb0").value == 90,
                        "a UTF-8 unit such as the degree sign is accepted");
    const auto foreign = evaluate("10cm", "px");
    expectations.expect(foreign.error == ValueExpressionError::UnknownUnit && foreign.unit == "cm",
                        "any other unit is refused, by name");
    expectations.expect(evaluate("10px", "").error == ValueExpressionError::UnknownUnit,
                        "a unitless field refuses a unit");
    expectations.expect(evaluate("1920/2").value == 960 && evaluate("(3 + 4) * 2").value == 14 &&
                            evaluate("2 + 3 * 4").value == 14 && evaluate("-(2 + 3)").value == -5,
                        "arithmetic follows the usual precedence");
    expectations.expect(evaluate("(10 + 5) px", "px").value == 15 &&
                            evaluate("10px + 5px", "px").value == 15,
                        "a unit may follow a group or each number");
    expectations.expect(evaluate("+= 10", "", 5).value == 15 &&
                            evaluate("-=2", "", 5).value == 3 &&
                            evaluate("*= 3", "", 5).value == 15 &&
                            evaluate("/= 2", "", 5).value == 2.5,
                        "relative entries apply to the current value");
    expectations.expect(evaluate("+10", "", 5).value == 10,
                        "a leading plus is a sign, not a relative entry");
    expectations.expect(evaluate("").error == ValueExpressionError::Empty &&
                            evaluate("  ").error == ValueExpressionError::Empty,
                        "empty text is its own refusal");
    expectations.expect(evaluate("abc").error != ValueExpressionError::None &&
                            evaluate("1 +").error == ValueExpressionError::Syntax &&
                            evaluate("(1").error == ValueExpressionError::Syntax &&
                            evaluate("1 2").error == ValueExpressionError::Syntax,
                        "malformed text is refused, never zero");
    expectations.expect(evaluate("1/0").error == ValueExpressionError::DivideByZero,
                        "division by zero is refused");
    expectations.expect(evaluate("1e308 * 10").error == ValueExpressionError::NotFinite,
                        "an overflow to infinity is refused");

    // Integers are exact across the whole int64 range: nothing goes through a double.
    expectations.expect(integer("9007199254740993").integer == 9007199254740993LL,
                        "an integer above 2^53 parses exactly");
    expectations.expect(integer("9223372036854775807").integer ==
                                std::numeric_limits<std::int64_t>::max() &&
                            integer("-9223372036854775808").integer ==
                                std::numeric_limits<std::int64_t>::min(),
                        "both int64 extremes parse");
    expectations.expect(integer("9223372036854775808").error == ValueExpressionError::Overflow &&
                            integer("9223372036854775807 + 1").error ==
                                ValueExpressionError::Overflow &&
                            integer("4611686018427387904 * 2").error ==
                                ValueExpressionError::Overflow,
                        "integer overflow is refused, not wrapped");
    expectations.expect(integer("7/2").error == ValueExpressionError::NotInteger &&
                            integer("1.5").error == ValueExpressionError::NotInteger &&
                            integer("12.0").integer == 12 && integer("8/2").integer == 4,
                        "an integer division must come out whole");
    expectations.expect(integer("+= 1", 9007199254740992LL).integer == 9007199254740993LL,
                        "a relative integer entry stays exact");
}

void testScrubRebaseAndThreshold(Expectations& expectations) {
    QWidget host;
    auto* layout = new QHBoxLayout(&host);
    auto& field = *new kit::KValueField(&host);
    layout->addWidget(&field);
    field.setRange(-100000.0, 100000.0);
    field.setValue(0.0);
    host.show();
    QCoreApplication::processEvents();
    // A newly shown window hands focus to its first Tab stop; start from none.
    field.clearFocus();
    const int threshold = QApplication::startDragDistance();
    const QPointF start = field.cellRect().center();

    mouse(field, QEvent::MouseButtonPress, start);
    mouse(field, QEvent::MouseMove, start + QPointF(threshold + 1, 0));
    expectations.expect(field.isScrubbing() && field.value() == 1.0,
                        "the first scrub frame moves one step, not the whole threshold, got " +
                            std::to_string(field.value()));
    mouse(field, QEvent::MouseMove, start + QPointF(threshold + 30, 0));
    expectations.expect(field.value() == 30.0, "then one step per pixel");
    // Shift pressed mid-drag: the next segment starts where the pointer is.
    mouse(field, QEvent::MouseMove, start + QPointF(threshold + 40, 0), Qt::ShiftModifier);
    expectations.expect(field.value() == 40.0,
                        "pressing Shift mid-drag re-bases instead of multiplying the whole drag, "
                        "got " +
                            std::to_string(field.value()));
    mouse(field, QEvent::MouseMove, start + QPointF(threshold + 45, 0), Qt::ShiftModifier);
    expectations.expect(field.value() == 90.0, "and the new segment runs at ten steps a pixel");
    // The move that reports the modifier change still travelled at the old scale.
    mouse(field, QEvent::MouseMove, start + QPointF(threshold + 46, 0));
    expectations.expect(field.value() == 100.0, "releasing Shift re-bases again without a jump");
    mouse(field, QEvent::MouseMove, start + QPointF(threshold + 50, 0));
    expectations.expect(field.value() == 104.0, "back at one step a pixel");
    mouse(field, QEvent::MouseButtonRelease, start + QPointF(threshold + 50, 0));
    expectations.expect(!field.isScrubbing(), "the release ends the scrub");
    expectations.expect(!field.hasFocus(),
                        "a scrub does not leave keyboard focus on the field, so frame stepping "
                        "keeps working");
}

void testGrabLossFinishesTheScrub(Expectations& expectations) {
    QWidget host;
    auto* layout = new QHBoxLayout(&host);
    auto& field = *new kit::KValueField(&host);
    layout->addWidget(&field);
    field.setRange(-1000.0, 1000.0);
    host.show();
    QCoreApplication::processEvents();
    QSignalSpy finished(&field, &kit::KValueField::editFinished);
    QSignalSpy scrubFinished(&field, &kit::KValueField::scrubFinished);
    const QPointF start = field.cellRect().center();
    mouse(field, QEvent::MouseButtonPress, start);
    mouse(field, QEvent::MouseMove, start + QPointF(40, 0));
    QEvent ungrab(QEvent::UngrabMouse);
    QCoreApplication::sendEvent(&field, &ungrab);
    expectations.expect(!field.isScrubbing() && !field.hasLiveEdit() && finished.count() == 1 &&
                            scrubFinished.count() == 1,
                        "losing the pointer grab mid-scrub commits it as one edit");
    const double kept = field.value();
    mouse(field, QEvent::MouseMove, start + QPointF(80, 0));
    expectations.expect(field.value() == kept, "and later motion no longer scrubs");

    mouse(field, QEvent::MouseButtonPress, start);
    mouse(field, QEvent::MouseMove, start + QPointF(-40, 0));
    QEvent deactivate(QEvent::WindowDeactivate);
    QCoreApplication::sendEvent(&field, &deactivate);
    expectations.expect(!field.isScrubbing() && finished.count() == 2,
                        "so does the window losing activation (Alt-Tab)");
}

void testShortestFormatting(Expectations& expectations) {
    kit::KValueField field;
    field.setRange(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::max());
    field.setValue(0.1);
    expectations.expect(field.editText() == QStringLiteral("0.1"),
                        "the editor shows 0.1, not 17 digits of floating-point noise, got " +
                            field.editText().toStdString());
    field.setValue(0.1 + 0.2);
    expectations.expect(field.editText().toDouble() == 0.1 + 0.2,
                        "the shortest text still reads back to the exact double");
    field.setValue(1920.0);
    expectations.expect(field.editText() == QStringLiteral("1920"), "a whole number is bare");
    field.setValue(-0.0);
    expectations.expect(field.editText() == QStringLiteral("0"), "negative zero reads as zero");
}

void testTransportKeysPassThrough(Expectations& expectations) {
    QWidget host;
    auto* layout = new QHBoxLayout(&host);
    auto& field = *new kit::KValueField(&host);
    layout->addWidget(&field);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();
    field.clearFocus();
    const QPointF centre = field.cellRect().center();
    mouse(field, QEvent::MouseButtonPress, centre);
    expectations.expect(!field.hasFocus(), "a press does not take keyboard focus");
    mouse(field, QEvent::MouseButtonRelease, centre);
    expectations.expect(field.isEditing(), "the click opens the editor");
    QKeyEvent inEditor(QEvent::ShortcutOverride, Qt::Key_Escape, Qt::NoModifier);
    inEditor.ignore();
    QCoreApplication::sendEvent(field.lineEdit(), &inEditor);
    expectations.expect(inEditor.isAccepted(),
                        "the open editor claims Escape, so Cancel RAM Preview cannot fire under it");
    key(*field.lineEdit(), Qt::Key_Return);
    for (const auto code : {Qt::Key_Left, Qt::Key_Right, Qt::Key_Home, Qt::Key_End}) {
        QKeyEvent transport(QEvent::ShortcutOverride, code, Qt::NoModifier);
        transport.ignore();
        QCoreApplication::sendEvent(&field, &transport);
        expectations.expect(!transport.isAccepted(),
                            "after the edit closes, the transport keys reach the window again");
    }
}

void testTheWheelGroupsAndAccumulates(Expectations& expectations) {
    QWidget host;
    auto* layout = new QHBoxLayout(&host);
    auto& field = *new kit::KValueField(&host);
    layout->addWidget(&field);
    field.setRange(0.0, 1000.0);
    field.setValue(100.0);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();
    field.setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();

    QSignalSpy started(&field, &kit::KValueField::editStarted);
    QSignalSpy finished(&field, &kit::KValueField::editFinished);
    wheel(field, 0, Qt::AltModifier, 120);
    expectations.expect(field.value() == 100.0, "a wheel event with no vertical delta steps nothing");
    wheel(field, 40, Qt::AltModifier);
    wheel(field, 40, Qt::AltModifier);
    expectations.expect(field.value() == 100.0,
                        "fractions of a notch accumulate rather than each stepping");
    wheel(field, 40, Qt::AltModifier);
    expectations.expect(field.value() == 101.0, "three thirds of a notch are one step");
    wheel(field, 120, Qt::AltModifier);
    wheel(field, -240, Qt::AltModifier);
    expectations.expect(field.value() == 100.0, "and whole notches step both ways");
    expectations.expect(started.count() == 1 && finished.count() == 0,
                        "a burst of wheel steps is ONE edit while it lasts");
    QTest::qWait(kit::kWheelGestureIdleMs + 150);
    expectations.expect(finished.count() == 1 && !field.hasLiveEdit(),
                        "which finishes once the wheel has been still");
    wheel(field, 120);
    expectations.expect(field.value() == 100.0,
                        "with no modifier and no open editor the wheel belongs to the panel");
}

void testTabTraversalInAPlainTree(Expectations& expectations) {
    QWidget host;
    auto* layout = new QHBoxLayout(&host);
    auto* x = new kit::KValueField(&host);
    auto* y = new kit::KValueField(&host);
    auto* z = new kit::KValueField(&host);
    for (auto* field : {x, y, z}) {
        field->setRange(-1000.0, 1000.0);
        layout->addWidget(field);
    }
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();

    const QPointF centre = x->cellRect().center();
    mouse(*x, QEvent::MouseButtonPress, centre);
    mouse(*x, QEvent::MouseButtonRelease, centre);
    expectations.expect(x->isEditing(), "X opens on a click");
    x->lineEdit()->setText(QStringLiteral("5"));
    key(*QApplication::focusWidget(), Qt::Key_Tab);
    expectations.expect(x->value() == 5.0 && !x->isEditing(), "Tab commits X");
    expectations.expect(y->isEditing() && QApplication::focusWidget() == y->lineEdit(),
                        "and lands in Y, ready to type -- not skipping it");
    expectations.expect(y->lineEdit()->selectedText() == y->lineEdit()->text(),
                        "with Y's value selected");
    y->lineEdit()->setText(QStringLiteral("6"));
    key(*QApplication::focusWidget(), Qt::Key_Tab);
    expectations.expect(y->value() == 6.0 && z->isEditing(), "Tab again commits Y and opens Z");
    key(*QApplication::focusWidget(), Qt::Key_Backtab, Qt::ShiftModifier);
    expectations.expect(!z->isEditing() && y->isEditing(),
                        "Shift+Tab goes back to Y rather than returning to the same field");
    key(*QApplication::focusWidget(), Qt::Key_Backtab, Qt::ShiftModifier);
    expectations.expect(x->isEditing(), "and back to X");
    key(*QApplication::focusWidget(), Qt::Key_Escape);
    expectations.expect(!x->isEditing() && QApplication::focusWidget() == x,
                        "Escape commits and leaves focus on the field");
    key(*x, Qt::Key_7, Qt::NoModifier, QStringLiteral("7"));
    expectations.expect(x->isEditing() && x->lineEdit()->text() == QStringLiteral("7"),
                        "a digit typed into a focused field starts an edit with it");
    key(*QApplication::focusWidget(), Qt::Key_Return);
    expectations.expect(x->value() == 7.0, "and Return commits it");
}

void testIntegerMode(Expectations& expectations) {
    QWidget host;
    auto* layout = new QHBoxLayout(&host);
    auto& field = *new kit::KValueField(&host);
    layout->addWidget(&field);
    field.setIntegerRange(std::numeric_limits<std::int64_t>::min(),
                          std::numeric_limits<std::int64_t>::max());
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();
    expectations.expect(field.isIntegerMode() && field.decimals() == 0, "integer mode is on");

    QSignalSpy integerChanged(&field, &kit::KValueField::integerValueChanged);
    const auto type = [&field](const QString& text) {
        const QPointF centre = field.cellRect().center();
        mouse(field, QEvent::MouseButtonPress, centre);
        mouse(field, QEvent::MouseButtonRelease, centre);
        field.lineEdit()->setText(text);
        key(*field.lineEdit(), Qt::Key_Return);
    };
    type(QStringLiteral("9007199254740993"));
    expectations.expect(field.integerValue() == 9007199254740993LL,
                        "a value above 2^53 is stored exactly");
    expectations.expect(field.editText() == QStringLiteral("9007199254740993"),
                        "and edits back exactly");
    field.stepBy(1);
    expectations.expect(field.integerValue() == 9007199254740994LL, "a step is exact");
    type(QStringLiteral("9223372036854775807"));
    field.stepBy(1);
    expectations.expect(field.integerValue() == std::numeric_limits<std::int64_t>::max(),
                        "stepping past the maximum saturates");
    type(QStringLiteral("2.5"));
    expectations.expect(field.isEditing() && !field.refusal().isEmpty(),
                        "a fraction is refused in an integer field");
    key(*field.lineEdit(), Qt::Key_Escape);
    expectations.expect(field.integerValue() == std::numeric_limits<std::int64_t>::max(),
                        "and the last valid integer stays");
    expectations.expect(!integerChanged.isEmpty(), "integer changes are reported exactly");

    field.setIntegerRange(0, 10);
    field.setIntegerValue(5);
    const QPointF start = field.cellRect().center();
    mouse(field, QEvent::MouseButtonPress, start);
    mouse(field, QEvent::MouseMove, start + QPointF(QApplication::startDragDistance() + 3, 0));
    mouse(field, QEvent::MouseButtonRelease,
          start + QPointF(QApplication::startDragDistance() + 3, 0));
    expectations.expect(field.integerValue() == 8, "an integer field scrubs in whole steps");
}

void testNarrowWidthsNeverClipDigits(Expectations& expectations) {
    kit::KValueField field;
    field.setRange(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::max());
    field.setDecimals(2);
    field.setUnit(QStringLiteral("px"));
    const QFontMetrics metrics(kit::font(kit::TypeRole::Value));
    const auto fits = [&] {
        return metrics.horizontalAdvance(field.paintedText()) <= field.cellTextRect().width();
    };
    field.resize(kit::px(kit::Size::ValueCellMin), kit::px(kit::Size::Control));
    field.setValue(-123456789012.5);
    expectations.expect(fits() && field.isDisplayRounded() &&
                            field.paintedText().contains(QLatin1Char('e')),
                        "a long value falls back to scientific notation that fits, got " +
                            field.paintedText().toStdString());
    expectations.expect(field.paintedText().startsWith(QLatin1Char('-')),
                        "and its sign is never the part that is clipped");
    field.resize(kit::px(kit::Spacing::XL), kit::px(kit::Size::Control));
    expectations.expect(field.paintedText() == QString(QChar(0x2026)),
                        "a cell too narrow for even one digit shows an ellipsis, got " +
                            field.paintedText().toStdString());
    field.resize(400, kit::px(kit::Size::Control));
    field.setValue(12.25);
    expectations.expect(field.paintedText() == QStringLiteral("12.25 px") &&
                            !field.isDisplayRounded(),
                        "a value that fits is not marked as rounded");
}

void testMixedDefaultAndAccessibility(Expectations& expectations) {
    QWidget host;
    auto* layout = new QHBoxLayout(&host);
    auto& field = *new kit::KValueField(&host);
    layout->addWidget(&field);
    field.setRange(0.0, 100.0);
    field.setValue(20.0);
    field.setLabel(QStringLiteral("Gain"));
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();

    field.setMixed(true);
    expectations.expect(field.displayedValue() == QString(QChar(0x2014)),
                        "a mixed field shows a dash");
    const QPointF centre = field.cellRect().center();
    mouse(field, QEvent::MouseButtonPress, centre);
    mouse(field, QEvent::MouseButtonRelease, centre);
    expectations.expect(field.lineEdit()->text().isEmpty(), "and opens with an empty editor");
    key(*field.lineEdit(), Qt::Key_Return);
    expectations.expect(!field.isEditing() && field.isMixed() && field.value() == 20.0,
                        "an empty entry changes nothing and stays mixed");
    mouse(field, QEvent::MouseButtonPress, centre);
    mouse(field, QEvent::MouseButtonRelease, centre);
    field.lineEdit()->setText(QStringLiteral("33"));
    key(*field.lineEdit(), Qt::Key_Return);
    expectations.expect(!field.isMixed() && field.value() == 33.0,
                        "a typed value resolves the mixed state");

    field.setDefaultValue(50.0);
    expectations.expect(field.isNonDefault(), "a value away from its default is marked");
    QSignalSpy started(&field, &kit::KValueField::editStarted);
    QSignalSpy finished(&field, &kit::KValueField::editFinished);
    field.resetToDefault();
    expectations.expect(field.value() == 50.0 && !field.isNonDefault() && started.count() == 1 &&
                            finished.count() == 1,
                        "reset is one complete edit back to the default");

    auto* accessible = QAccessible::queryAccessibleInterface(&field);
    expectations.expect(accessible != nullptr && accessible->role() == QAccessible::SpinBox,
                        "the field reads as a spin box");
    auto* value = accessible != nullptr ? accessible->valueInterface() : nullptr;
    expectations.expect(value != nullptr && value->currentValue().toDouble() == 50.0 &&
                            value->maximumValue().toDouble() == 100.0 &&
                            value->minimumValue().toDouble() == 0.0,
                        "with its value, minimum and maximum");
    expectations.expect(accessible != nullptr &&
                            accessible->text(QAccessible::Name) == QStringLiteral("Gain"),
                        "and its label as the name");

    field.setEnabled(false);
    expectations.expect(field.cursor().shape() == Qt::ArrowCursor,
                        "a disabled field does not advertise a scrub");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    Expectations expectations;
    testTheExpressionParser(expectations);
    testScrubRebaseAndThreshold(expectations);
    testGrabLossFinishesTheScrub(expectations);
    testShortestFormatting(expectations);
    testTransportKeysPassThrough(expectations);
    testTheWheelGroupsAndAccumulates(expectations);
    testTabTraversalInAPlainTree(expectations);
    testIntegerMode(expectations);
    testNarrowWidthsNeverClipDigits(expectations);
    testMixedDefaultAndAccessibility(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
