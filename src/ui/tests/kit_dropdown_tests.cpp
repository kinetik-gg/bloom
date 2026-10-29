#include <bloom/ui/kit/accessibility.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/dropdown_popup.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/popup_list.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QAbstractItemModel>
#include <QAccessible>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QImage>
#include <QKeyEvent>
#include <QListView>
#include <QMouseEvent>
#include <QRect>
#include <QSignalSpy>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
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
    kit::KDropdown* dropdown = nullptr;

    Fixture() {
        auto* layout = new QVBoxLayout(&host);
        dropdown = new kit::KDropdown(&host);
        layout->addWidget(dropdown);
        host.resize(240, 120);
        host.show();
        host.activateWindow();
        QCoreApplication::processEvents();
    }
};

void clickRow(kit::KDropdown& dropdown, const int row,
              const Qt::MouseButton button = Qt::LeftButton) {
    auto* view = dropdown.popupView();
    const QRect rect = view->visualRect(view->model()->index(row, 0));
    const QPointF centre = rect.center();
    QMouseEvent release(QEvent::MouseButtonRelease, centre, view->viewport()->mapToGlobal(centre),
                        button, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(view->viewport(), &release);
    QCoreApplication::processEvents();
}

void key(QWidget& widget, const int code, const Qt::KeyboardModifiers modifiers = Qt::NoModifier,
         const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, code, modifiers, text);
    QCoreApplication::sendEvent(&widget, &press);
    QCoreApplication::processEvents();
}

void testBuildingTheListNeverReportsAChoice(Expectations& expectations) {
    Fixture fixture;
    auto& dropdown = *fixture.dropdown;
    QSignalSpy changed(&dropdown, &kit::KDropdown::currentIndexChanged);
    QSignalSpy activated(&dropdown, &kit::KDropdown::activated);
    QSignalSpy items(&dropdown, &kit::KDropdown::itemsChanged);
    expectations.expect(dropdown.currentIndex() == -1, "an empty dropdown has no current value");

    const int linear = dropdown.addItem(QStringLiteral("Linear"), QStringLiteral("linear"));
    const int ease = dropdown.addItem(QStringLiteral("Ease In Out"));
    expectations.expect(dropdown.currentIndex() == linear,
                        "the first item added becomes the current value");
    expectations.expect(changed.count() == 0 && activated.count() == 0,
                        "adding items emits neither currentIndexChanged nor activated: building a "
                        "list is never a commit");
    expectations.expect(items.count() == 2, "each added item reports itemsChanged");
    expectations.expect(dropdown.itemData(linear).toString() == QStringLiteral("linear"),
                        "an item's payload survives");

    dropdown.setCurrentIndex(ease);
    expectations.expect(changed.count() == 1 && activated.count() == 0,
                        "a programmatic change reports currentIndexChanged only");
    dropdown.chooseIndex(linear);
    expectations.expect(changed.count() == 2 && activated.count() == 1 &&
                            activated.front().front().toInt() == linear,
                        "the artist's choice reports both, activated once");
    dropdown.chooseIndex(linear);
    expectations.expect(activated.count() == 1,
                        "re-choosing the value it already holds is not an edit");

    dropdown.setItemText(ease, QStringLiteral("Ease"));
    dropdown.clearItems();
    expectations.expect(items.count() == 4, "renaming and clearing report itemsChanged");
    expectations.expect(changed.count() == 2 && activated.count() == 1,
                        "and neither reports a choice");
}

void testChoosingFromThePopupCommitsAndCloses(Expectations& expectations) {
    Fixture fixture;
    auto& dropdown = *fixture.dropdown;
    (void)dropdown.addItem(QStringLiteral("Linear"));
    const int ease = dropdown.addItem(QStringLiteral("Ease In Out"));

    QSignalSpy activated(&dropdown, &kit::KDropdown::activated);
    dropdown.showPopup();
    QCoreApplication::processEvents();
    expectations.expect(dropdown.isPopupVisible() && dropdown.visualState() == kit::State::Pressed,
                        "an open dropdown reads as pressed");
    expectations.expect(dropdown.popup()->objectName() == QStringLiteral("kDropdownPopup"),
                        "the popup is findable by name");
    clickRow(dropdown, ease, Qt::RightButton);
    clickRow(dropdown, ease, Qt::MiddleButton);
    expectations.expect(dropdown.currentIndex() == 0 && activated.count() == 0 &&
                            dropdown.isPopupVisible(),
                        "a right or middle release on a row commits nothing");
    clickRow(dropdown, ease);
    expectations.expect(dropdown.currentIndex() == ease && activated.count() == 1,
                        "a left release on a row commits it, as the artist's choice");
    expectations.expect(!dropdown.isPopupVisible(), "committing closes the popup");
}

