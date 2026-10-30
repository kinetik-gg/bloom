// The kit's text, row, surface and list primitives: KLabel's height rule and tone, the one
// selected-row recipe and row separator, the checkerboard tokens, the split handle's grab loss,
// the anchor grid's round dots, the radio dot on the pixel grid, and the item-view rows the kit
// proxy style paints (tree carets, an un-notched selected row).

#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/split_handle.hpp>
#include <bloom/ui/kit/surfaces.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QEvent>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPoint>
#include <QRegion>
#include <QStandardItemModel>
#include <QTreeView>
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

// A single-line label is one control tall; a wrapped one grows to its text instead of clipping to
// the first line -- whether it was wrapped through KLabel or through a plain QLabel pointer.
void testKLabelWrapsAndTones(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto* single = new kit::KLabel(QStringLiteral("One line"), &host);
    auto* wrapped = new kit::KLabel(
        QStringLiteral("A long explanatory hint that cannot possibly fit on a single line at this "
                       "width, so it has to wrap onto several lines and grow to show them all."),
        &host);
    wrapped->setWordWrap(true);
    auto* viaQLabel = new kit::KLabel(wrapped->text(), &host);
    static_cast<QLabel*>(viaQLabel)->setWordWrap(true);
    layout->addWidget(single);
    layout->addWidget(wrapped);
    layout->addWidget(viaQLabel);
    host.resize(160, 400);
    host.show();
    QCoreApplication::processEvents();

    expectations.expect(single->height() == kit::px(kit::Size::Control),
                        "a single-line label is exactly one control tall");
    expectations.expect(wrapped->height() > kit::px(kit::Size::Control) * 2,
                        "a word-wrapped label grows to its wrapped height");
    expectations.expect(viaQLabel->height() > kit::px(kit::Size::Control) * 2,
                        "a wrap set through a QLabel pointer still grows by the time it is shown");

    single->setTone(kit::Color::Warn);
    expectations.expect(single->palette().color(QPalette::Active, QPalette::WindowText) ==
                            kit::color(kit::Color::Warn),
                        "setTone colours the label by its token");
    expectations.expect(single->palette().color(QPalette::Disabled, QPalette::WindowText) ==
                            kit::disabledInk(kit::color(kit::Color::Warn)),
                        "a disabled toned label fades from its own tone, the one disabled recipe");
    expectations.expect(single->tone() == kit::Color::Warn, "the tone is reported back");

    // Inside an item view (a list row's cell), a label still paints in its own tone, never the
    // viewport's Text ink.
    QTreeView view;
    auto* inCell = new kit::KLabel(QStringLiteral("Queued"), view.viewport());
    inCell->setTone(kit::Color::Muted);
    expectations.expect(inCell->foregroundRole() == QPalette::WindowText &&
                            inCell->palette().color(inCell->foregroundRole()) == kit::color(kit::Color::Muted),
                        "a label in an item view paints in its tone");
}

// KRow and the timeline share one recipe: the opaque wash over the row's surface, an Accent edge
// at the leading side, and one snapped separator along the bottom.
void testTheSelectedRowRecipeAndTheSeparator(Expectations& expectations) {
    for (const qreal dpr : {1.0, 1.5}) {
        kit::KRow row;
        row.resize(200, kit::px(kit::Size::ListRow));
        row.setRowState(0, true);
        QImage image(static_cast<int>(200 * dpr),
                     static_cast<int>(kit::px(kit::Size::ListRow) * dpr),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        row.render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
        const std::string at = " at DPR " + std::to_string(dpr);
        expectations.expect(image.pixelColor(image.width() / 2, image.height() / 2) ==
                                kit::selectionWash(kit::Color::Surface),
                            "a selected row is the selection wash" + at);
        const int edge = static_cast<int>(std::round(kit::px(kit::Size::SelectionEdge) * dpr));
        expectations.expect(image.pixelColor(edge - 1, image.height() / 2) ==
                                    kit::color(kit::Color::Accent) &&
                                image.pixelColor(edge, image.height() / 2) ==
                                    kit::selectionWash(kit::Color::Surface),
                            "the leading Accent edge is exactly SelectionEdge wide" + at);
        expectations.expect(image.pixelColor(image.width() / 2, image.height() - 1) ==
                                    kit::color(kit::Color::Background) &&
                                image.pixelColor(image.width() / 2, image.height() - 2) !=
                                    kit::color(kit::Color::Background),
                            "the separator is one opaque device-pixel row at the bottom" + at);
    }
}

void testTheCheckerboardUsesTheCheckerTokens(Expectations& expectations) {
    QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    kit::drawAlphaCheckerboard(painter, QRectF(0, 0, 32, 32), kit::px(kit::Size::CheckerCell));
    painter.end();
    expectations.expect(image.pixelColor(1, 1) == kit::color(kit::Color::CheckerDark) &&
                            image.pixelColor(9, 1) == kit::color(kit::Color::CheckerLight),
                        "the checkerboard alternates the mid-grey CheckerDark and CheckerLight");
    expectations.expect(image.pixelColor(31, 31).alpha() == 255,
                        "the square checkerboard reaches its corners");
}

void testTheSplitHandleEndsADragOnGrabLoss(Expectations& expectations) {
    kit::KSplitHandle handle(Qt::Horizontal);
    handle.resize(kit::px(kit::Size::SplitHandle), 100);
    handle.show();
    int finished = 0;
    QObject::connect(&handle, &kit::KSplitHandle::dragFinished, [&finished] { ++finished; });
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(2, 2), QPointF(2, 2), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&handle, &press);
    QEvent ungrab(QEvent::UngrabMouse);
    QCoreApplication::sendEvent(&handle, &ungrab);
    expectations.expect(finished == 1, "losing the grab mid-drag finishes the drag");
    int dragged = 0;
    QObject::connect(&handle, &kit::KSplitHandle::dragged, [&dragged](int) { ++dragged; });
    QMouseEvent move(QEvent::MouseMove, QPointF(40, 2), QPointF(40, 2), Qt::NoButton, Qt::NoButton,
                     Qt::NoModifier);
    QCoreApplication::sendEvent(&handle, &move);
    expectations.expect(dragged == 0, "a hover after the lost grab resizes nothing");
}

