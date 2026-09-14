// task U8, issue #131, formal amendment 2, A7: KPanelSwitcher's own unit contract, mirroring
// kit_dropdown_tests.cpp's shape -- the switcher composes KDropdownPopup (tested there) but adds
// its own icon-bearing closed field, content-hugging sizing, and tooltip-forwarding behavior.

#include <bloom/ui/kit/dropdown_popup.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/panel_switcher.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QFont>
#include <QIcon>
#include <QPixmap>
#include <QSignalSpy>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
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
    kit::KPanelSwitcher* switcher = nullptr;

    Fixture() {
        auto* layout = new QVBoxLayout(&host);
        switcher = new kit::KPanelSwitcher(&host);
        layout->addWidget(switcher);
        host.resize(300, 120);
        host.show();
        host.activateWindow();
        QCoreApplication::processEvents();
    }
};

void testTheFontIsTypeUiAtNaturalCaseNeverUppercase(Expectations& expectations) {
    Fixture fixture;
    const QFont font = fixture.switcher->font();
    expectations.expect(font.capitalization() != QFont::AllUppercase,
                        "formal amendment 2, A8: the switcher never takes UiSmall's uppercase "
                        "transform");
    expectations.expect(font.families().contains(kit::interfaceFontFamily()),
                        "the switcher is Type::UI's own family");
}

void testTheClosedFieldCarriesTheCurrentValueAndIcon(Expectations& expectations) {
    Fixture fixture;
    auto& switcher = *fixture.switcher;
    expectations.expect(switcher.count() == 0, "a fresh switcher is empty");
    expectations.expect(switcher.currentIndex() == -1, "an empty switcher has no current value");

    const QIcon viewerIcon = kit::icon(kit::IconId::Stack, kit::Size::IconMedium);
    const int viewer =
        switcher.addItem(viewerIcon, QStringLiteral("Viewer"), QStringLiteral("bloom.viewer"));
    const int nodes =
        switcher.addItem(QIcon{}, QStringLiteral("Nodes"), QStringLiteral("bloom.nodes"));

    expectations.expect(switcher.count() == 2, "items are added in order");
    expectations.expect(switcher.currentIndex() == viewer,
                        "the first item added becomes the current value");
    expectations.expect(switcher.itemText(viewer) == QStringLiteral("Viewer"),
                        "the registry's Title-case display name is stored verbatim");
    expectations.expect(!switcher.itemIcon(viewer).isNull(),
                        "the icon-bearing item keeps its icon");
    expectations.expect(switcher.itemIcon(nodes).isNull(),
                        "an item added with a null QIcon carries no icon at all");
    expectations.expect(switcher.itemData(viewer).toString() == QStringLiteral("bloom.viewer"),
                        "an item's payload survives");
    expectations.expect(switcher.currentData().toString() == QStringLiteral("bloom.viewer"),
                        "currentData() reads the current item's payload");

    QSignalSpy changed(&switcher, &kit::KPanelSwitcher::currentIndexChanged);
    switcher.setCurrentIndex(nodes);
    expectations.expect(switcher.currentIndex() == nodes, "setCurrentIndex changes the value");
    expectations.expect(changed.count() == 1, "changing the value reports it exactly once");
    switcher.setCurrentIndex(nodes);
    expectations.expect(changed.count() == 1, "re-setting the same value reports nothing");
}

void testFindDataLocatesAnItemByItsPayload(Expectations& expectations) {
    Fixture fixture;
    auto& switcher = *fixture.switcher;
    (void)switcher.addItem(QIcon{}, QStringLiteral("Assets"), QStringLiteral("bloom.assets"));
    const int viewer =
        switcher.addItem(QIcon{}, QStringLiteral("Viewer"), QStringLiteral("bloom.viewer"));

    expectations.expect(switcher.findData(QStringLiteral("bloom.viewer")) == viewer,
                        "findData locates the item carrying that payload");
    expectations.expect(switcher.findData(QStringLiteral("bloom.nonexistent")) == -1,
                        "findData reports -1 for a payload no item carries");
}

void testTheClosedFieldsTooltipTracksTheCurrentItem(Expectations& expectations) {
    Fixture fixture;
    auto& switcher = *fixture.switcher;
    const int assets =
        switcher.addItem(QIcon{}, QStringLiteral("Assets"), QStringLiteral("bloom.assets"));
    const int missing = switcher.addItem(QIcon{}, QStringLiteral("Editor unavailable"),
                                         QStringLiteral("bloom.missing"));
    switcher.setItemToolTip(missing, QStringLiteral("Unavailable editor: bloom.missing"));

    expectations.expect(switcher.toolTip().isEmpty(),
                        "the resting current item (Assets) carries no tooltip");
    switcher.setCurrentIndex(missing);
    expectations.expect(switcher.toolTip() == QStringLiteral("Unavailable editor: bloom.missing"),
                        "switching to the unavailable item surfaces its ported tooltip, mirroring "
                        "QComboBox's own Qt::ToolTipRole forwarding");
    switcher.setCurrentIndex(assets);
    expectations.expect(switcher.toolTip().isEmpty(),
                        "switching away clears the tooltip back to that item's own (empty) one");
}

void testThePopupOpensOnTheRaisedSurfaceAndCloses(Expectations& expectations) {
    Fixture fixture;
    auto& switcher = *fixture.switcher;
    (void)switcher.addItem(QIcon{}, QStringLiteral("Viewer"), QStringLiteral("bloom.viewer"));
    (void)switcher.addItem(QIcon{}, QStringLiteral("Nodes"), QStringLiteral("bloom.nodes"));

    expectations.expect(!switcher.isPopupVisible(), "the popup starts closed");
    switcher.showPopup();
    QCoreApplication::processEvents();
    expectations.expect(switcher.isPopupVisible(), "the popup opens");
    expectations.expect(switcher.visualState() == kit::State::Pressed,
                        "an open switcher reads as pressed, not resting");

    switcher.hidePopup();
    QCoreApplication::processEvents();
    expectations.expect(!switcher.isPopupVisible(), "the popup closes");
    expectations.expect(switcher.visualState() != kit::State::Pressed,
                        "closing the popup releases the pressed state");
}

void testTheFieldHugsItsContentRatherThanStretching(Expectations& expectations) {
    Fixture fixture;
    auto& switcher = *fixture.switcher;
    (void)switcher.addItem(kit::icon(kit::IconId::Folder, kit::Size::IconMedium),
                           QStringLiteral("Assets"), QStringLiteral("bloom.assets"));

    // formal amendment 2, A7: the field is content-hugging, not the QComboBox-era stretched
    // field the owner rejected. A short label with one icon comes in well under a typical
    // header row's own width.
    expectations.expect(switcher.sizeHint().width() < 150,
                        "the closed field hugs a short label plus one icon");

    expectations.expect(switcher.sizeHint().height() == kit::px(kit::Size::Control),
                        "formal amendment 2, A10: the field is Control (26) tall");

    const QPixmap rendered = switcher.grab();
    expectations.expect(!rendered.isNull(), "the switcher renders offscreen");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    Expectations expectations;
    testTheFontIsTypeUiAtNaturalCaseNeverUppercase(expectations);
    testTheClosedFieldCarriesTheCurrentValueAndIcon(expectations);
    testFindDataLocatesAnItemByItsPayload(expectations);
    testTheClosedFieldsTooltipTracksTheCurrentItem(expectations);
    testThePopupOpensOnTheRaisedSurfaceAndCloses(expectations);
    testTheFieldHugsItsContentRatherThanStretching(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
