#pragma once
#include "node_editor_items.hpp"
#include "window_fixture.hpp"
#include <QGraphicsScene>
#include <QGridLayout>
#include <QImage>
#include <QSignalSpy>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/search_popup.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/surfaces.hpp>
#include <bloom/ui/timeline_editor.hpp>

namespace bloom::ui::test {
template <typename Expect> void auditUi3(WindowFixture& fixture, const Expect& expect) {
    using namespace kit;
    auto* window = fixture.window.get();
    // A4: geometry, including the selector-to-menu boundary, owns spacing.
    for (auto* panel : window->findChildren<EditorArea*>()) {
        auto* header = panel->findChild<QWidget*>("editorHeader");
        auto* maximize = panel->findChild<QWidget*>("maximizeAreaButton");
        expect(maximize->mapTo(header, QPoint()).x() + maximize->width() >=
                   header->width() - px(Spacing::ChromePadding) - px(Size::Hairline),
               maximize, "A/D18 maximize occupies the panel's right edge");
        if (auto* footer = panel->findChild<QWidget*>("editorFooter")) {
            const auto image = panel->grab().toImage();
            const auto ratio = panel->devicePixelRatioF();
            expect(image.width() == qRound(panel->width() * ratio), footer,
                   "A1 panel capture uses physical device extent");
        }
    }
    auto* menus = window->findChild<QWidget*>("viewerHeaderMenuBar");
    QList<QWidget*> visible;
    for (auto* child : menus->findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly))
        if (child->isVisible())
            visible.append(child);
    std::ranges::sort(visible, {}, [](QWidget* widget) { return widget->x(); });
    for (qsizetype i = 1; i < visible.size(); ++i)
        expect(visible[i]->x() - visible[i - 1]->geometry().right() - 1 == px(Spacing::ChromeGap),
               menus, "A4 one gap between header items");

