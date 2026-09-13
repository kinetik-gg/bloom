// Task F1, item F2: the kit-wide focus/hover border rule. One 1px border and nothing else --
// Border at rest, BorderHover under the pointer, Accent while active (focused, editing, or holding
// an open popup) -- and FOCUS WINS, so a control that is both focused and hovered stays Accent.
//
// Every widget the rule applies to is pinned here at all four points (rest, hover, focus,
// focus+hover) through its own borderToken() seam, the same kind of public state seam
// visualState()/fillForState() already are. One file rather than six edits, because the point of
// the rule is that the six answers are identical.

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/panel_switcher.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QEnterEvent>
#include <QEvent>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPointF>
#include <QRect>
#include <QRgb>
#include <QStyle>
#include <QStyleOption>
#include <QVBoxLayout>
#include <QWidget>

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

void sendEnter(QWidget& widget) {
    QEnterEvent enter(QPointF(1.0, 1.0), QPointF(1.0, 1.0), QPointF(1.0, 1.0));
    QCoreApplication::sendEvent(&widget, &enter);
}

void sendLeave(QWidget& widget) {
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(&widget, &leave);
}

// Drives one widget through rest / hover / focus / focus+hover and asserts the one border rule at
// each point. `widget` must already be inside a shown window so focus is real.
template <typename Widget>
void pinTheFourPoints(Widget& widget, const std::string& what, Expectations& expectations) {
    widget.clearFocus();
    sendLeave(widget);
    expectations.expect(widget.borderToken() == kit::Color::Border, what + " rests on Border");

    sendEnter(widget);
    expectations.expect(widget.borderToken() == kit::Color::BorderHover,
                        what + " takes BorderHover under the pointer");

    sendLeave(widget);
    widget.setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(widget.hasFocus(), what + " can take keyboard focus at all");
    expectations.expect(widget.borderToken() == kit::Color::Accent,
                        what + " takes Accent when focused");

    // The point of the rule: hovering something you are already editing must not demote its
    // border back to the weaker hover ink.
    sendEnter(widget);
    expectations.expect(widget.borderToken() == kit::Color::Accent,
                        what + " keeps Accent when focus and hover are both on -- focus wins");

    sendLeave(widget);
    widget.clearFocus();
    widget.setEnabled(false);
    sendEnter(widget);
    expectations.expect(widget.borderToken() == kit::Color::Border,
                        what + " stays at Border when disabled, with no hover response");
    widget.setEnabled(true);
    sendLeave(widget);
}

void testTheRuleItself(Expectations& expectations) {
    expectations.expect(kit::borderForInteraction(true, false, false) == kit::Color::Border,
                        "rest is Border");
    expectations.expect(kit::borderForInteraction(true, false, true) == kit::Color::BorderHover,
                        "hover is BorderHover");
    expectations.expect(kit::borderForInteraction(true, true, false) == kit::Color::Accent,
                        "active is Accent");
    expectations.expect(kit::borderForInteraction(true, true, true) == kit::Color::Accent,
                        "active plus hover is still Accent: focus wins over hover");
    expectations.expect(kit::borderForInteraction(false, true, true) == kit::Color::Border,
                        "a disabled control has no hover and no focus response at all");
}

void testEveryKitControlFollowsIt(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);

    auto& button = *new kit::KButton(QStringLiteral("Render"), &host);
    auto& dropdown = *new kit::KDropdown(&host);
    dropdown.addItem(QStringLiteral("Linear"));
    dropdown.addItem(QStringLiteral("Filmic"));
    auto& switcher = *new kit::KPanelSwitcher(&host);
    switcher.addItem(QIcon(), QStringLiteral("Timeline"));
    switcher.addItem(QIcon(), QStringLiteral("Nodes"));
    auto& field = *new kit::KValueField(&host);
    auto& toggle = *new kit::KSwitch(&host);
    auto& slider = *new kit::KSlider(&host);
    layout->addWidget(&button);
    layout->addWidget(&dropdown);
    layout->addWidget(&switcher);
    layout->addWidget(&field);
    layout->addWidget(&toggle);
    layout->addWidget(&slider);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    pinTheFourPoints(button, "KButton", expectations);
    pinTheFourPoints(dropdown, "KDropdown", expectations);
    pinTheFourPoints(switcher, "KPanelSwitcher", expectations);
    pinTheFourPoints(field, "KValueField", expectations);
    pinTheFourPoints(toggle, "KSwitch", expectations);
    pinTheFourPoints(slider, "KSlider", expectations);
}