void testADisabledItemIsVisibleButNotSelectable(Expectations& expectations) {
    Fixture fixture;
    auto& dropdown = *fixture.dropdown;
    const int linear = dropdown.addItem(QStringLiteral("Linear"));
    const int unavailable = dropdown.addItem(QStringLiteral("Bezier"));
    dropdown.setItemEnabled(unavailable, false);
    expectations.expect(dropdown.count() == 2 &&
                            dropdown.itemText(unavailable) == QStringLiteral("Bezier"),
                        "a disabled item stays in the list and readable");

    QSignalSpy changed(&dropdown, &kit::KDropdown::currentIndexChanged);
    dropdown.setCurrentIndex(unavailable);
    dropdown.chooseIndex(unavailable);
    expectations.expect(dropdown.currentIndex() == linear && changed.count() == 0,
                        "both paths refuse a disabled item and report nothing");

    dropdown.showPopup();
    QCoreApplication::processEvents();
    clickRow(dropdown, unavailable);
    expectations.expect(dropdown.currentIndex() == linear && dropdown.isPopupVisible(),
                        "a disabled row swallows the click and leaves the popup open");
    dropdown.hidePopup();

    // Disabling the CURRENT item keeps it the value (it is what the document holds), shown muted
    // with that item's own explanation rather than silently falling to no value.
    dropdown.setItemToolTip(linear, QStringLiteral("Unavailable in this composition"));
    dropdown.setItemEnabled(linear, false);
    expectations.expect(dropdown.currentIndex() == linear && changed.count() == 0,
                        "disabling the current item keeps it current and emits nothing");
    expectations.expect(dropdown.toolTip() == QStringLiteral("Unavailable in this composition"),
                        "the field carries the disabled item's own reason");
}

void testAnOversizedValueElidesAndTheFloorIsReal(Expectations& expectations) {
    Fixture fixture;
    auto& dropdown = *fixture.dropdown;
    const QString longValue =
        QStringLiteral("A qualified display transform with a very long descriptive name");
    (void)dropdown.addItem(longValue);
    QCoreApplication::processEvents();
    expectations.expect(dropdown.minimumSizeHint().width() < dropdown.sizeHint().width() / 2,
                        "the minimum is a few characters, not the widest item");

    dropdown.resize(120, dropdown.sizeHint().height());
    const QString displayed = dropdown.displayedText();
    expectations.expect(displayed != longValue && displayed.endsWith(QChar(0x2026)),
                        "an oversized value ends in an ellipsis");
    expectations.expect(dropdown.currentText() == longValue,
                        "eliding is presentation only: the value itself is untouched");
    dropdown.resize(dropdown.sizeHint());
    expectations.expect(dropdown.displayedText() == longValue,
                        "given its preferred width the value prints in full");

    dropdown.setWidthFloor(kit::px(kit::Size::PropertiesDropdownWidth));
    expectations.expect(dropdown.minimumWidth() == kit::px(kit::Size::PropertiesDropdownWidth) &&
                            dropdown.maximumWidth() == QWIDGETSIZE_MAX &&
                            dropdown.sizePolicy().horizontalPolicy() == QSizePolicy::Maximum,
                        "setWidthFloor is a floor: it neither fixes the width nor stretches it");
}

void testTheStateMachineAndDisabledDropdown(Expectations& expectations) {
    Fixture fixture;
    auto& dropdown = *fixture.dropdown;
    (void)dropdown.addItem(QStringLiteral("Linear"));
    dropdown.clearFocus();
    QCoreApplication::processEvents();
    expectations.expect(dropdown.visualState() == kit::State::Normal,
                        "an unfocused, unhovered dropdown rests");
    dropdown.setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(dropdown.visualState() == kit::State::Focused, "focus is its own state");
    dropdown.clearFocus();
    dropdown.setEnabled(false);
    expectations.expect(dropdown.visualState() == kit::State::Disabled,
                        "disabled outranks the rest");
    expectations.expect(dropdown.cursor().shape() == Qt::ArrowCursor,
                        "a disabled dropdown does not show the pointing hand");
    dropdown.setEnabled(true);
    expectations.expect(dropdown.cursor().shape() == Qt::PointingHandCursor,
                        "an enabled one does");
}