    KValueField value;
    value.resize(120, px(Size::Control));
    expect(value.cellRect().top() == px(Spacing::FieldMargin) &&
               value.cellRect().left() == px(Spacing::FieldMargin),
           &value, "B5 primitive field margin");
    for (auto* section : window->findChildren<KSection*>()) {
        expect(section->bodyLayout()->contentsMargins() ==
                   QMargins(px(Spacing::SectionPadding), px(Spacing::SectionPadding),
                            px(Spacing::SectionPadding), px(Spacing::SectionPadding)),
               section, "B6 uniform section padding");
    }
    auto* properties = window->findChild<QWidget*>("propertiesEditor");
    if (properties) {
        for (auto* row : properties->findChildren<KPropertyRow*>()) {
            if (!row->isVisible())
                continue;
            auto* label = row->findChild<QLabel*>("propertiesRowLabel");
            if (!label)
                continue;
            auto* first = row->layout()->itemAt(2)->widget();
            if (first)
                expect(first->x() - label->geometry().right() - 1 >= px(Spacing::PropertyGutter),
                       row, "B6 label/control gutter survives narrow panels");
        }
    }
    auto* stack = window->findChild<TimelineLayerStack*>();
    for (auto* row : stack->findChildren<KRow*>()) {
        if (!row->isVisible())
            continue;
        auto* disclosure = row->disclosureButton();
        expect(disclosure->size() == QSize(px(Size::ToggleCell), px(Size::ToggleCell)), row,
               "C8 disclosure uses toggle cell");
        for (auto* toggle : row->findChildren<KIconToggle*>())
            expect(toggle->size() == QSize(px(Size::ToggleCell), px(Size::ToggleCell)), toggle,
                   "C7 identical square toggle cells");
    }
    // C9/F28: actual device-resolution vector coverage at two canvas zooms for every DPR entry.
    for (double zoom : {0.75, 1.5}) {
        const double dpr = window->devicePixelRatioF();
        QGraphicsScene scene;
        auto* diamond = new KDiamond;
        diamond->setAttribute(Qt::WA_TranslucentBackground);
        diamond->setFixedSize(px(Size::ToggleCell), px(Size::ToggleCell));
        diamond->setIndicator(true, KDiamond::Fill::Half);
        scene.addWidget(diamond)->setPos(24, 24);
        QImage raster(qRound(96 * dpr), qRound(96 * dpr), QImage::Format_ARGB32_Premultiplied);
        raster.setDevicePixelRatio(dpr);
        raster.fill(Qt::transparent);
        QPainter painter(&raster);
        scene.render(&painter, QRectF(0, 0, 96, 96), QRectF(0, 0, 96 / zoom, 96 / zoom));
        painter.end();
        QRect ink;
        int opaque = 0;
        for (int y = 0; y < raster.height(); ++y)
            for (int x = 0; x < raster.width(); ++x) {
                const auto c = raster.pixelColor(x, y);
                if (c.alpha() > 160 && c.red() > 150 && c.green() > 100 && c.blue() < 120) {
                    ink |= QRect(x, y, 1, 1);
                    ++opaque;
                }
            }
        const auto radius = std::round(kKeyDiamondRadius * dpr * zoom);
        expect(opaque > 4 && std::abs(ink.width() - 2 * radius) <= 6, window,
               "C9/F28 vector diamond has crisp coverage at DPR and canvas zoom");
        diamond->setIndicator(false, KDiamond::Fill::None);
        raster.fill(Qt::transparent);
        QPainter emptyPainter(&raster);
        scene.render(&emptyPainter, QRectF(0, 0, 96, 96), QRectF(0, 0, 96 / zoom, 96 / zoom));
        emptyPainter.end();
        const QPoint center(qRound(36 * dpr * zoom), qRound(36 * dpr * zoom));
        int outline = 0;
        for (int y = 0; y < raster.height(); ++y)
            for (int x = 0; x < raster.width(); ++x)
                outline += raster.pixelColor(x, y).alpha() > 40;
        const auto stroke = std::max(1.0, std::round(kDiamondStroke * dpr * zoom));
        expect(raster.pixelColor(center).alpha() == 0 && outline >= 4 * radius * stroke, window,
               "C9 empty diamond keeps its open center and icon-weight vector stroke");
    }
    KIconButton accent;
    accent.setIcon(icon(IconId::Pause, IconRole::Chrome));
    accent.setCheckable(true);
    accent.setChecked(true);
    const auto ink = accent.grab().toImage();
    int white = 0, blue = 0;
    for (int y = 0; y < ink.height(); ++y)
        for (int x = 0; x < ink.width(); ++x) {
            const auto c = ink.pixelColor(x, y);
            white += c.red() > 220 && c.green() > 220 && c.blue() > 220;
            blue += c.blue() > c.red() + 80;
        }
    expect(white > 4 && blue > 20, &accent, "E19 accent button paints white glyph over blue");
    KRow plain;
    plain.resize(200, px(Size::ListRow));
    plain.setRowState(0, false);
    const auto even = plain.grab().toImage();
    plain.setRowState(1, false);
    expect(even == plain.grab().toImage(), &plain, "D15 no alternating row stripes");
    plain.setRowState(0, true);
    const auto selected = plain.grab().toImage();
    expect(selected.pixelColor(0, 0) == color(Color::SurfaceRaised), &plain,
           "D14 selected row is lighter with no side stripe");

