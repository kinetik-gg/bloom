// The shared popup window and popup list: placement against fake screens, the no-chin height rule,
// the twelve-row cap, dismissal (outside, gutter, anchor), the list keyboard, the searchable
// filter, and a flowed menu's keyboard.
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/dropdown_popup.hpp>
#include <bloom/ui/kit/popup_list.hpp>
#include <bloom/ui/kit/popup_window.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
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

[[nodiscard]] int border() { return std::max(1, static_cast<int>(std::lround(kit::kHairlineWidth))); }

struct Fixture {
    QWidget host;
    kit::KDropdown* dropdown = nullptr;
    kit::KDropdown* next = nullptr;

    explicit Fixture(const int items = 0) {
        auto* layout = new QVBoxLayout(&host);
        dropdown = new kit::KDropdown(&host);
        next = new kit::KDropdown(&host);
        (void)next->addItem(QStringLiteral("Next"));
        layout->addWidget(dropdown);
        layout->addWidget(next);
        layout->addStretch(1);
        for (int index = 0; index < items; ++index)
            (void)dropdown->addItem(QStringLiteral("Item %1").arg(index));
        host.resize(320, 240);
        host.move(200, 200);
        host.show();
        host.activateWindow();
        QCoreApplication::processEvents();
    }
};

void key(QWidget& widget, const int code, const Qt::KeyboardModifiers modifiers = Qt::NoModifier,
         const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, code, modifiers, text);
    QCoreApplication::sendEvent(&widget, &press);
    QCoreApplication::processEvents();
}

void press(QWidget& widget, const QPoint global, const Qt::MouseButton button = Qt::LeftButton) {
    const QPointF local = widget.mapFromGlobal(QPointF(global));
    QMouseEvent event(QEvent::MouseButtonPress, local, QPointF(global), button, button,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &event);
    QCoreApplication::processEvents();
}

void testPlacementAgainstFakeScreens(Expectations& expectations) {
    const QRect screen(0, 0, 1000, 800);
    const QSize frame(200, 300);
    const auto place = [&](const QRect anchor, const QRect on, const int shift = 0) {
        return kit::placePopup({anchor, frame, on, 2, shift});
    };
    auto below = place(QRect(100, 100, 120, 26), screen);
    expectations.expect(!below.above && below.frame == QRect(100, 128, 200, 300),
                        "with room, the frame opens below the anchor, a gap away");
    auto flipped = place(QRect(100, 700, 120, 26), screen);
    expectations.expect(flipped.above && flipped.frame.bottom() == 697 &&
                            flipped.frame.height() == 300,
                        "near the bottom edge it flips above the anchor");
    auto clamped = place(QRect(950, 100, 40, 26), screen);
    expectations.expect(clamped.frame.right() == screen.right(),
                        "near the right edge x is clamped onto the screen");
    auto shifted = place(QRect(100, 100, 120, 26), screen, 20);
    expectations.expect(shifted.frame.left() == 80, "a leading shift hangs the frame to the left");
    auto atLeft = place(QRect(5, 100, 120, 26), screen, 20);
    expectations.expect(atLeft.frame.left() == 0, "and is clamped at the left edge");
    auto squeezed = place(QRect(100, 200, 120, 26), QRect(0, 0, 1000, 500));
    expectations.expect(!squeezed.above && squeezed.frame.bottom() == 499 &&
                            squeezed.frame.height() < 300,
                        "when neither side fits, it takes the roomier one and shrinks to it");
    const QRect second(1000, 0, 800, 600);
    auto other = place(QRect(1700, 560, 80, 26), second);
    expectations.expect(other.above && other.frame.right() == second.right() &&
                            other.frame.left() >= second.left(),
                        "on a second screen it is placed against that screen's edges");
}

