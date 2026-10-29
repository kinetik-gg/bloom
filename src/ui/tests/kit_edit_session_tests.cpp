// Lane B: the edit-session contract beyond KValueField -- KSlider and KRangeSelector (one drag or
// one run of keys is one edit, grab loss commits, keyboard handles), KSwitch hover and projection,
// KCheckBox focus/label/mixed, KSearchField's two Escapes, and the KInlineEdit policy itself
// (Escape confirms, refused text keeps the editor open, a popup focus change is not an ending).

#include <bloom/ui/kit/inline_edit.hpp>
#include <bloom/ui/kit/range_selector.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/text_field.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QAccessible>
#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <iostream>
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

void mouse(QWidget& widget, const QEvent::Type type, const QPointF& point) {
    const auto buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    const auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, point, widget.mapToGlobal(point), button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &event);
    QCoreApplication::processEvents();
}

void key(QWidget& widget, const QEvent::Type type, const Qt::Key code,
         const Qt::KeyboardModifiers modifiers = {}, const bool autoRepeat = false) {
    QKeyEvent event(type, code, modifiers, QString{}, autoRepeat);
    QCoreApplication::sendEvent(&widget, &event);
    QCoreApplication::processEvents();
}

void testTheSliderIsOneEditPerGesture(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& slider = *new kit::KSlider(&host);
    layout->addWidget(&slider);
    slider.setRange(0.0, 100.0);
    slider.setValue(50.0);
    slider.setFixedWidth(220);
    host.show();
    QCoreApplication::processEvents();
    QSignalSpy started(&slider, &kit::KSlider::editStarted);
    QSignalSpy finished(&slider, &kit::KSlider::editFinished);

    // Grab the handle off-centre: nothing moves until the pointer does.
    const QPointF grab = slider.handleRect().center() + QPointF(3.0, 0.0);
    mouse(slider, QEvent::MouseButtonPress, grab);
    expectations.expect(slider.value() == 50.0,
                        "pressing the handle keeps the grab offset rather than jumping");
    mouse(slider, QEvent::MouseMove, grab + QPointF(20.0, 0.0));
    mouse(slider, QEvent::MouseMove, grab + QPointF(40.0, 0.0));
    expectations.expect(slider.value() > 50.0 && slider.isDragging(), "the drag moves it");
    slider.setValue(10.0);
    expectations.expect(slider.value() > 50.0, "a projection mid-drag is refused");
    mouse(slider, QEvent::MouseButtonRelease, grab + QPointF(40.0, 0.0));
    expectations.expect(started.count() == 1 && finished.count() == 1,
                        "one drag is one edit: one start, one finish");

    slider.setFocus(Qt::TabFocusReason);
    for (int i = 0; i < 3; ++i)
        key(slider, QEvent::KeyPress, Qt::Key_Right, {}, i > 0);
    expectations.expect(started.count() == 2 && finished.count() == 1,
                        "held arrow presses are one open edit");
    key(slider, QEvent::KeyRelease, Qt::Key_Right);
    expectations.expect(finished.count() == 2, "which finishes on the release");
    key(slider, QEvent::KeyPress, Qt::Key_Home);
    key(slider, QEvent::KeyPress, Qt::Key_Escape);
    expectations.expect(slider.value() == 0.0 && finished.count() == 3,
                        "Escape confirms a key edit rather than undoing it");

    mouse(slider, QEvent::MouseButtonPress, slider.trackRect().center());
    QEvent ungrab(QEvent::UngrabMouse);
    QCoreApplication::sendEvent(&slider, &ungrab);
    expectations.expect(!slider.isDragging() && finished.count() == 4,
                        "losing the pointer grab commits the drag");
    QKeyEvent override(QEvent::ShortcutOverride, Qt::Key_Left, Qt::NoModifier);
    override.ignore();
    QCoreApplication::sendEvent(&slider, &override);
    expectations.expect(override.isAccepted(), "a focused slider claims the arrows it steps with");

    auto* accessible = QAccessible::queryAccessibleInterface(&slider);
    expectations.expect(accessible != nullptr && accessible->role() == QAccessible::Slider &&
                            accessible->valueInterface() != nullptr &&
                            accessible->valueInterface()->maximumValue().toDouble() == 100.0,
                        "the slider reads as a slider with its value");
}