// task U8, issue #131, fix 3: the design sheet's dropdown chrome -- bordered closed field with the
// vendored double up/down chevron.
void testTheClosedFieldIsBorderedWithTheVendoredDoubleChevron(Expectations& expectations) {
    Fixture fixture;
    auto& dropdown = *fixture.dropdown;
    (void)dropdown.addItem(QStringLiteral("Linear"));
    expectations.expect(
        QFile::exists(kit::iconResourcePath(kit::IconId::CaretUpDown, kit::IconWeight::Regular)) &&
            QFile::exists(kit::iconResourcePath(kit::IconId::CaretUpDown, kit::IconWeight::Fill)),
        "the vendored double-chevron asset backs IconId::CaretUpDown in both weights");
    QCoreApplication::processEvents();
    dropdown.clearFocus();
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(&dropdown, &leave);
    QCoreApplication::processEvents();
    const QImage image = dropdown.grab().toImage();
    const auto near = [](const QColor& left, const QColor& right) {
        return std::abs(left.red() - right.red()) <= 2 &&
               std::abs(left.green() - right.green()) <= 2 &&
               std::abs(left.blue() - right.blue()) <= 2;
    };
    bool sawFill = false;
    bool sawBorder = false;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            sawFill = sawFill || near(image.pixelColor(x, y), kit::color(kit::Color::SurfaceRaised));
            sawBorder = sawBorder || near(image.pixelColor(x, y), kit::color(kit::Color::Border));
        }
    expectations.expect(sawFill && sawBorder,
                        "the closed field wears the one control look: the SurfaceRaised plate "
                        "inside a Border hairline");
}