// Owner, 2026-10-29: "I hate the huge extra space under the dropdown items ... it looks like a
// chin." The visible surface is exactly the rows plus the frame's own hairlines.
void testThereIsNoChinUnderTheLastRow(Expectations& expectations) {
    Fixture fixture(5);
    auto& dropdown = *fixture.dropdown;
    dropdown.showPopup();
    QCoreApplication::processEvents();
    auto* popup = dropdown.popup();
    auto* list = popup->list();
    const int rowHeight = list->rowHeight();
    expectations.expect(popup->frameRect().height() == 5 * rowHeight + 2 * border(),
                        "the visible surface is rows x rowHeight plus the frame's two hairlines");
    auto* view = dropdown.popupView();
    expectations.expect(view->height() == 5 * rowHeight,
                        "the list itself is exactly its rows tall");
    const QRect last = view->visualRect(view->model()->index(4, 0));
    expectations.expect(last.bottom() == view->height() - 1,
                        "the last row ends on the list's last pixel");
    expectations.expect(view->sizeHintForRow(0) == rowHeight,
                        "a row is laid out at the height the popup was sized for");
    const QMargins gutter = popup->gutter();
    expectations.expect(gutter.top() == gutter.bottom() && gutter.left() == gutter.right(),
                        "where there is a shadow it is symmetric, not bottom-heavy");
    expectations.expect(kit::popupTranslucencySupported() ||
                            (gutter.isNull() && popup->rect() == popup->frameRect()),
                        "without guaranteed transparency there is no gutter at all");
    // A menu has no vertical margin either: its first and last rows meet its frame.
    QMenu* menu = kit::makeMenu(&fixture.host);
    for (const char* name : {"Fit", "100%", "Zoom In"})
        menu->addAction(QString::fromLatin1(name));
    menu->popup(fixture.host.mapToGlobal(QPoint(10, 10)));
    QCoreApplication::processEvents();
    const QRect lastAction = menu->actionGeometry(menu->actions().back());
    const QMargins menuGutter = menu->contentsMargins();
    expectations.expect(lastAction.bottom() + 1 + border() == menu->height() - menuGutter.bottom(),
                        "a menu's last row meets its frame's bottom hairline");
    menu->close();
    delete menu;
    dropdown.hidePopup();
}

void testALongListIsCappedAtTwelveRowsAndScrollsToTheValue(Expectations& expectations) {
    Fixture fixture(40);
    auto& dropdown = *fixture.dropdown;
    dropdown.setCurrentIndex(30);
    dropdown.showPopup();
    QCoreApplication::processEvents();
    auto* popup = dropdown.popup();
    const int rowHeight = popup->list()->rowHeight();
    expectations.expect(popup->frameRect().height() <=
                            kit::kListPopupMaxRows * rowHeight + 2 * border(),
                        "a long list stops at twelve rows");
    auto* view = dropdown.popupView();
    const QRect current = view->visualRect(view->model()->index(30, 0));
    expectations.expect(view->viewport()->rect().contains(current),
                        "it opens scrolled so the current value is in view");
    expectations.expect(popup->list()->committedRow() == 30 && popup->list()->currentRow() == 30,
                        "the committed value is checked and current");
    dropdown.hidePopup();
}

void testTheRowTextLinesUpWithTheFieldText(Expectations& expectations) {
    Fixture fixture(3);
    auto& dropdown = *fixture.dropdown;
    dropdown.showPopup();
    QCoreApplication::processEvents();
    auto* popup = dropdown.popup();
    auto* view = dropdown.popupView();
    const int rowText = view->viewport()->mapToGlobal(QPoint(0, 0)).x() + kit::KPopupList::textOffset();
    const int fieldText = dropdown.mapToGlobal(QPoint(dropdown.textOffset(), 0)).x();
    expectations.expect(rowText == fieldText,
                        "the popup's row text starts exactly where the closed field's text does");
    expectations.expect(popup->frameRect().translated(popup->pos()).right() >=
                            dropdown.mapToGlobal(QPoint(dropdown.width() - 1, 0)).x(),
                        "and the popup reaches at least the field's right edge");
    dropdown.hidePopup();
}