void testTheRangeSelectorKeysAndCoincidentHandles(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& range = *new kit::KRangeSelector(&host);
    layout->addWidget(&range);
    range.setRange(0.0, 100.0);
    range.setValues(20.0, 80.0);
    range.setFixedWidth(300);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();
    range.setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    QSignalSpy started(&range, &kit::KRangeSelector::editStarted);
    QSignalSpy finished(&range, &kit::KRangeSelector::editFinished);

    expectations.expect(range.activeHandle() == kit::KRangeSelector::Handle::Lower,
                        "Tab arrives on the lower handle");
    key(range, QEvent::KeyPress, Qt::Key_Right);
    key(range, QEvent::KeyRelease, Qt::Key_Right);
    expectations.expect(range.lowerValue() == 21.0 && range.upperValue() == 80.0,
                        "an arrow steps the active handle");
    key(range, QEvent::KeyPress, Qt::Key_PageUp);
    key(range, QEvent::KeyRelease, Qt::Key_PageUp);
    expectations.expect(range.lowerValue() == 31.0, "Page steps ten times as far");
    key(range, QEvent::KeyPress, Qt::Key_Tab);
    expectations.expect(range.activeHandle() == kit::KRangeSelector::Handle::Upper &&
                            range.hasFocus(),
                        "Tab moves to the upper handle without leaving the control");
    key(range, QEvent::KeyPress, Qt::Key_End);
    key(range, QEvent::KeyRelease, Qt::Key_End);
    expectations.expect(range.upperValue() == 100.0, "End sends the active handle to its limit");
    key(range, QEvent::KeyPress, Qt::Key_Home);
    key(range, QEvent::KeyRelease, Qt::Key_Home);
    expectations.expect(range.upperValue() == range.lowerValue(),
                        "Home stops the upper handle at the lower one, never crossing it");
    expectations.expect(started.count() == finished.count() && started.count() == 4,
                        "each key run is one edit");

    // Coincident handles: the first move's direction picks the handle.
    range.setValues(50.0, 50.0);
    const QPointF on = range.handleRect(kit::KRangeSelector::Handle::Lower).center();
    mouse(range, QEvent::MouseButtonPress, on);
    mouse(range, QEvent::MouseMove, on + QPointF(30.0, 0.0));
    mouse(range, QEvent::MouseButtonRelease, on + QPointF(30.0, 0.0));
    expectations.expect(range.lowerValue() == 50.0 && range.upperValue() > 50.0,
                        "dragging right off stacked handles takes the upper one");
    range.setValues(50.0, 50.0);
    mouse(range, QEvent::MouseButtonPress, on);
    mouse(range, QEvent::MouseMove, on - QPointF(30.0, 0.0));
    mouse(range, QEvent::MouseButtonRelease, on - QPointF(30.0, 0.0));
    expectations.expect(range.upperValue() == 50.0 && range.lowerValue() < 50.0,
                        "and dragging left takes the lower one -- they never stick");
}

void testSwitchAndCheckBox(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& toggle = *new kit::KSwitch(&host);
    auto& check = *new kit::KCheckBox(QStringLiteral("Include audio"), &host);
    layout->addWidget(&toggle);
    layout->addWidget(&check);
    host.show();
    QCoreApplication::processEvents();

    toggle.setChecked(true);
    expectations.expect(toggle.thumbPosition() == 1.0,
                        "a programmatic setChecked places the thumb without animating");
    QEnterEvent enter(QPointF(4, 4), QPointF(4, 4), toggle.mapToGlobal(QPointF(4, 4)));
    QCoreApplication::sendEvent(&toggle, &enter);
    expectations.expect(toggle.visualState() == kit::State::Hover,
                        "a checked switch still shows hover");

    expectations.expect(check.width() > kit::px(kit::Size::Control) &&
                            check.label() == QStringLiteral("Include audio"),
                        "a check box carries its own clickable label");
    QSignalSpy toggled(&check, &kit::KCheckBox::toggled);
    mouse(check, QEvent::MouseButtonPress, QPointF(check.width() - 4.0, check.height() / 2.0));
    mouse(check, QEvent::MouseButtonRelease, QPointF(check.width() - 4.0, check.height() / 2.0));
    expectations.expect(check.isChecked() && toggled.count() == 1,
                        "clicking the label toggles the box");
    check.setMixed(true);
    expectations.expect(check.isMixed() && !check.isChecked(), "a mixed box is indeterminate");
    check.click();
    expectations.expect(!check.isMixed() && check.isChecked() && toggled.count() == 2,
                        "a click resolves the mixed state to checked");
}