// Windows and Linux: Up/Down step a closed dropdown's value; Alt+Down, F4, Space and Enter open
// it; typing jumps to the next item that starts with what was typed. Every one of these is the
// artist's choice.
void testTheClosedFieldKeyboard(Expectations& expectations) {
    Fixture fixture;
    auto& dropdown = *fixture.dropdown;
    for (const char* name : {"Normal", "Multiply", "Screen", "Overlay", "Soft Light"})
        (void)dropdown.addItem(QString::fromLatin1(name));
    dropdown.setItemEnabled(1, false);
    QSignalSpy activated(&dropdown, &kit::KDropdown::activated);
#if !defined(Q_OS_MACOS)
    key(dropdown, Qt::Key_Down);
    expectations.expect(dropdown.currentIndex() == 2 && activated.count() == 1,
                        "Down steps to the next enabled value, skipping the disabled one");
    key(dropdown, Qt::Key_Up);
    expectations.expect(dropdown.currentIndex() == 0, "Up steps back over it too");
    key(dropdown, Qt::Key_End);
    expectations.expect(dropdown.currentIndex() == 4, "End chooses the last value");
    key(dropdown, Qt::Key_Down, Qt::AltModifier);
    expectations.expect(dropdown.isPopupVisible() && dropdown.currentIndex() == 4,
                        "Alt+Down opens the list without changing the value");
    dropdown.hidePopup();
#endif
    key(dropdown, Qt::Key_F4);
    expectations.expect(dropdown.isPopupVisible(), "F4 opens the list");
    dropdown.hidePopup();
    key(dropdown, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
    expectations.expect(dropdown.isPopupVisible(), "Space opens the list");
    dropdown.hidePopup();
    key(dropdown, Qt::Key_O, Qt::NoModifier, QStringLiteral("o"));
    expectations.expect(dropdown.currentText() == QStringLiteral("Overlay"),
                        "typing a letter jumps to the next value that starts with it");
    key(dropdown, Qt::Key_Escape);
    expectations.expect(dropdown.currentText() == QStringLiteral("Overlay") &&
                            !dropdown.isPopupVisible(),
                        "Esc on a closed field changes nothing");
}

void testItemsChangedFollowsEveryListEdit(Expectations& expectations) {
    Fixture fixture;
    auto& dropdown = *fixture.dropdown;
    (void)dropdown.addItem(QStringLiteral("Fit"));
    QSignalSpy items(&dropdown, &kit::KDropdown::itemsChanged);
    dropdown.setItemEnabled(0, false);
    dropdown.setItemEnabled(0, true);
    dropdown.setEnabled(false);
    expectations.expect(items.count() == 3,
                        "enabling or disabling an item, or the dropdown, reports itemsChanged");
    dropdown.setItemEnabled(0, true);
    dropdown.setItemText(0, QStringLiteral("Fit"));
    expectations.expect(items.count() == 3, "a change that changes nothing reports nothing");
}

void testTheKitControlsAreAccessible(Expectations& expectations) {
    Fixture fixture;
    auto& dropdown = *fixture.dropdown;
    dropdown.setAccessibleName(QStringLiteral("Blending"));
    (void)dropdown.addItem(QStringLiteral("Normal"));
    (void)dropdown.addItem(QStringLiteral("Screen"));
    dropdown.setCurrentIndex(1);
    auto* combo = QAccessible::queryAccessibleInterface(&dropdown);
    expectations.expect(combo != nullptr && combo->role() == QAccessible::ComboBox,
                        "a KDropdown is a combo box to assistive technology");
    if (combo != nullptr) {
        expectations.expect(combo->text(QAccessible::Name) == QStringLiteral("Blending") &&
                                combo->text(QAccessible::Value) == QStringLiteral("Screen"),
                            "its name and value are the accessible name and current text");
        expectations.expect(combo->state().collapsed && !combo->state().expanded,
                            "a closed dropdown reads collapsed");
        dropdown.showPopup();
        QCoreApplication::processEvents();
        expectations.expect(combo->state().expanded, "an open one reads expanded");
        dropdown.hidePopup();
    }

    kit::KRadioGroup group(&fixture.host);
    (void)group.addOption(QStringLiteral("Left"));
    (void)group.addOption(QStringLiteral("Right"));
    group.setOptionEnabled(1, false);
    auto* options = QAccessible::queryAccessibleInterface(&group);
    expectations.expect(options != nullptr && options->childCount() == 2,
                        "a KRadioGroup exposes one accessible child per option");
    if (options != nullptr && options->childCount() == 2) {
        auto* left = options->child(0);
        auto* right = options->child(1);
        expectations.expect(left->role() == QAccessible::RadioButton && left->state().checked &&
                                left->text(QAccessible::Name) == QStringLiteral("Left"),
                            "an option is a radio button carrying its label and checked state");
        expectations.expect(right->state().disabled && !right->state().checked,
                            "a disabled option reads disabled");
        expectations.expect(options->indexOfChild(right) == 1 && options->child(1) == right,
                            "the children are stable");
    }
}

void testARadioGroupKeepsADisabledCurrentAndEntersFromMixed(Expectations& expectations) {
    QWidget host;
    kit::KRadioGroup group(&host);
    for (const char* name : {"A", "B", "C"})
        (void)group.addOption(QString::fromLatin1(name));
    group.setCurrentIndex(1);
    group.setOptionEnabled(1, false);
    expectations.expect(group.currentIndex() == 1,
                        "disabling the current option keeps it current, rather than going mixed");
    group.setOptionEnabled(1, true);
    group.setMixed();
    key(group, Qt::Key_Left);
    expectations.expect(group.currentIndex() == 2, "from mixed, Left enters at the last option");
    group.setMixed();
    key(group, Qt::Key_Right);
    expectations.expect(group.currentIndex() == 0, "and Right at the first");
}

// Regression (owner, 2026-09-15): a dropdown hosted inside a QGraphicsProxyWidget (a node card
// row) could not open. The kit popup must stay a real top-level window placed below the anchor's
// visual position -- through the view's zoom, not the widget's unscaled size.
void testAProxiedDropdownOpensATopLevelPopupBelowItself(Expectations& expectations) {
    QGraphicsScene scene;
    QGraphicsView view(&scene);
    view.resize(600, 400);
    view.show();
    auto* dropdown = new kit::KDropdown;
    dropdown->addItem(QStringLiteral("Normal"));
    dropdown->addItem(QStringLiteral("Add"));
    auto* proxy = scene.addWidget(dropdown);
    proxy->setPos(120, 80);
    view.scale(0.5, 0.5);
    QCoreApplication::processEvents();
    dropdown->showPopup();
    QCoreApplication::processEvents();
    auto* popup = dropdown->popup();
    expectations.expect(popup->isVisible() && popup->graphicsProxyWidget() == nullptr &&
                            popup->isWindow(),
                        "a proxied dropdown opens a real top-level popup, not a scene sub-proxy");
    const QRect visual = kit::visualGlobalRect(*dropdown);
    expectations.expect(std::abs(visual.width() - dropdown->width() / 2) <= 1,
                        "the anchor's visual rectangle follows the view's zoom");
    const QRect frame = popup->frameRect().translated(popup->pos());
    expectations.expect(frame.top() == visual.bottom() + 1 + kit::px(kit::Spacing::XXS),
                        "the frame opens just below what is drawn, not below the unscaled widget");
    dropdown->hidePopup();
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("BLOOM_REDUCED_MOTION", "1");
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    Expectations expectations;
    testBuildingTheListNeverReportsAChoice(expectations);
    testChoosingFromThePopupCommitsAndCloses(expectations);
    testADisabledItemIsVisibleButNotSelectable(expectations);
    testAnOversizedValueElidesAndTheFloorIsReal(expectations);
    testTheStateMachineAndDisabledDropdown(expectations);
    testTheClosedFieldIsBorderedWithTheVendoredDoubleChevron(expectations);
    testTheClosedFieldKeyboard(expectations);
    testItemsChangedFollowsEveryListEdit(expectations);
    testTheKitControlsAreAccessible(expectations);
    testARadioGroupKeepsADisabledCurrentAndEntersFromMixed(expectations);
    testAProxiedDropdownOpensATopLevelPopupBelowItself(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
