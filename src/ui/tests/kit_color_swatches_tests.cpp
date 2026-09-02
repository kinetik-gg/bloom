#include <bloom/ui/kit/color_swatches.hpp>
#include <bloom/ui/kit/theme.hpp>

#include <QApplication>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QVBoxLayout>
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

struct Fixture {
    QWidget host;
    kit::KColorSwatchRow* row = nullptr;

    Fixture() {
        auto* layout = new QVBoxLayout(&host);
        row = new kit::KColorSwatchRow(&host);
        layout->addWidget(row);
        host.resize(320, 60);
        host.show();
        host.activateWindow();
        QCoreApplication::processEvents();
    }
};

void testRecentStoreIsMostRecentFirstAndDedups(Expectations& expectations) {
    kit::KRecentColorStore store;
    const kit::KColor red = kit::KColor::fromRgba(1.0F, 0.0F, 0.0F);
    const kit::KColor green = kit::KColor::fromRgba(0.0F, 1.0F, 0.0F);
    const kit::KColor blue = kit::KColor::fromRgba(0.0F, 0.0F, 1.0F);

    store.push(red);
    store.push(green);
    store.push(blue);
    expectations.expect(store.colors().size() == 3, "three distinct pushes keep three entries");
    expectations.expect(store.colors()[0] == blue, "the most recently pushed color is first");

    store.push(red);
    expectations.expect(store.colors().size() == 3,
                        "re-pushing an existing color does not duplicate it");
    expectations.expect(store.colors()[0] == red,
                        "re-pushing an existing color moves it to the front");
    expectations.expect(store.colors()[1] == blue && store.colors()[2] == green,
                        "the rest keep their relative order");
}

void testRecentStoreEvictsOldestPastCapacity(Expectations& expectations) {
    kit::KRecentColorStore store(2);
    store.push(kit::KColor::fromRgba(1.0F, 0.0F, 0.0F));
    store.push(kit::KColor::fromRgba(0.0F, 1.0F, 0.0F));
    store.push(kit::KColor::fromRgba(0.0F, 0.0F, 1.0F));
    expectations.expect(store.colors().size() == 2, "capacity is enforced");
    expectations.expect(store.colors()[0] == kit::KColor::fromRgba(0.0F, 0.0F, 1.0F),
                        "the newest push survives");
    expectations.expect(store.colors()[1] == kit::KColor::fromRgba(0.0F, 1.0F, 0.0F),
                        "the second-newest push survives");
}

void testClickingASlotAppliesItsColor(Expectations& expectations) {
    Fixture fixture;
    auto& row = *fixture.row;
    const kit::KColor teal = kit::KColor::fromRgba(0.0F, 0.6F, 0.6F);
    row.setSlotColor(2, teal);

    QSignalSpy activated(&row, &kit::KColorSwatchRow::colorActivated);
    const QRectF slot = row.slotRect(2);
    const QPointF centre = slot.center();
    QMouseEvent press(QEvent::MouseButtonPress, centre, row.mapToGlobal(centre.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&row, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, centre, row.mapToGlobal(centre.toPoint()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&row, &release);
    QCoreApplication::processEvents();

    expectations.expect(activated.count() == 1, "a quick click activates the slot exactly once");
    expectations.expect(qvariant_cast<kit::KColor>(activated.constFirst().constFirst()) == teal,
                        "the activated color is the pinned slot's color");
}

void testContextMenuSetsASlotToTheCurrentColor(Expectations& expectations) {
    Fixture fixture;
    auto& row = *fixture.row;
    const kit::KColor magenta = kit::KColor::fromRgba(1.0F, 0.0F, 1.0F);
    row.setCurrentColor(magenta);
    expectations.expect(!row.isSlotPinned(0), "a fresh slot starts unpinned");

    QSignalSpy pinned(&row, &kit::KColorSwatchRow::slotPinned);
    const QPointF centre = row.slotRect(0).center();
    QContextMenuEvent contextEvent(QContextMenuEvent::Mouse, centre.toPoint(),
                                   row.mapToGlobal(centre.toPoint()));
    QCoreApplication::sendEvent(&row, &contextEvent);
    QCoreApplication::processEvents();

    expectations.expect(row.isSlotPinned(0), "a context-menu gesture pins the slot");
    expectations.expect(row.slotColor(0) == magenta,
                        "the pinned slot takes the row's current color");
    expectations.expect(pinned.count() == 1, "the pin is reported exactly once");

    row.clearSlot(0);
    expectations.expect(!row.isSlotPinned(0), "clearSlot() releases the pin");
    expectations.expect(!row.slotColor(0).has_value(),
                        "an unbound, unpinned slot has no color at all");
}

void testSlotsReadThroughTheBoundStoreWhenUnpinned(Expectations& expectations) {
    Fixture fixture;
    auto& row = *fixture.row;
    kit::KRecentColorStore store;
    store.push(kit::KColor::fromRgba(0.2F, 0.2F, 0.2F));
    store.push(kit::KColor::fromRgba(0.4F, 0.4F, 0.4F));
    row.setRecentColorStore(&store);

    expectations.expect(row.slotColor(0) == store.colors()[0],
                        "an unpinned slot mirrors the store's most-recent-first order");
    expectations.expect(row.slotColor(1) == store.colors()[1], "and so on down the list");
    expectations.expect(!row.slotColor(row.slotCount() - 1).has_value(),
                        "a slot past what the store has to offer is empty, not garbage");

    row.setSlotColor(0, kit::KColor::fromRgba(1.0F, 1.0F, 1.0F));
    expectations.expect(row.slotColor(0) == kit::KColor::fromRgba(1.0F, 1.0F, 1.0F),
                        "a pinned slot overrides what the store would otherwise show there");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    Expectations expectations;
    testRecentStoreIsMostRecentFirstAndDedups(expectations);
    testRecentStoreEvictsOldestPastCapacity(expectations);
    testClickingASlotAppliesItsColor(expectations);
    testContextMenuSetsASlotToTheCurrentColor(expectations);
    testSlotsReadThroughTheBoundStoreWhenUnpinned(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