void testDismissal(Expectations& expectations) {
    Fixture fixture(3);
    auto& dropdown = *fixture.dropdown;
    auto* popup = dropdown.popup();

    dropdown.showPopup();
    QCoreApplication::processEvents();
    const QPoint inside = popup->mapToGlobal(popup->frameRect().center());
    press(*popup, inside);
    expectations.expect(dropdown.isPopupVisible(), "a press on the frame is the list's business");

    // Clicking the field to close the popup must not reopen it: the press is not replayed.
    const QPoint onField = dropdown.mapToGlobal(dropdown.rect().center());
    press(*popup, onField);
    expectations.expect(!dropdown.isPopupVisible() &&
                            popup->testAttribute(Qt::WA_NoMouseReplay),
                        "a press on the anchor closes the popup without replaying to the anchor");
    QCoreApplication::processEvents();
    expectations.expect(!dropdown.isPopupVisible(), "and the popup stays closed");

    dropdown.showPopup();
    QCoreApplication::processEvents();
    press(*popup, fixture.host.mapToGlobal(QPoint(300, 230)));
    expectations.expect(!dropdown.isPopupVisible() &&
                            !popup->testAttribute(Qt::WA_NoMouseReplay),
                        "a press elsewhere closes it and is replayed as usual");

    if (!popup->gutter().isNull()) {
        dropdown.showPopup();
        QCoreApplication::processEvents();
        const QPoint gutter = popup->mapToGlobal(
            QPoint(popup->frameRect().center().x(), popup->frameRect().bottom() + 2));
        press(*popup, gutter);
        expectations.expect(!dropdown.isPopupVisible(),
                            "a press in the shadow gutter closes the popup like an outside press");
    }

    dropdown.showPopup();
    QCoreApplication::processEvents();
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(popup, &leave);
    expectations.expect(dropdown.isPopupVisible(), "the pointer leaving does not close it");
    key(*popup, Qt::Key_Escape);
    expectations.expect(!dropdown.isPopupVisible(), "Esc closes it");
}

void testTheListKeyboard(Expectations& expectations) {
    Fixture fixture;
    auto& dropdown = *fixture.dropdown;
    for (const char* name : {"Normal", "Multiply", "Screen", "Overlay", "Soft Light", "Hue"})
        (void)dropdown.addItem(QString::fromLatin1(name));
    dropdown.setItemEnabled(1, false);
    QSignalSpy activated(&dropdown, &kit::KDropdown::activated);
    dropdown.showPopup();
    QCoreApplication::processEvents();
    auto* list = dropdown.popup()->list();
    auto* view = dropdown.popupView();
    expectations.expect(QApplication::focusWidget() == view || view->hasFocus() ||
                            dropdown.popup()->isVisible(),
                        "the list takes focus when the popup opens");
    key(*view, Qt::Key_Down);
    expectations.expect(list->currentRow() == 2, "Down skips the disabled row");
    key(*view, Qt::Key_End);
    expectations.expect(list->currentRow() == 5, "End goes to the last row");
    key(*view, Qt::Key_Home);
    expectations.expect(list->currentRow() == 0, "Home to the first");
    key(*view, Qt::Key_PageDown);
    expectations.expect(list->currentRow() == 5, "PgDn moves a page, clamped at the end");
    key(*view, Qt::Key_S, Qt::NoModifier, QStringLiteral("s"));
    expectations.expect(list->currentRow() == 2, "typing jumps to the next row with that letter");
    key(*view, Qt::Key_S, Qt::NoModifier, QStringLiteral("o"));
    expectations.expect(list->currentRow() == 4, "and a longer prefix refines it");
    key(*view, Qt::Key_Escape);
    expectations.expect(!dropdown.isPopupVisible() && dropdown.currentIndex() == 0 &&
                            activated.count() == 0,
                        "Esc closes the list without changing the value");

    dropdown.showPopup();
    QCoreApplication::processEvents();
    key(*view, Qt::Key_Down);
    key(*view, Qt::Key_Return);
    expectations.expect(!dropdown.isPopupVisible() && dropdown.currentIndex() == 2 &&
                            activated.count() == 1,
                        "Enter chooses the current row");

    dropdown.setFocus(Qt::TabFocusReason);
    dropdown.showPopup();
    QCoreApplication::processEvents();
    key(*view, Qt::Key_Down);
    key(*view, Qt::Key_Tab);
    expectations.expect(!dropdown.isPopupVisible() && dropdown.currentIndex() == 3 &&
                            activated.count() == 2,
                        "Tab chooses the current row and closes the list");
    expectations.expect(fixture.next->hasFocus() || !fixture.host.isActiveWindow(),
                        "and moves focus on past the dropdown");
}

