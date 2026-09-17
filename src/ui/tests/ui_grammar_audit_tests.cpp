#include "node_editor_items.hpp"
#include "ui3_audit.hpp"
#include "window_fixture.hpp"
#include <QGraphicsView>
#include <QMenu>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/timeline_ruler.hpp>
#include <bloom/ui/window_status_bar.hpp>
#include <iostream>

namespace {
int run(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomGrammarTests");
    app.setApplicationName("Metrics");
    bloom::ui::kit::installKinetikTheme(app);
    using namespace bloom::ui;
    test::WindowFixture fixture;
    int failures = 0, controls = 0, icons = 0;
    const auto expect = [&](bool condition, const QWidget* widget, const char* message) {
        if (!condition) {
            ++failures;
            std::cerr << widget->metaObject()->className() << ' '
                      << widget->objectName().toStdString() << " height=" << widget->height()
                      << ": " << message << '\n';
        }
    };
    const auto panels = fixture.window->findChildren<EditorArea*>();
    expect(panels.size() == 5, fixture.window.get(), "fixture must exercise all five panels");
    for (auto* panel : panels) {
        auto* header = panel->findChild<QWidget*>("editorHeader");
        expect(header && header->height() == kit::px(kit::Size::HeaderRow), panel, "header token");
        if (auto* footer = panel->findChild<QWidget*>("editorFooter"))
            expect(footer->height() == kit::px(kit::Size::FooterRow), footer, "footer token");
        // A1: the rounded frame overlay exists and is the panel's topmost child widget, so no
        // header, footer or content corner can paint over the curve.
        QWidget* topmost = nullptr;
        for (auto* child : panel->children())
            if (auto* widget = qobject_cast<QWidget*>(child); widget && !widget->isWindow())
                topmost = widget;
        expect(topmost != nullptr && topmost->objectName() == QStringLiteral("panelFrame"), panel,
               "A1 panel frame overlay is the topmost child, clipping chrome at rounded corners");
        const auto inset = kit::px(kit::Spacing::ChromePadding);
        auto* picker = panel->findChild<QWidget*>("editorTypePicker");
        expect(picker->mapTo(header, QPoint()).y() == inset, picker, "A2 chrome vertical inset");
        for (auto* widget : panel->findChildren<QWidget*>()) {
            if (widget->property("chromeControl").toBool() ||
                widget->objectName() == "editorTypePicker" ||
                widget->objectName() == "maximizeAreaButton") {
                expect(widget->height() == kit::px(kit::Size::Control), widget,
                       "chrome child must use Control");
                ++controls;
            }
            if (qobject_cast<kit::KDropdown*>(widget) || qobject_cast<kit::KIconButton*>(widget) ||
                qobject_cast<kit::KMenuButton*>(widget) ||
                qobject_cast<kit::KValueField*>(widget) || qobject_cast<kit::KSwitch*>(widget) ||
                qobject_cast<kit::KSlider*>(widget) || qobject_cast<kit::KSearchField*>(widget) ||
                qobject_cast<kit::KLabel*>(widget) || qobject_cast<kit::KColorChip*>(widget)) {
                expect(widget->height() ==
                           kit::px((qobject_cast<kit::KIconToggle*>(widget) ||
                                    widget->property("rowCell").toString() == "toggle" ||
                                    widget->width() == kit::px(kit::Size::ToggleCell))
                                       ? kit::Size::ToggleCell
                                       : kit::Size::Control),
                       widget, "kit control token");
                ++controls;
            }
            if (const auto* button = qobject_cast<kit::KButton*>(widget)) {
                const auto expected = button->controlSize() == kit::KButton::ControlSize::Roomy
                                          ? kit::Size::ControlRoomy
                                          : kit::Size::Control;
                expect(button->height() == kit::px(expected), button, "button role token");
                ++controls;
            }
            if (qobject_cast<kit::KRow*>(widget)) {
                expect(widget->height() == kit::px(widget->property("headerRow").toBool()
                                                       ? kit::Size::Control
                                                       : kit::Size::ListRow),
                       widget, "list row token");
                if (widget->isVisible()) {
                    for (auto* cell :
                         widget->findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly)) {
                        const auto role = cell->property("rowCell").toString();
                        if (role.isEmpty())
                            continue;
                        expect(widget->rect().contains(cell->geometry()), cell,
                               "row cell must fit inside its row");
                        expect(cell->width() >= kit::px(role == "toggle"
                                                            ? kit::Size::ToggleCell
                                                            : kit::Size::DropdownWidth),
                               cell, "row column pitch");
                    }
                }
            }
            if (qobject_cast<kit::KPropertyRow*>(widget)) {
                expect(widget->layout()->contentsMargins().left() ==
                           kit::px(kit::Spacing::RowPadding),
                       widget, "B5 row owns its padding");
                expect(widget->height() == kit::px(kit::Size::PropertyRow) *
                                               std::max(1, widget->property("rowLines").toInt()),
                       widget, "property row token");
            }
            if (auto* button = qobject_cast<QToolButton*>(widget);
                button && !button->icon().isNull()) {
                const auto pixmap =
                    button->icon().pixmap(button->iconSize(), widget->devicePixelRatioF());
                const auto expected =
                    QSize(qRound(button->iconSize().width() * widget->devicePixelRatioF()),
                          qRound(button->iconSize().height() * widget->devicePixelRatioF()));
                expect(pixmap.size() == expected &&
                           pixmap.devicePixelRatio() == widget->devicePixelRatioF(),
                       widget, "icon must be rasterized at process DPR");
                ++icons;
            }
            if (const auto* dropdown = qobject_cast<kit::KDropdown*>(widget)) {
                for (int index = 0; index < dropdown->count(); ++index) {
                    const auto icon = dropdown->itemIcon(index);
                    if (icon.isNull())
                        continue;
                    const auto box = kit::px(kit::Size::IconChrome);
                    const auto pixmap = icon.pixmap(QSize(box, box), widget->devicePixelRatioF());
                    expect(pixmap.width() == qRound(box * widget->devicePixelRatioF()) &&
                               pixmap.devicePixelRatio() == widget->devicePixelRatioF(),
                           widget, "dropdown icon variant must use exact device pixels");
                    ++icons;
                }
            }
            if (const auto* toggle = qobject_cast<kit::KIconToggle*>(widget)) {
                const auto pixmap = toggle->glyphPixmap();
                expect(pixmap.width() ==
                           qRound(kit::px(kit::Size::IconControl) * widget->devicePixelRatioF()),
                       widget, "toggle integer physical glyph box");
                ++icons;
            }
        }
    }
    auto* viewerMenus = fixture.window->findChild<QWidget*>("viewerHeaderMenuBar");
    auto* overflow = fixture.window->findChild<kit::KMenuButton*>("viewerHeaderOverflowButton");
    const bool collapsed = viewerMenus->property("collapsed").toBool();
    expect(!collapsed || (overflow && overflow->isVisible()), viewerMenus,
           "A3 default viewer menus fit or expose the overflow affordance");
    auto* add = fixture.window->findChild<kit::KMenuButton*>("viewerAddMenuButton");
    bool addInOverflow = false;
    if (add && overflow && overflow->menu()) {
        for (auto* action : overflow->menu()->actions())
            addInOverflow = addInOverflow || action->menu() == add->menu();
    }
    expect(add && (add->isVisible() || addInOverflow), viewerMenus,
           "A3 Add is visible or reachable from the overflow menu");
    for (const auto* name : {"viewerCompositionMenuButton", "viewerFullscreenButton"}) {
        auto* retired = fixture.window->findChild<QWidget*>(name);
        expect(retired && !retired->isVisible(), viewerMenus, "A3 retired controls stay hidden");
    }
    auto* stack = fixture.window->findChild<TimelineLayerStack*>();
    auto* lanes = fixture.window->findChild<TimelineLaneRegion*>("timelineLaneRegion");
    if (!lanes) {
        for (auto* candidate : fixture.window->findChildren<TimelineLaneRegion*>())
            if (candidate->parentWidget()->objectName() == "timelineBody")
                lanes = candidate;
    }
    expect(stack && lanes, fixture.window.get(), "D11 timeline halves exist");
    if (stack && lanes) {
        for (int row = 0; row < stack->rowCount(); ++row)
            expect(stack->mapTo(fixture.window.get(), QPoint(0, stack->rowTop(row))).y() ==
                       lanes->mapTo(fixture.window.get(), QPoint(0, lanes->rowTop(row))).y(),
                   stack, "D11 rows share exact global Y");
        // task TL-FIX2: the divider is now the draggable KSplitHandle (Size::SplitHandle), which
        // replaced the bare TimelineSeparator gap here.
        expect(lanes->x() - stack->geometry().right() - 1 == kit::px(kit::Size::SplitHandle), lanes,
               "D10 lane divider token");
    }
    auto* ruler = fixture.window->findChild<TimelineRuler*>();
    const auto axis = ruler->axisForWidth(ruler->width());
    expect(axis && axis->pixelForSeconds(0) == kit::px(kit::Spacing::LanePadding), ruler,
           "D10 key axis has left breathing room");
    expect(axis && std::abs(ruler->playheadLabelRect().center().x() -
                            axis->pixelForTime(fixture.session.currentTime())) < 0.01,
           ruler, "D12 playhead readout centered on needle");
    for (const auto& label : ruler->majorTickLabelRectsForTest())
        expect(!label.intersects(ruler->playheadLabelRect()), ruler,
               "D12 ruler labels avoid playhead readout");
    auto* navigatorRow = fixture.window->findChild<QWidget*>("timelineNavigatorRow");
    expect(!navigatorRow->isVisible(), navigatorRow, "D17 fitted composition hides navigator row");
    auto* status = fixture.window->statusStrip();
    expect(status && status->isVisible(), fixture.window.get(), "status remains visible");
    for (auto* cell : status->findChildren<kit::KLabel*>()) {
        expect(cell->font() == kit::font(kit::TypeRole::UiSmall), cell, "status UiSmall role");
        expect(cell->height() == kit::px(kit::Size::Control), cell, "status row height");
    }
    expect(!status->colorChipTextForTest().isEmpty(), status, "color state is always explicit");
    auto* tools = fixture.window->findChild<kit::KToolColumn*>("viewerToolColumn");
    expect(tools && tools->isVisible() && tools->width() == kit::px(kit::Size::ToolColumnWidth),
           fixture.window.get(), "tool column token");
    expect(tools->pos() == QPoint(0, 0), tools, "E20 tool strip sticks to canvas left");
    expect(kit::color(kit::Color::OnAccent) == QColor(Qt::white), tools, "E19 white accent ink");
    auto* zoomDropdown = fixture.window->findChild<kit::KDropdown*>("viewerZoomDropdown");
    expect(zoomDropdown->width() >= zoomDropdown->minimumSizeHint().width(), zoomDropdown,
           "E21 zoom fits widest item plus chevron");
    const auto choices = tools->findChildren<kit::KIconToggle*>();
    expect(choices.size() == 10, tools, "ten enabled tool choices");
    for (auto* choice : choices) {
        expect(choice->isEnabled(), choice, "viewer tools enabled");
        expect(choice->height() == kit::px(kit::Size::ToggleCell) && !choice->toolTip().isEmpty(),
               choice, "tool extent and help");
        expect(tools->rect().contains(choice->geometry()), choice, "tool stays inside column");
    }
    auto* propertiesFilter = fixture.window->findChild<kit::KToolColumn*>("propertiesFilterStrip");
    expect(propertiesFilter != nullptr &&
               propertiesFilter->width() == kit::px(kit::Size::ToolColumnWidth),
           fixture.window.get(), "Properties filter strip uses the tool-column token");
    if (propertiesFilter != nullptr) {
        const auto filters = propertiesFilter->findChildren<kit::KIconToggle*>();
        int checked = 0;
        expect(filters.size() == 5, propertiesFilter, "Properties filter has five choices");
        for (auto* filter : filters) {
            checked += filter->isChecked() ? 1 : 0;
            expect(filter->size() ==
                       QSize(kit::px(kit::Size::ToggleCell), kit::px(kit::Size::ToggleCell)),
                   filter, "Properties filter choices use ToggleCell at every DPR");
            expect(!filter->toolTip().isEmpty() &&
                       propertiesFilter->rect().contains(filter->geometry()),
                   filter, "Properties filter choices stay inside the strip with help text");
        }
        expect(checked == 1, propertiesFilter, "Properties filter choices are exclusive");
    }
    int cards = 0, fields = 0, alignedSockets = 0;
    for (auto* view : fixture.window->findChildren<QGraphicsView*>()) {
        if (!view->scene())
            continue;
        for (auto* item : view->scene()->items()) {
            auto* card = dynamic_cast<node_editor::NodeItem*>(item);
            if (!card || card->data(kNodeItemKindRole).toString() != "node")
                continue;
            ++cards;
            const auto [nameRect, categoryRect] = card->titleBandRects();
            expect(nameRect.center().y() == categoryRect.center().y() &&
                       !nameRect.intersects(categoryRect),
                   view, "F26 title and category share one row without overlap");
            expect(card->cardWidth() >= kit::px(kit::Size::NodeCardWidth), view,
                   "node card width token floor");
            expect(card->parameterRowHeight() == kit::px(kit::Size::PropertyRow), view,
                   "node row pitch");
            for (auto* child : card->childItems()) {
                auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(child);
                if (!proxy || !proxy->widget() ||
                    !proxy->widget()->property("nodeParameterRowPitch").isValid())
                    continue;
                auto* field = proxy->widget();
                ++fields;
                expect(qobject_cast<kit::KPropertyRow*>(field) &&
                           field->height() == kit::px(kit::Size::PropertyRow),
                       field, "F28 node parameter is a shared property row");
                for (auto* diamond : field->findChildren<KeyframeDiamond*>()) {
                    for (auto* value :
                         field->findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly)) {
                        if (value == diamond || !value->isVisible() || value->inherits("QLabel"))
                            continue;
                        expect(!diamond->geometry().intersects(value->geometry()), diamond,
                               "F28 diamond column never overlaps a field or swatch");
                    }
                }
                for (auto* value : field->findChildren<kit::KValueField*>()) {
                    const auto text = value->displayedValue();
                    const auto point = text.indexOf('.');
                    expect(point < 0 || (text.size() - point <= 3 && !text.endsWith('0')), value,
                           "F28 resting values have at most two decimals and no trailing zeros");
                }
                expect(card->cardRect().contains(proxy->mapRectToParent(proxy->boundingRect())),
                       field, "field contained in node");
            }
            for (auto* socket : card->sockets()) {
                expect(socket->pos().x() == 0 || socket->pos().x() == card->cardWidth(), view,
                       "socket on card edge");
                bool hasRole = false, aligned = false;
                for (auto* child : card->childItems()) {
                    auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(child);
                    if (!proxy || !proxy->widget() ||
                        !proxy->widget()->property("nodeParameterRowPitch").isValid() ||
                        proxy->widget()->property("nodeParameterRole").toString() != socket->name)
                        continue;
                    hasRole = true;
                    aligned =
                        aligned || std::abs(proxy->pos().y() + proxy->widget()->height() / 2.0 -
                                            socket->pos().y()) < 0.01;
                }
                if (hasRole) {
                    expect(aligned, view, "socket aligned to its kit parameter row");
                    ++alignedSockets;
                }
            }
        }
    }
    expect(cards >= 4 && fields >= 5 && alignedSockets >= 5, fixture.window.get(),
           "audit real node cards and socket/control pairs");
    for (int width : {1600, 1920}) {
        fixture.window->resize(width, 1200);
        QTest::qWait(20);
        auto* timeline = fixture.window->findChild<QWidget*>("timelineHeaderMenus");
        expect(!timeline->property("collapsed").toBool(), timeline,
               "timeline menus fit at >=1600px");
        expect(status->isVisible(), status, "status survives window resizing");
    }
    QStringList groups;
    for (const auto& entry : stack->entries())
        if (entry.rowKind == TimelineLayerEntry::Kind::Group)
            groups.append(entry.name);
    expect(groups == QStringList{"Object", "Transform", "Source"}, stack,
           "D16 groups match Properties order");
    const auto beforeCollapse = stack->rowCount();
    auto* disclosure = stack->findChild<QToolButton*>("timelinePropertyDisclosure");
    expect(disclosure && disclosure->isVisible(), stack, "D16 group disclosure is visible");
    if (disclosure) {
        QTest::mouseClick(disclosure, Qt::LeftButton);
        expect(stack->rowCount() < beforeCollapse, stack,
               "D16 group click collapses its properties");
        QTest::mouseClick(disclosure, Qt::LeftButton);
        expect(stack->rowCount() == beforeCollapse, stack,
               "D16 group click restores its properties");
    }
    std::cout << "Node audit: " << cards << " cards, " << fields << " fields, " << alignedSockets
              << " aligned sockets\n";
    expect(controls > 30 && icons > 10, fixture.window.get(),
           "audit must inspect real controls and glyphs");
    std::cout << "Audited " << panels.size() << " panels, " << controls << " controls, " << icons
              << " icons at DPR " << fixture.window->devicePixelRatioF() << '\n';
    test::auditUi3(fixture, expect);
    return failures == 0 ? 0 : 1;
}

} // namespace
int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
