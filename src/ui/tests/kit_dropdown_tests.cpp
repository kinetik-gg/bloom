#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/dropdown_popup.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QAbstractItemModel>
#include <QApplication>
#include <QEvent>
#include <QFile>
#include <QImage>
#include <QListView>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QString>
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

void clickRow(kit::KDropdown& dropdown, const int row) {
    auto* view = dropdown.popupView();
    const QRect rect = view->visualRect(view->model()->index(row, 0));
    const QPointF centre = rect.center();
    QMouseEvent release(QEvent::MouseButtonRelease, centre, view->viewport()->mapToGlobal(centre),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(view->viewport(), &release);
    QCoreApplication::processEvents();
}

void testTheClosedFieldCarriesTheCurrentValue(Expectations& expectations) {
    Fixture fixture;
    auto& dropdown = *fixture.dropdown;
    expectations.expect(dropdown.count() == 0, "a fresh dropdown is empty");
    expectations.expect(dropdown.currentIndex() == -1, "an empty dropdown has no current value");

    const int linear = dropdown.addItem(QStringLiteral("Linear"), QStringLiteral("linear"));
    const int ease = dropdown.addItem(QStringLiteral("Ease In Out"));
    expectations.expect(dropdown.count() == 2, "items are added in order");
    expectations.expect(dropdown.currentIndex() == linear,
                        "the first item added becomes the current value");
    expectations.expect(dropdown.currentText() == QStringLiteral("Linear"),
                        "the closed field carries the current value");
    expectations.expect(dropdown.itemData(linear).toString() == QStringLiteral("linear"),
                        "an item's payload survives");

    QSignalSpy changed(&dropdown, &kit::KDropdown::currentIndexChanged);
    dropdown.setCurrentIndex(ease);
    expectations.expect(dropdown.currentText() == QStringLiteral("Ease In Out"),
                        "setting the index changes the value");
    expectations.expect(changed.count() == 1, "changing the value reports it exactly once");
    dropdown.setCurrentIndex(ease);
    expectations.expect(changed.count() == 1, "re-setting the same value reports nothing");
}

void testThePopupOpensOnTheRaisedSurfaceAndCloses(Expectations& expectations) {
    Fixture fixture;
    auto& dropdown = *fixture.dropdown;
    (void)dropdown.addItem(QStringLiteral("Linear"));
    (void)dropdown.addItem(QStringLiteral("Ease In Out"));

    expectations.expect(!dropdown.isPopupVisible(), "the popup starts closed");
    dropdown.showPopup();
    QCoreApplication::processEvents();
    expectations.expect(dropdown.isPopupVisible(), "the popup opens");
    expectations.expect(dropdown.visualState() == kit::State::Pressed,
                        "an open dropdown reads as pressed, not as resting");
    expectations.expect(dropdown.popupView()->model()->rowCount() == 2,
                        "the popup lists every item");
    expectations.expect(dropdown.popupView()->textElideMode() == Qt::ElideRight,
                        "an oversized row ellipsizes rather than widening the popup");
    expectations.expect(dropdown.popup()->objectName() == QStringLiteral("kDropdownPopup"),
                        "the popup is findable by name");

    dropdown.hidePopup();
    QCoreApplication::processEvents();
    expectations.expect(!dropdown.isPopupVisible(), "the popup closes");
    expectations.expect(dropdown.visualState() != kit::State::Pressed,
                        "closing the popup releases the pressed state");
}

void testChoosingFromThePopupCommitsAndCloses(Expectations& expectations) {
    Fixture fixture;
    auto& dropdown = *fixture.dropdown;
    (void)dropdown.addItem(QStringLiteral("Linear"));
    const int ease = dropdown.addItem(QStringLiteral("Ease In Out"));

    QSignalSpy changed(&dropdown, &kit::KDropdown::currentIndexChanged);
    dropdown.showPopup();
    QCoreApplication::processEvents();
    clickRow(dropdown, ease);

    expectations.expect(dropdown.currentIndex() == ease, "clicking a row commits that value");
    expectations.expect(changed.count() == 1, "committing reports the change once");
    expectations.expect(!dropdown.isPopupVisible(), "committing closes the popup");
}

void testADisabledItemIsVisibleButNotSelectable(Expectations& expectations) {
    Fixture fixture;
    auto& dropdown = *fixture.dropdown;
    const int linear = dropdown.addItem(QStringLiteral("Linear"));
    const int unavailable = dropdown.addItem(QStringLiteral("Bezier"));
    dropdown.setItemEnabled(unavailable, false);

    expectations.expect(!dropdown.isItemEnabled(unavailable), "the item reports as disabled");
    expectations.expect(dropdown.count() == 2,
                        "a disabled item stays in the list rather than disappearing");
    expectations.expect(dropdown.itemText(unavailable) == QStringLiteral("Bezier"),
                        "a disabled item stays readable");

    QSignalSpy changed(&dropdown, &kit::KDropdown::currentIndexChanged);
    dropdown.setCurrentIndex(unavailable);
    expectations.expect(dropdown.currentIndex() == linear,
                        "setCurrentIndex refuses a disabled item: the programmatic path is closed "
                        "too, not just the click");
    expectations.expect(changed.count() == 0, "a refused selection reports nothing");

    dropdown.showPopup();
    QCoreApplication::processEvents();
    clickRow(dropdown, unavailable);
    expectations.expect(dropdown.currentIndex() == linear,
                        "clicking a disabled row commits nothing");
    expectations.expect(dropdown.isPopupVisible(),
                        "a disabled row swallows the click rather than closing the popup");
    dropdown.hidePopup();
}

void testAnOversizedValueElidesInTheClosedField(Expectations& expectations) {
    Fixture fixture;
    auto& dropdown = *fixture.dropdown;
    const QString longValue =
        QStringLiteral("A qualified display transform with a very long descriptive name");
    (void)dropdown.addItem(longValue);
    QCoreApplication::processEvents();

    dropdown.resize(120, dropdown.sizeHint().height());
    QCoreApplication::processEvents();
    const QString displayed = dropdown.displayedText();
    expectations.expect(displayed != longValue, "an oversized value does not print in full");
    expectations.expect(displayed.endsWith(QChar(0x2026)),
                        "an oversized value ends in an ellipsis");
    expectations.expect(dropdown.currentText() == longValue,
                        "eliding is presentation only: the value itself is untouched");

    dropdown.resize(dropdown.sizeHint());
    QCoreApplication::processEvents();
    expectations.expect(dropdown.displayedText() == longValue,
                        "given its preferred width the value prints in full");
}

void testTheStateMachineAndDisabledDropdown(Expectations& expectations) {
    Fixture fixture;
    auto& dropdown = *fixture.dropdown;
    (void)dropdown.addItem(QStringLiteral("Linear"));

    // Showing the host hands focus to its only focusable child, so resting has to be established
    // rather than assumed.
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
    dropdown.showPopup();
    QCoreApplication::processEvents();
    // showPopup() is reachable programmatically, but a disabled control must never be opened by
    // the artist; the click path is what is closed here.
    dropdown.hidePopup();
    dropdown.setEnabled(true);
}

// task U8, issue #131, fix 3: the design sheet's dropdown chrome -- bordered closed field with the
// vendored double up/down chevron, bordered popup at Radius::Small.
void testTheClosedFieldIsBorderedWithTheVendoredDoubleChevron(Expectations& expectations) {
    Fixture fixture;
    auto& dropdown = *fixture.dropdown;
    (void)dropdown.addItem(QStringLiteral("Linear"));
    QCoreApplication::processEvents();

    expectations.expect(
        QFile::exists(kit::iconResourcePath(kit::IconId::CaretUpDown, kit::IconWeight::Regular)) &&
            QFile::exists(kit::iconResourcePath(kit::IconId::CaretUpDown, kit::IconWeight::Fill)),
        "the vendored double-chevron asset backs IconId::CaretUpDown in both weights");

    // Drive the widget to REST before sampling: on some offscreen platforms (CI's Qt 6.8.3) the
    // freshly shown window hands activation focus to its only focusable child on a later
    // event-loop pass, and a cursor parked at the origin can leave it hovered -- both repaint the
    // border in a state colour and would make the rest-state pins fail for environmental reasons.
    QCoreApplication::processEvents();
    dropdown.clearFocus();
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(&dropdown, &leave);
    QCoreApplication::processEvents();

    const QPixmap rendered = dropdown.grab();
    expectations.expect(!rendered.isNull(), "the bordered closed field renders offscreen");

    // task U8, formal amendment 1, A2: the closed field's fill is ControlSurface (not Field), and
    // it still borders with Border (#222222) as A2 asked to verify.
    const QImage image = rendered.toImage();
    const QColor controlSurface = kit::color(kit::Color::ControlSurface);
    const QColor border = kit::color(kit::Color::Border);
    const auto near = [](const QColor& left, const QColor& right) {
        return std::abs(left.red() - right.red()) <= 2 &&
               std::abs(left.green() - right.green()) <= 2 &&
               std::abs(left.blue() - right.blue()) <= 2;
    };
    bool sawFill = false;
    bool sawBorder = false;
    for (int y = 0; y < image.height() && (!sawFill || !sawBorder); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (near(pixel, controlSurface)) {
                sawFill = true;
            }
            if (near(pixel, border)) {
                sawBorder = true;
            }
        }
    }
    expectations.expect(sawFill, "the closed field really paints ControlSurface");
    expectations.expect(sawBorder, "the closed field really paints the Border hairline");
}