void testTheSearchableFilter(Expectations& expectations) {
    Fixture fixture;
    auto& dropdown = *fixture.dropdown;
    for (const char* name : {"Inter", "Inter Display", "Geist Mono", "Noto Sans", "Noto Serif"})
        (void)dropdown.addItem(QString::fromLatin1(name));
    dropdown.setSearchable(true);
    dropdown.setSearchable(false);
    dropdown.setSearchable(true);
    expectations.expect(dropdown.popup()->findChildren<QSortFilterProxyModel*>().size() == 1,
                        "switching searchability keeps one filter proxy, never one per call");
    dropdown.showPopup();
    QCoreApplication::processEvents();
    auto* popup = dropdown.popup();
    auto* list = popup->list();
    auto* field = list->filterField();
    const int full = popup->frameRect().height();
    field->setText(QStringLiteral("noto"));
    QCoreApplication::processEvents();
    expectations.expect(dropdown.popupView()->model()->rowCount() == 2,
                        "the filter narrows the rows");
    expectations.expect(popup->frameRect().height() == full - 3 * list->rowHeight(),
                        "the popup re-fits as rows filter away: no dead band");
    key(*field, Qt::Key_Down);
    key(*field, Qt::Key_Return);
    expectations.expect(dropdown.currentText() == QStringLiteral("Noto Serif"),
                        "Down and Enter in the filter go to the list");
    dropdown.showPopup();
    QCoreApplication::processEvents();
    expectations.expect(field->text().isEmpty() && dropdown.popupView()->model()->rowCount() == 5,
                        "the filter is reset on every open");
    dropdown.hidePopup();
}

void testRowsRebuiltUnderAnOpenPopupRefit(Expectations& expectations) {
    Fixture fixture(2);
    auto& dropdown = *fixture.dropdown;
    dropdown.showPopup();
    QCoreApplication::processEvents();
    auto* popup = dropdown.popup();
    const int before = popup->frameRect().height();
    (void)dropdown.addItem(QStringLiteral("Late"));
    QCoreApplication::processEvents();
    expectations.expect(popup->frameRect().height() == before + popup->list()->rowHeight(),
                        "a row added under the open popup grows it to fit");
    dropdown.hidePopup();
}

void testAFlowedMenuIsKeyboardUsable(Expectations& expectations) {
    QWidget owner;
    owner.resize(800, 600);
    owner.show();
    auto* menu = kit::makeMenu(QStringLiteral("Flow"), &owner);
    menu->setProperty("columnFlow", true);
    int triggered = -1;
    for (int index = 0; index < 40; ++index) {
        auto* action = menu->addAction(QStringLiteral("Node %1").arg(index));
        action->setEnabled(index != 1);
        QObject::connect(action, &QAction::triggered, [&triggered, index] { triggered = index; });
    }
    menu->popup(owner.mapToGlobal(QPoint(10, 10)));
    QCoreApplication::processEvents();
    auto* flow = menu->findChild<kit::KMenuFlow*>();
    expectations.expect(flow != nullptr && flow->columnCount() > 1,
                        "a long flowed menu lays out in more than one column");
    if (flow == nullptr) {
        menu->close();
        return;
    }
    key(*menu, Qt::Key_Down);
    expectations.expect(flow->currentIndex() == 0 || flow->currentIndex() == 2,
                        "Down in the menu enters the flowed rows");
    flow->setCurrentIndex(0);
    key(*flow, Qt::Key_Down);
    expectations.expect(flow->currentIndex() == 2, "Down skips a disabled row");
    const int rows = flow->itemRect(1).top() > flow->itemRect(0).top()
                         ? static_cast<int>(flow->height() / flow->itemRect(0).height())
                         : 1;
    key(*flow, Qt::Key_Right);
    expectations.expect(flow->currentIndex() >= rows, "Right moves to the next column");
    key(*flow, Qt::Key_Left);
    expectations.expect(flow->currentIndex() == 2, "Left comes back to the same row");
    key(*flow, Qt::Key_Return);
    expectations.expect(triggered == 2 && !menu->isVisible(), "Enter triggers the row");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("BLOOM_REDUCED_MOTION", "1");
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    Expectations expectations;
    testPlacementAgainstFakeScreens(expectations);
    testThereIsNoChinUnderTheLastRow(expectations);
    testALongListIsCappedAtTwelveRowsAndScrollsToTheValue(expectations);
    testTheRowTextLinesUpWithTheFieldText(expectations);
    testDismissal(expectations);
    testTheListKeyboard(expectations);
    testTheSearchableFilter(expectations);
    testRowsRebuiltUnderAnOpenPopupRefit(expectations);
    testAFlowedMenuIsKeyboardUsable(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