void testTheSearchFieldsTwoEscapes(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& search = *new kit::KSearchField(&host);
    layout->addWidget(&search);
    host.show();
    QCoreApplication::processEvents();
    search.setText(QStringLiteral("blur"));
    expectations.expect(search.clearAction()->isVisible(), "a clear button shows with text");
    QSignalSpy escaped(&search, &kit::KSearchField::escaped);
    QKeyEvent override(QEvent::ShortcutOverride, Qt::Key_Escape, Qt::NoModifier);
    override.ignore();
    QCoreApplication::sendEvent(&search, &override);
    expectations.expect(override.isAccepted(), "a search field with text claims Escape");
    key(search, QEvent::KeyPress, Qt::Key_Escape);
    expectations.expect(search.text().isEmpty() && escaped.isEmpty() &&
                            !search.clearAction()->isVisible(),
                        "the first Escape clears the text");
    key(search, QEvent::KeyPress, Qt::Key_Escape);
    expectations.expect(escaped.count() == 1, "the second is left for the surface around it");
}

void testTheInlineEditPolicy(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& field = *new kit::KLineEdit(&host);
    auto& other = *new kit::KLineEdit(&host);
    layout->addWidget(&field);
    layout->addWidget(&other);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();
    auto& policy = field.useInlineEdit([](const QString& text) {
        return text.trimmed().isEmpty() ? QStringLiteral("A name cannot be empty") : QString{};
    });
    QSignalSpy accepted(&policy, &kit::KInlineEdit::accepted);
    QSignalSpy refused(&policy, &kit::KInlineEdit::refused);
    field.setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();

    field.setText(QStringLiteral("Plate"));
    key(field, QEvent::KeyPress, Qt::Key_Escape);
    expectations.expect(accepted.count() == 1 &&
                            accepted.front().at(1).value<kit::KInlineEdit::Ending>() ==
                                kit::KInlineEdit::Ending::Escape,
                        "Escape confirms the typed text");
    field.setText(QString{});
    key(field, QEvent::KeyPress, Qt::Key_Return);
    expectations.expect(accepted.count() == 1 && policy.hasRefusal() &&
                            field.property("kitError").toBool() &&
                            field.toolTip() == QStringLiteral("A name cannot be empty"),
                        "Return with refused text stays open with an Error border and the reason");
    QFocusEvent popup(QEvent::FocusOut, Qt::PopupFocusReason);
    QCoreApplication::sendEvent(&field, &popup);
    expectations.expect(refused.isEmpty(), "the editor's own context menu is not an ending");
    key(field, QEvent::KeyPress, Qt::Key_Escape);
    expectations.expect(refused.count() == 1 && !policy.hasRefusal() &&
                            !field.property("kitError").toBool(),
                        "Escape with refused text ends the edit on the last valid value");
    field.setText(QStringLiteral("Plate 2"));
    field.setFocus(Qt::OtherFocusReason);
    other.setFocus(Qt::MouseFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(accepted.count() == 2, "focus loss commits");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    Expectations expectations;
    testTheSliderIsOneEditPerGesture(expectations);
    testTheRangeSelectorKeysAndCoincidentHandles(expectations);
    testSwitchAndCheckBox(expectations);
    testTheSearchFieldsTwoEscapes(expectations);
    testTheInlineEditPolicy(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