void testThePopupFrameIsBorderedAtRadiusSmall(Expectations& expectations) {
    Fixture fixture;
    auto& dropdown = *fixture.dropdown;
    (void)dropdown.addItem(QStringLiteral("Linear"));
    const QString sheet = dropdown.popup()->styleSheet();
    expectations.expect(sheet.contains(QStringLiteral("border: %1px solid %2;")
                                           .arg(static_cast<int>(kit::kHairlineWidth))
                                           .arg(kit::hex(kit::Color::Border))),
                        "the popup frame carries the Border hairline");
    expectations.expect(
        sheet.contains(
            QStringLiteral("border-radius: %1px;").arg(kit::radiusPx(kit::Radius::Small, 0))),
        "the popup frame corners are Radius::Small, matching the closed field");
    expectations.expect(sheet.contains(kit::hex(kit::Color::SurfaceRaised)),
                        "the popup rests on SurfaceRaised");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    Expectations expectations;
    testTheClosedFieldCarriesTheCurrentValue(expectations);
    testThePopupOpensOnTheRaisedSurfaceAndCloses(expectations);
    testChoosingFromThePopupCommitsAndCloses(expectations);
    testADisabledItemIsVisibleButNotSelectable(expectations);
    testAnOversizedValueElidesInTheClosedField(expectations);
    testTheStateMachineAndDisabledDropdown(expectations);
    testTheClosedFieldIsBorderedWithTheVendoredDoubleChevron(expectations);
    testThePopupFrameIsBorderedAtRadiusSmall(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
