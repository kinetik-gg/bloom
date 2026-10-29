// Task F1, item F2: the kit-wide focus/hover border rule. One 1px border and nothing else --
// Border at rest, BorderHover under the pointer, Accent while active (focused, editing, or holding
// an open popup) -- and FOCUS WINS, so a control that is both focused and hovered stays Accent.
//
// Every widget the rule applies to is pinned here at all four points (rest, hover, focus,
// focus+hover) through its own borderToken() seam, the same kind of public state seam
// visualState()/fillForState() already are. One file rather than six edits, because the point of
// the rule is that the six answers are identical.

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_picker.hpp>
#include <bloom/ui/kit/color_swatches.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/panel_switcher.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QEnterEvent>
#include <QEvent>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QPainter>
#include <QPointF>
#include <QRect>
#include <QRgb>
#include <QStandardItemModel>
#include <QStyle>
#include <QStyleOption>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>

#include <iostream>
#include <optional>
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
    // A KValueField that Tab reaches opens its editor, a child that then holds the focus.
    expectations.expect(widget.hasFocus() || widget.isAncestorOf(QApplication::focusWidget()),
                        what + " can take keyboard focus at all");
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

// The rest of the focusable kit: icon buttons, radio groups (both presentations), the section
// header, the swatch row and the picker canvases all show keyboard focus -- and only keyboard
// focus: a click that lands focus on them leaves them looking exactly as at rest.
void testTheRemainingFocusablePrimitives(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& iconButton = *new kit::KIconButton(&host);
    iconButton.setIcon(kit::icon(kit::IconId::Settings, kit::IconRole::Chrome));
    auto& radio = *new kit::KRadioGroup(&host);
    radio.addOption(QStringLiteral("Corner"));
    radio.addOption(QStringLiteral("Smooth"));
    auto& section = *new kit::KSection(QStringLiteral("Advanced"), &host);
    auto& swatches = *new kit::KColorSwatchRow(&host);
    layout->addWidget(&iconButton);
    layout->addWidget(&radio);
    layout->addWidget(&section);
    layout->addWidget(&swatches);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();

    pinTheFourPoints(iconButton, "KIconButton", expectations);
    iconButton.setFocus(Qt::MouseFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(iconButton.hasFocus() && iconButton.borderToken() == kit::Color::Border,
                        "a click-focused KIconButton does not light its focus border");
    iconButton.clearFocus();

    pinTheFourPoints(radio, "KRadioGroup (segmented)", expectations);
    radio.setPresentation(kit::KRadioGroup::Presentation::Discrete);
    pinTheFourPoints(radio, "KRadioGroup (discrete)", expectations);

    section.header()->setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(section.headerHasKeyboardFocus(),
                        "the KSection header takes keyboard focus");
    QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
    QCoreApplication::sendEvent(section.header(), &space);
    expectations.expect(section.isCollapsed(), "Space on the focused header collapses it");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(section.header(), &enter);
    expectations.expect(!section.isCollapsed(), "Enter expands it again");
    section.header()->clearFocus();
    section.header()->setFocus(Qt::MouseFocusReason);
    expectations.expect(!section.headerHasKeyboardFocus(),
                        "a click on the header is not keyboard focus");

    expectations.expect(swatches.focusPolicy() == Qt::TabFocus,
                        "the swatch row is reached by Tab, not by a click");
    swatches.setSlotColor(1, kit::KColor::fromRgba(1.0F, 0.0F, 0.0F));
    swatches.setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QCoreApplication::sendEvent(&swatches, &right);
    expectations.expect(swatches.focusedSlot() == 1, "the arrow keys walk the swatch slots");
    std::optional<kit::KColor> activated;
    QObject::connect(&swatches, &kit::KColorSwatchRow::colorActivated,
                     [&activated](const kit::KColor& value) { activated = value; });
    QKeyEvent apply(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(&swatches, &apply);
    expectations.expect(activated.has_value() && activated->red > 0.99F,
                        "Enter applies the focused swatch");
}

void testThePickerCanvasesTakeKeyboardFocusAndStep(Expectations& expectations) {
    kit::KColorPicker picker;
    picker.setColor(kit::KColor::fromHsva(120.0F, 0.5F, 0.5F));
    picker.show();
    picker.activateWindow();
    QCoreApplication::processEvents();
    for (int index = 0; index < 3; ++index) {
        auto* region = picker.canvasRegion(index);
        expectations.expect(region != nullptr && region->focusPolicy() == Qt::TabFocus,
                            "every picker canvas region is reachable by Tab");
    }
    const float before = picker.color().toHsva()[1];
    auto* square = picker.canvasRegion(0);
    square->setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(kit::hasKeyboardFocus(*square), "the SV square holds keyboard focus");
    QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QCoreApplication::sendEvent(square, &right);
    expectations.expect(picker.color().toHsva()[1] > before,
                        "Right on the SV square raises saturation");
    const float hueBefore = picker.color().toHsva()[0];
    auto* hue = picker.canvasRegion(1);
    hue->setFocus(Qt::TabFocusReason);
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::ShiftModifier);
    QCoreApplication::sendEvent(hue, &down);
    expectations.expect(picker.color().toHsva()[0] > hueBefore + 10.0F,
                        "Shift+Down on the hue bar takes a coarse step");
}

// An item view's current row shows an Accent inset under keyboard focus and nothing under a click:
// the list form of the one focus rule, painted by the kit proxy style.
void testItemViewsShowKeyboardFocusOnTheCurrentRow(Expectations& expectations) {
    QStandardItemModel model;
    for (const char* name : {"Alpha", "Beta", "Gamma"})
        model.appendRow(new QStandardItem(QString::fromLatin1(name)));
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& other = *new kit::KButton(QStringLiteral("Other"), &host);
    auto& tree = *new QTreeView(&host);
    tree.setModel(&model);
    tree.setRootIsDecorated(false);
    tree.setHeaderHidden(true);
    tree.setAllColumnsShowFocus(true);
    layout->addWidget(&other);
    layout->addWidget(&tree);
    host.resize(240, 200);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();
    tree.setCurrentIndex(model.index(1, 0));
    tree.clearSelection();
    const auto accentPixels = [&tree] {
        const QImage image = tree.viewport()->grab().toImage();
        const QColor accent = kit::color(kit::Color::Accent);
        int count = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                count += image.pixelColor(x, y) == accent;
        return count;
    };
    tree.setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(accentPixels() > 0,
                        "a keyboard-focused item view insets its current row in Accent");
    other.setFocus(Qt::TabFocusReason);
    tree.setFocus(Qt::MouseFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(accentPixels() == 0, "a click-focused item view shows no focus inset");
}

// The focus tracker repaints on every keyboard-focus change but re-resolves stylesheet rules only
// for widgets that opted in -- a custom-painted kit control is never unpolished.
void testTheFocusTrackerRestylesOnlyOptedInWidgets(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto& menuButton = *new kit::KMenuButton(&host);
    menuButton.setText(QStringLiteral("View"));
    menuButton.setFocusPolicy(Qt::TabFocus);
    auto& button = *new kit::KButton(QStringLiteral("Render"), &host);
    layout->addWidget(&menuButton);
    layout->addWidget(&button);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();

    int polishes = 0;
    struct PolishCounter final : QObject {
        int* count;
        explicit PolishCounter(int* target) : count(target) {}
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::Polish || event->type() == QEvent::StyleChange ||
                event->type() == QEvent::PaletteChange)
                ++*count;
            return false;
        }
    } counter(&polishes);
    button.installEventFilter(&counter);
    button.setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    button.clearFocus();
    QCoreApplication::processEvents();
    expectations.expect(polishes == 0, "a kit-painted button is repainted, never re-polished");
    expectations.expect(menuButton.property("bloomFocusRestyle").toBool(),
                        "a KMenuButton opts in to restyling: its focus rule is a sheet rule");
    menuButton.setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(kit::hasKeyboardFocus(menuButton),
                        "a KMenuButton reports keyboard focus for its sheet rule");
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
    testTheRemainingFocusablePrimitives(expectations);
    testThePickerCanvasesTakeKeyboardFocusAndStep(expectations);
    kit::installKinetikTheme(application);
    testItemViewsShowKeyboardFocusOnTheCurrentRow(expectations);
    testTheFocusTrackerRestylesOnlyOptedInWidgets(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