    const auto category = [](std::string_view type) {
        const auto* definition = document::builtInNodeDefinitions().find(type, 1);
        return definition ? node_editor::nodeCategoryName(*definition) : QString{};
    };
    for (const auto& [type, expected] :
         std::array{std::pair{document::kScalarToStringNodeType, "Convert"},
                    std::pair{document::kStringLengthNodeType, "String"},
                    std::pair{document::kCompareNodeType, "Logic"},
                    std::pair{document::kFrameNumberNodeType, "Time"},
                    std::pair{document::kHueShiftNodeType, "Color"},
                    std::pair{document::kVector2MathNodeType, "Vector"},
                    std::pair{document::kCompositionSizeNodeType, "Values"}})
        expect(category(type) == expected, window, "F24 normalized node category");
    for (const auto& definition : document::builtInNodeDefinitions().definitions())
        if (node_editor::nodeCategoryName(definition) == "Utilities")
            expect(definition.key.typeId.starts_with("bloom.separate-") ||
                       definition.key.typeId.starts_with("bloom.combine-") ||
                       document::isRerouteNodeType(definition.key.typeId),
                   window, "F24 Utilities contains only plumbing");
    if (auto* expand = window->findChild<KButton*>("propertiesSolidColorExpand")) {
        expand->setChecked(true);
        QApplication::processEvents();
        auto* chip = window->findChild<QWidget*>("propertiesSolidColorChip");
        auto* components = window->findChild<QWidget*>("solidColorFieldGroup");
        expect(chip->mapTo(window, QPoint()).x() == components->mapTo(window, QPoint()).x(), chip,
               "B6 expanded RGBA starts beneath the swatch control column");
        expand->setChecked(false);
    }
    for (auto* view : window->findChildren<QGraphicsView*>()) {
        auto* scene = qobject_cast<NodeGraphicsScene*>(view->scene());
        if (!scene)
            continue;
        scene->clearSelection();
        for (auto* item : scene->items()) {
            auto* edge = dynamic_cast<node_editor::NodeEdgeItem*>(item);
            if (!edge)
                continue;
            const auto bounds = edge->boundingRect();
            if (bounds.width() > 2000 || bounds.height() > 2000)
                continue;
            const auto capture = [&](bool active) {
                QImage image(QSize(qCeil(bounds.width()), qCeil(bounds.height())),
                             QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::transparent);
                QPainter painter(&image);
                painter.translate(-bounds.topLeft());
                edge->emphasize(active);
                edge->paint(&painter, nullptr, nullptr);
                return image;
            };
            const auto idle = capture(false), active = capture(true);
            bool sameCoverage = true, brighter = false;
            for (int y = 0; y < idle.height(); ++y)
                for (int x = 0; x < idle.width(); ++x) {
                    const auto a = idle.pixelColor(x, y), b = active.pixelColor(x, y);
                    sameCoverage = sameCoverage && a.alpha() == b.alpha();
                    brighter = brighter || (b.alpha() > 100 && b.value() > a.value());
                }
            expect(sameCoverage && brighter, view,
                   "F27 active links keep identical stroke coverage and a lighter tint");
            edge->emphasize(false);
            break;
        }
        QSignalSpy search(scene, &NodeGraphicsScene::addSearchRequested);
        QGraphicsSceneMouseEvent event(QEvent::GraphicsSceneMouseDoubleClick);
        event.setScenePos(QPointF(-1000, -1000));
        event.setScreenPos(window->mapToGlobal(QPoint(200, 200)));
        event.setButton(Qt::LeftButton);
        QApplication::sendEvent(scene, &event);
        expect(search.count() == 1, view, "F25 empty canvas double-click opens search");
        for (auto* popup : window->findChildren<KSearchPopup*>())
            popup->close();
    }
    fixture.session.clearSelection();
    auto* count = window->findChild<QWidget*>("nodeSelectionReadout");
    expect(count && !count->isVisible(), window, "F22 zero selection hides count");
    for (const char* name : {"nodeSnapSwitch", "nodeLinkStyleDropdown"})
        expect(!window->findChild<QWidget*>(name)->isVisible(), window,
               "F22 footer controls live in View");

    std::unique_ptr<QMenu> menu(makeMenu(window));
    menu->setProperty("columnFlow", true);
    for (int i = 0; i < 40; ++i)
        menu->addAction(QString::number(i));
    menu->popup(window->mapToGlobal(QPoint(1900, 1100)));
    QApplication::processEvents();
    const QRect bounds(window->mapToGlobal(QPoint()), window->size());
    expect(menu->height() <= window->height() * kMenuWindowHeightShare &&
               bounds.contains(menu->geometry()) && menu->property("flowColumns").toInt() >= 2,
           menu.get(), "F23 menu columns fit inside half-height window boundary");
    menu->close();
}
} // namespace bloom::ui::test
