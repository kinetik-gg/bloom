#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdlib>
#include <functional>
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

template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (std::invoke(predicate)) {
            return true;
        }
    }
    return std::invoke(predicate);
}

void clickAt(QWidget& widget, const QPoint& point) {
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), widget.mapToGlobal(QPointF(point)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point),
                        widget.mapToGlobal(QPointF(point)), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &release);
    QCoreApplication::processEvents();
}

void moveTo(QWidget& widget, const QPoint& point) {
    QMouseEvent move(QEvent::MouseMove, QPointF(point), widget.mapToGlobal(QPointF(point)),
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &move);
    QCoreApplication::processEvents();
}

void testTheRadioGroupModelIsOneOfMany(Expectations& expectations) {
    kit::KRadioGroup group;
    const int reference = group.addOption(QStringLiteral("Reference"));
    const int qualified = group.addOption(QStringLiteral("Qualified"));
    const int raw = group.addOption(QStringLiteral("Raw"));

    expectations.expect(group.count() == 3, "options are added in order");
    expectations.expect(group.currentIndex() == reference,
                        "the first option added becomes the choice");
    expectations.expect(group.optionText(qualified) == QStringLiteral("Qualified"),
                        "an option keeps its label");

    QSignalSpy changed(&group, &kit::KRadioGroup::currentIndexChanged);
    group.setCurrentIndex(raw);
    expectations.expect(group.currentIndex() == raw, "the choice moves");
    expectations.expect(changed.count() == 1, "moving the choice reports it once");
    group.setCurrentIndex(raw);
    expectations.expect(changed.count() == 1, "re-choosing the same option reports nothing");
}

void sendLeave(QWidget& widget) {
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(&widget, &leave);
    QCoreApplication::processEvents();
}

void testBothPresentationsShareOneStateMachine(Expectations& expectations) {
    kit::KRadioGroup group;
    (void)group.addOption(QStringLiteral("Reference"));
    const int qualified = group.addOption(QStringLiteral("Qualified"));
    group.resize(group.sizeHint());
    QCoreApplication::processEvents();

    for (const auto presentation :
         {kit::KRadioGroup::Presentation::Segmented, kit::KRadioGroup::Presentation::Discrete}) {
        group.setPresentation(presentation);
        group.resize(group.sizeHint());
        // The pointer never left the widget between presentations; clear the hover explicitly so
        // the resting assertion below is about resting rather than about leftover state.
        sendLeave(group);
        QCoreApplication::processEvents();

        expectations.expect(group.stateForOption(group.currentIndex()) == kit::State::Selected,
                            "the chosen option reads as selected in both presentations");
        expectations.expect(group.stateForOption(qualified) == kit::State::Normal,
                            "an unchosen, unhovered option rests in both presentations");

        const QRect target = group.optionRect(qualified);
        expectations.expect(target.isValid() && group.rect().contains(target.center()),
                            "every option occupies a real rectangle inside the control");

        moveTo(group, target.center());
        expectations.expect(group.stateForOption(qualified) == kit::State::Hover,
                            "hovering an option reads as hover in both presentations");

        clickAt(group, target.center());
        expectations.expect(group.currentIndex() == qualified,
                            "clicking an option chooses it in both presentations");
        group.setCurrentIndex(0);
    }
}

void testASegmentedBarEndsFlush(Expectations& expectations) {
    kit::KRadioGroup group;
    for (const char* label : {"A", "B", "C"}) {
        (void)group.addOption(QString::fromLatin1(label));
    }
    // A width that does not divide evenly by three: the remainder must not leave a gap at the end.
    group.resize(100, group.sizeHint().height());
    QCoreApplication::processEvents();
    expectations.expect(group.optionRect(0).left() == 0, "the bar starts at the control's edge");
    expectations.expect(group.optionRect(2).right() == group.rect().right(),
                        "the last segment absorbs the remainder and ends flush");
    expectations.expect(group.optionRect(0).right() + 1 == group.optionRect(1).left(),
                        "segments abut without a seam");
}