void testAnOpenPopupCountsAsActive(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& dropdown = *new kit::KDropdown(&host);
    dropdown.addItem(QStringLiteral("Linear"));
    dropdown.addItem(QStringLiteral("Filmic"));
    auto& switcher = *new kit::KPanelSwitcher(&host);
    switcher.addItem(QIcon(), QStringLiteral("Timeline"));
    switcher.addItem(QIcon(), QStringLiteral("Nodes"));
    layout->addWidget(&dropdown);
    layout->addWidget(&switcher);
    host.show();
    QCoreApplication::processEvents();

    dropdown.clearFocus();
    sendLeave(dropdown);
    dropdown.showPopup();
    QCoreApplication::processEvents();
    expectations.expect(dropdown.isPopupVisible(), "the dropdown popup actually opened");
    expectations.expect(dropdown.borderToken() == kit::Color::Accent,
                        "a KDropdown with an open popup is active: Accent, not the Pressed "
                        "BorderHover its surface recipe still uses");
    sendEnter(dropdown);
    expectations.expect(dropdown.borderToken() == kit::Color::Accent,
                        "hovering a KDropdown with an open popup does not demote it");
    dropdown.hidePopup();
    QCoreApplication::processEvents();

    switcher.clearFocus();
    sendLeave(switcher);
    switcher.showPopup();
    QCoreApplication::processEvents();
    expectations.expect(switcher.isPopupVisible(), "the switcher popup actually opened");
    expectations.expect(switcher.borderToken() == kit::Color::Accent,
                        "a KPanelSwitcher with an open popup is active: Accent");
    switcher.hidePopup();
    QCoreApplication::processEvents();
}

void testTheValueFieldCellIsBorderlessAtRest(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& field = *new kit::KValueField(&host);
    layout->addWidget(&field);
    host.show();
    QCoreApplication::processEvents();

    field.clearFocus();
    sendLeave(field);
    expectations.expect(field.cellBorderColor().alpha() == 0,
                        "a resting KValueField cell strokes nothing at all");

    sendEnter(field);
    expectations.expect(field.cellBorderColor() == kit::color(kit::Color::BorderHover),
                        "hovering gives the cell its one border");

    sendLeave(field);
    field.setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(field.cellBorderColor() == kit::color(kit::Color::Accent),
                        "focusing turns that one border Accent");
    sendEnter(field);
    expectations.expect(field.cellBorderColor() == kit::color(kit::Color::Accent),
                        "and hovering the focused cell keeps it Accent");
}

void testQtsOwnFocusRectangleIsNeverDrawn(Expectations& expectations) {
    // The rule forbids a second outline, and Qt's dotted focus rectangle would be exactly that on
    // top of a control that already paints its own border. The proxy style installed
    // application-wide drops the primitive outright.
    kit::AltUnderlineProxyStyle style;
    QImage canvas(24, 24, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    QStyleOption option;
    option.rect = QRect(2, 2, 20, 20);
    option.state =
        QStyle::State_Enabled | QStyle::State_HasFocus | QStyle::State_KeyboardFocusChange;
    style.drawPrimitive(QStyle::PE_FrameFocusRect, &option, &painter, nullptr);
    painter.end();

    bool anythingPainted = false;
    for (int y = 0; y < canvas.height() && !anythingPainted; ++y) {
        for (int x = 0; x < canvas.width(); ++x) {
            if (canvas.pixel(x, y) != qRgb(0, 0, 0)) {
                anythingPainted = true;
                break;
            }
        }
    }
    expectations.expect(!anythingPainted,
                        "PE_FrameFocusRect paints nothing through the kit's proxy style");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testTheRuleItself(expectations);
    testEveryKitControlFollowsIt(expectations);
    testAnOpenPopupCountsAsActive(expectations);
    testTheValueFieldCellIsBorderlessAtRest(expectations);
    testQtsOwnFocusRectangleIsNeverDrawn(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