class Grid final : public kit::KAnchorGrid {
  public:
    using KAnchorGrid::KAnchorGrid;
    int selectedPoint() const override { return 4; }
    void choosePoint(int) override {}
};

void testAnchorDotsAreRoundAndTheRadioDotIsOnTheGrid(Expectations& expectations) {
    Grid grid;
    QImage image(grid.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    grid.render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
    const QRect dot = grid.pointRect(0);
    expectations.expect(image.pixelColor(dot.left(), dot.top()).alpha() < 128 &&
                            image.pixelColor(dot.center()).alpha() == 255,
                        "an anchor dot is round: its corner is clear, its centre filled");

    kit::KRadioGroup radio;
    radio.setPresentation(kit::KRadioGroup::Presentation::Discrete);
    radio.addOption(QStringLiteral("Corner"));
    radio.resize(radio.sizeHint());
    const QImage marks = radio.grab().toImage();
    const QRect row = radio.optionRect(0);
    const int mark = kit::px(kit::Size::RadioMark);
    const int markLeft = row.left() + kit::px(kit::Spacing::XS);
    const int markTop = row.center().y() + 1 - mark / 2;
    const int dot2 = kit::px(kit::Size::RadioDot);
    const QColor accent = kit::color(kit::Color::Accent);
    const int dotLeft = markLeft + (mark - dot2) / 2;
    const int dotTop = markTop + (mark - dot2) / 2;
    expectations.expect(marks.pixelColor(dotLeft + dot2 / 2, dotTop + dot2 / 2) == accent,
                        "the selected radio's dot is solid Accent at its centre");
    kit::KRadioGroup longLabels;
    longLabels.addOption(QStringLiteral("A rather long first choice"));
    longLabels.addOption(QStringLiteral("A rather long second choice"));
    expectations.expect(longLabels.minimumSizeHint().width() < longLabels.sizeHint().width(),
                        "labels elide, so the minimum is narrower than the widest label");
}

// Item views are painted by the kit proxy style: the tree discloses with the Phosphor carets, and
// a selected row is one continuous wash from the far left -- no notch at the indentation.
void testTreeRowsAreUnnotchedAndUseTheKitCarets(Expectations& expectations) {
    QStandardItemModel model;
    auto* parent = new QStandardItem(QStringLiteral("Folder"));
    parent->appendRow(new QStandardItem(QStringLiteral("Child")));
    model.appendRow(parent);
    model.appendRow(new QStandardItem(QStringLiteral("Sibling")));
    QTreeView tree;
    tree.setModel(&model);
    tree.setHeaderHidden(true);
    tree.setSelectionBehavior(QAbstractItemView::SelectRows);
    tree.setIndentation(kit::px(kit::Size::ToggleCell));
    tree.resize(240, 120);
    tree.expandAll();
    tree.show();
    QCoreApplication::processEvents();
    const QModelIndex child = model.index(0, 0, model.index(0, 0));
    tree.selectionModel()->select(child,
                                  QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QCoreApplication::processEvents();
    const QImage image = tree.viewport()->grab().toImage();
    const QRect childRow = tree.visualRect(child);
    const int y = childRow.center().y();
    const QColor wash = kit::selectionWash(kit::Color::Background);
    const QColor washAlternate = kit::selectionWash(kit::Color::Surface);
    const auto isWash = [&](const QColor& value) {
        return value == wash || value == washAlternate;
    };
    expectations.expect(isWash(image.pixelColor(childRow.left() - 4, y)),
                        "the indentation of a selected row carries the selection wash (no notch)");
    expectations.expect(image.pixelColor(0, y) == kit::color(kit::Color::Accent),
                        "the selected row's leading edge is Accent");
    expectations.expect(isWash(image.pixelColor(childRow.right() - 4, y)),
                        "and the cell itself carries the same wash");

    // The parent's caret is a real glyph in the branch column, in the kit's Muted ink family.
    const QRect parentRow = tree.visualRect(model.index(0, 0));
    bool inked = false;
    for (int x = 0; x < parentRow.left() && !inked; ++x)
        for (int row = parentRow.top(); row <= parentRow.bottom(); ++row)
            if (image.pixelColor(x, row).lightness() >
                kit::color(kit::Color::Surface).lightness() + 40) {
                inked = true;
                break;
            }
    expectations.expect(inked, "the expandable row paints a caret in its branch column");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    Expectations expectations;
    testKLabelWrapsAndTones(expectations);
    testTheSelectedRowRecipeAndTheSeparator(expectations);
    testTheCheckerboardUsesTheCheckerTokens(expectations);
    testTheSplitHandleEndsADragOnGrabLoss(expectations);
    testAnchorDotsAreRoundAndTheRadioDotIsOnTheGrid(expectations);
    testTreeRowsAreUnnotchedAndUseTheKitCarets(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