void testDisabledOptionsAreRefusedAndSkipped(Expectations& expectations) {
    kit::KRadioGroup group;
    const int first = group.addOption(QStringLiteral("Reference"));
    const int unavailable = group.addOption(QStringLiteral("Qualified"));
    const int third = group.addOption(QStringLiteral("Raw"));
    group.setOptionEnabled(unavailable, false);
    group.resize(group.sizeHint());
    QCoreApplication::processEvents();

    expectations.expect(!group.isOptionEnabled(unavailable), "the option reports as disabled");
    expectations.expect(group.stateForOption(unavailable) == kit::State::Disabled,
                        "a disabled option reads as disabled, not as resting");

    QSignalSpy changed(&group, &kit::KRadioGroup::currentIndexChanged);
    group.setCurrentIndex(unavailable);
    expectations.expect(group.currentIndex() == first, "setCurrentIndex refuses a disabled option");
    clickAt(group, group.optionRect(unavailable).center());
    expectations.expect(group.currentIndex() == first,
                        "clicking a disabled option chooses nothing");
    expectations.expect(changed.count() == 0, "a refused choice reports nothing");

    // Arrow keys must skip the disabled option rather than parking on something uncommittable.
    QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QCoreApplication::sendEvent(&group, &right);
    expectations.expect(group.currentIndex() == third,
                        "arrow navigation steps over the disabled option");
    QKeyEvent left(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
    QCoreApplication::sendEvent(&group, &left);
    expectations.expect(group.currentIndex() == first,
                        "arrow navigation steps back over it too, rather than stopping");
}

void testTheSwitchThumbTravelsWithTheFastMotion(Expectations& expectations) {
    kit::KSwitch toggle;
    toggle.resize(toggle.sizeHint());
    QCoreApplication::processEvents();

    expectations.expect(!toggle.isChecked(), "a fresh switch is off");
    expectations.expect(toggle.thumbPosition() == 0.0, "its thumb rests at the off end");
    expectations.expect(toggle.visualState() == kit::State::Normal, "and it reads as resting");

    toggle.setChecked(true);
    expectations.expect(toggle.visualState() == kit::State::Selected,
                        "a checked switch reads as selected immediately, before the thumb arrives");
    expectations.expect(waitUntil([&toggle] { return toggle.thumbPosition() >= 1.0; }),
                        "the thumb travels all the way to the on end");

    toggle.setChecked(false);
    expectations.expect(waitUntil([&toggle] { return toggle.thumbPosition() <= 0.0; }),
                        "and all the way back");
}

void testTheSwitchHonoursReducedMotion(Expectations& expectations) {
    qputenv("BLOOM_REDUCED_MOTION", "1");
    kit::KSwitch toggle;
    toggle.resize(toggle.sizeHint());
    toggle.setChecked(true);
    // No event loop turn, no animation: under reduced motion the thumb is already there. A toggle
    // that cannot animate must still be a toggle.
    expectations.expect(toggle.thumbPosition() == 1.0,
                        "reduced motion puts the thumb at its end state with no travel");
    toggle.setChecked(false);
    expectations.expect(toggle.thumbPosition() == 0.0, "and back with no travel");
    qunsetenv("BLOOM_REDUCED_MOTION");
}

void testTheSwitchStateMachineAndDisabled(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& toggle = *new kit::KSwitch(&host);
    layout->addWidget(&toggle);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();

    toggle.setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(toggle.visualState() == kit::State::Focused, "focus is its own state");
    toggle.clearFocus();

    toggle.setDown(true);
    expectations.expect(toggle.visualState() == kit::State::Pressed, "pressing outranks resting");
    toggle.setDown(false);

    toggle.setEnabled(false);
    toggle.setDown(true);
    expectations.expect(toggle.visualState() == kit::State::Disabled,
                        "disabled outranks everything: a disabled switch does not respond");
    toggle.setDown(false);
    toggle.setEnabled(true);
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    Expectations expectations;
    testTheRadioGroupModelIsOneOfMany(expectations);
    testBothPresentationsShareOneStateMachine(expectations);
    testASegmentedBarEndsFlush(expectations);
    testDisabledOptionsAreRefusedAndSkipped(expectations);
    testTheSwitchThumbTravelsWithTheFastMotion(expectations);
    testTheSwitchHonoursReducedMotion(expectations);
    testTheSwitchStateMachineAndDisabled(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
