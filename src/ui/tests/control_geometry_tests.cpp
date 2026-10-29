// One control look and one control height (ui-grammar.md, "One control look, one control height").
//
// The main-window fixture is walked panel by panel (Viewer, Timeline, Nodes, Assets, Properties)
// and card row by card row: every kit control is one Control tall (a dense ToggleCell cell is
// already its body), paints a ToggleCell-tall body, and shares its vertical centre with every other
// control whose body overlaps it -- the same row. The pixels then prove the painting follows the
// geometry: just inside each body's left edge the grab shows the recipe's resting plate. A second
// pass builds each control kind on its own and holds that its resting, hovered and checked colours
// come from the one recipe (kit::buttonColors Secondary, or kit::fieldColors for text entry).
#include "node_editor_items.hpp"
#include "window_fixture.hpp"

#include <QEnterEvent>
#include <QGraphicsProxyWidget>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <cmath>
#include <iostream>
#include <map>
#include <vector>

namespace {
using namespace bloom::ui;

enum class Recipe { Button, Field, None };

struct Control {
    QWidget* widget = nullptr;
    QRectF body; // In the root's coordinates.
    Recipe recipe = Recipe::None;
    bool cell = false;
};

// A dense strip's fixed-pitch cell: a timeline layer switch, a tool choice, the tool column's More.
bool isCell(const QWidget& widget) {
    return widget.property("layerSwitch").toBool() || widget.property("toolChoice").toBool() ||
           qobject_cast<const kit::KToolColumn*>(widget.parentWidget()) != nullptr;
}

// The kit controls this rule governs, and which recipe each paints. A plate-less glyph (a
// disclosure caret, a column heading) and a menu title have no body to line up.
std::optional<Recipe> recipeOf(QWidget& widget) {
    if (qobject_cast<kit::KValueField*>(&widget) || widget.objectName() == "viewerTimeReadout" ||
        (qobject_cast<QLineEdit*>(&widget) && widget.property("kitControl").toBool()))
        return Recipe::Field;
    // The tool column keeps its plate-less choices (a recorded exception): measured, not sampled.
    if (widget.property("toolChoice").toBool())
        return Recipe::None;
    if (auto* button = qobject_cast<kit::KIconButton*>(&widget)) {
        const bool heading =
            button->parentWidget() && button->parentWidget()->property("headerRow").toBool();
        if (heading || button->property("disclosure").toBool())
            return std::nullopt;
        return Recipe::Button;
    }
    if (auto* button = qobject_cast<kit::KButton*>(&widget))
        return button->buttonKind() == kit::ButtonKind::Secondary ? Recipe::Button : Recipe::None;
    if (qobject_cast<kit::KDropdown*>(&widget))
        return Recipe::Button;
    if (auto* chip = qobject_cast<kit::KColorChip*>(&widget))
        return chip->shape() == kit::KColorChip::Shape::Square ? std::optional(Recipe::Button)
                                                               : std::nullopt;
    if (auto* group = qobject_cast<kit::KRadioGroup*>(&widget))
        return group->presentation() == kit::KRadioGroup::Presentation::Segmented
                   ? std::optional(Recipe::Button)
                   : std::nullopt;
    return std::nullopt;
}

bool shownWithin(const QWidget& widget, const QWidget& root) {
    return &widget == &root || (widget.isVisibleTo(&root) && !widget.isWindow());
}

std::vector<Control> controlsIn(QWidget& root, const bool onScreenOnly = true) {
    std::vector<Control> found;
    for (auto* widget : root.findChildren<QWidget*>()) {
        if (!shownWithin(*widget, root) || widget->window() != root.window())
            continue;
        // A row scrolled out of its view is not in any row on screen.
        if (onScreenOnly && widget->visibleRegion().isEmpty())
            continue;
        const auto recipe = recipeOf(*widget);
        if (!recipe)
            continue;
        // A labelled value field paints its body beside the label column: the same rows, a
        // narrower box.
        const auto* value = qobject_cast<kit::KValueField*>(widget);
        const QRectF body = value != nullptr ? value->cellRect() : kit::controlBody(*widget);
        found.push_back({widget, body.translated(widget->mapTo(&root, QPoint())), *recipe,
                         isCell(*widget)});
    }
    return found;
}

QString nameOf(const QWidget& widget) {
    return QStringLiteral("%1 %2").arg(QString::fromLatin1(widget.metaObject()->className()),
                                       widget.objectName());
}

// The resting plate a Button-recipe control shows just inside its left edge.
QColor restingPlate(const QWidget& widget) {
    if (const auto* button = qobject_cast<const QAbstractButton*>(&widget);
        button && button->isChecked() && !widget.property("layerSwitch").toBool())
        return kit::color(kit::Color::Accent);
    if (const auto* group = qobject_cast<const kit::KRadioGroup*>(&widget);
        group && group->currentIndex() == 0)
        return kit::color(kit::Color::Accent);
    return kit::buttonColors(kit::ButtonKind::Secondary, kit::State::Normal, false).fill;
}

bool near(const QColor& a, const QColor& b, const int tolerance = 2) {
    return std::abs(a.red() - b.red()) <= tolerance && std::abs(a.green() - b.green()) <= tolerance &&
           std::abs(a.blue() - b.blue()) <= tolerance;
}

int run(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomControlGeometryTests");
    app.setApplicationName("Geometry");
    kit::installKinetikTheme(app);
    test::WindowFixture fixture;
    int failures = 0;
    const auto fail = [&](const QString& message) {
        ++failures;
        std::cerr << message.toStdString() << '\n';
    };

    const int controlHeight = kit::px(kit::Size::Control);
    const qreal bodyHeight = kit::px(kit::Size::ToggleCell);
    const auto checkRoot = [&](QWidget& root, const QString& label, const QImage* image,
                               std::map<QString, int>& counts) {
        const auto controls = controlsIn(root, image != nullptr);
        for (const auto& control : controls) {
            const auto& widget = *control.widget;
            const int expected = control.cell ? kit::px(kit::Size::ToggleCell) : controlHeight;
            if (widget.height() != expected)
                fail(label + ": " + nameOf(widget) +
                     QStringLiteral(" is %1 tall, not %2").arg(widget.height()).arg(expected));
            if (std::abs(control.body.height() - bodyHeight) > 0.01)
                fail(label + ": " + nameOf(widget) +
                     QStringLiteral(" paints a %1-tall body").arg(control.body.height()));
            if ((qobject_cast<const kit::KIconButton*>(&widget) != nullptr) &&
                widget.width() != widget.height())
                fail(label + ": " + nameOf(widget) + " is not square");
            ++counts[QString::fromLatin1(widget.metaObject()->className())];
            if (image == nullptr || control.recipe == Recipe::None)
                continue;
            // Just inside the body's left hairline, at its vertical centre: the plate itself.
            const qreal ratio = image->devicePixelRatio();
            const qreal hairline = kit::snappedHairlineWidth(ratio);
            const QPointF sample(control.body.left() + hairline + 0.5 / ratio,
                                 control.body.center().y());
            const QPoint local = (sample - QPointF(widget.mapTo(&root, QPoint()))).toPoint();
            if (!widget.visibleRegion().contains(local))
                continue;
            const QColor seen = image->pixelColor(static_cast<int>(std::floor(sample.x() * ratio)),
                                                  static_cast<int>(std::floor(sample.y() * ratio)));
            const QColor wanted =
                control.recipe == Recipe::Field
                    ? kit::fieldColors(true, false, false, true).fill
                    : restingPlate(widget);
            if (!near(seen, wanted))
                fail(label + ": " + nameOf(widget) +
                     QStringLiteral(" shows %1 inside its body, not the recipe's %2")
                         .arg(seen.name(), wanted.name()));
        }
        // Bodies that overlap vertically are one row, and one row has one centre.
        for (std::size_t a = 0; a < controls.size(); ++a)
            for (std::size_t b = a + 1; b < controls.size(); ++b) {
                const auto& first = controls[a].body;
                const auto& second = controls[b].body;
                const bool overlap = first.top() < second.bottom() && second.top() < first.bottom();
                if (overlap && std::abs(first.center().y() - second.center().y()) > 0.01)
                    fail(label + ": " + nameOf(*controls[a].widget) + " and " +
                         nameOf(*controls[b].widget) +
                         QStringLiteral(" share a row but not a centre (%1 vs %2)")
                             .arg(first.center().y())
                             .arg(second.center().y()));
            }
    };

    fixture.clearInteraction();
    const QImage image = fixture.window->grab().toImage();
    std::map<QString, int> counts;
    const auto panels = fixture.window->findChildren<EditorArea*>();
    if (panels.size() != 5)
        fail(QStringLiteral("the fixture shows %1 panels, not five").arg(panels.size()));
    for (auto* panel : panels) {
        // Each panel is checked in window coordinates so the grab can be sampled, but only against
        // its own controls: two panels side by side are not one row.
        const auto label = QString::fromStdString(panel->editorId());
        QWidget& root = *panel;
        QImage panelImage = image.copy(QRect(
            (QPointF(panel->mapTo(fixture.window.get(), QPoint())) * image.devicePixelRatio())
                .toPoint(),
            panel->size() * image.devicePixelRatio()));
        panelImage.setDevicePixelRatio(image.devicePixelRatio());
        checkRoot(root, label, &panelImage, counts);
        auto* header = panel->findChild<QWidget*>("editorHeader");
        if (header == nullptr || controlsIn(*header).empty())
            fail(label + ": the header has no controls to measure");
    }
    // Node cards: every row is its own proxied widget, drawn at the canvas zoom.
    int cardRows = 0;
    for (auto* view : fixture.window->findChildren<QGraphicsView*>())
        if (view->scene() != nullptr)
            for (auto* item : view->scene()->items())
                if (auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(item);
                    proxy != nullptr && proxy->widget() != nullptr && proxy->isVisible()) {
                    checkRoot(*proxy->widget(), QStringLiteral("node card"), nullptr, counts);
                    ++cardRows;
                }
    for (const auto* kind : {"bloom::ui::kit::KIconButton", "bloom::ui::kit::KDropdown",
                             "bloom::ui::kit::KValueField", "bloom::ui::kit::KButton",
                             "bloom::ui::kit::KIconToggle", "bloom::ui::kit::KColorChip"})
        if (counts[QString::fromLatin1(kind)] == 0)
            fail(QStringLiteral("the walk measured no %1").arg(QString::fromLatin1(kind)));
    if (cardRows < 5)
        fail(QStringLiteral("only %1 node card rows were measured").arg(cardRows));

    // The footer's frame field sits one ChromeGap after the loop toggle, exactly the gap between
    // two transport buttons.
    auto* loop = fixture.window->findChild<QWidget*>("timelineLoopIndicator");
    auto* forward = fixture.window->findChild<QWidget*>("viewerStepToEndButton");
    auto* field = fixture.window->findChild<QWidget*>("viewerTimeReadout");
    if (loop && forward && field) {
        const int transportGap = loop->geometry().left() - forward->geometry().right();
        const int fieldGap = field->geometry().left() - loop->geometry().right();
        if (transportGap != fieldGap)
            fail(QStringLiteral("the frame field is %1 px from loop; transport buttons are %2 apart")
                     .arg(fieldGap)
                     .arg(transportGap));
    } else {
        fail(QStringLiteral("the transport and frame field are reachable by name"));
    }

    // ---- Each kind on its own: the one recipe ----------------------------------------------------
    QWidget host;
    auto* layout = new QHBoxLayout(&host);
    auto* button = new kit::KButton(QStringLiteral("Button"), &host);
    auto* iconButton = new kit::KIconButton(&host);
    iconButton->setIcon(kit::icon(kit::IconId::Play, kit::IconRole::Control));
    iconButton->setIconSize(QSize(kit::px(kit::Size::IconLarge), kit::px(kit::Size::IconLarge)));
    auto* toggle = new kit::KIconToggle(kit::IconId::Link, &host);
    auto* dropdown = new kit::KDropdown(&host);
    dropdown->addItem(QStringLiteral("Normal"));
    auto* chip = new kit::KColorChip(&host);
    chip->setFixedWidth(kit::px(kit::Size::DropdownWidth));
    auto* segmented = new kit::KRadioGroup(&host);
    segmented->setPresentation(kit::KRadioGroup::Presentation::Segmented);
    segmented->addOption(QStringLiteral("One"));
    segmented->addOption(QStringLiteral("Two"));
    segmented->setCurrentIndex(1);
    auto* valueField = new kit::KValueField(&host);
    auto* lineEdit = new kit::KLineEdit(QStringLiteral("Name"), &host);
    for (QWidget* widget : std::initializer_list<QWidget*>{
             button, iconButton, toggle, dropdown, chip, segmented, valueField, lineEdit})
        layout->addWidget(widget);
    host.show();
    QApplication::processEvents();
    const auto plateOf = [](QWidget& widget) {
        const QImage grab = widget.grab().toImage();
        const qreal ratio = grab.devicePixelRatio();
        const auto* value = qobject_cast<kit::KValueField*>(&widget);
        const QRectF body = value != nullptr ? value->cellRect() : kit::controlBody(widget);
        const qreal x = body.left() + kit::snappedHairlineWidth(ratio) + 0.5 / ratio;
        return grab.pixelColor(static_cast<int>(std::floor(x * ratio)),
                               static_cast<int>(std::floor(body.center().y() * ratio)));
    };
    const auto brightestInk = [](QWidget& widget) {
        const QImage grab = widget.grab().toImage();
        int brightest = 0;
        for (int y = 0; y < grab.height(); ++y)
            for (int x = 0; x < grab.width(); ++x)
                brightest = std::max(brightest, qGray(grab.pixel(x, y)));
        return brightest;
    };
    const auto secondary = [](const kit::State state) {
        return kit::buttonColors(kit::ButtonKind::Secondary, state, false);
    };
    for (QWidget* widget : std::initializer_list<QWidget*>{button, iconButton, toggle, dropdown,
                                                           chip, segmented}) {
        if (!near(plateOf(*widget), secondary(kit::State::Normal).fill))
            fail(nameOf(*widget) + " does not rest on the Secondary plate: " +
                 plateOf(*widget).name());
        if (widget->height() != controlHeight)
            fail(nameOf(*widget) + QStringLiteral(" is %1 tall").arg(widget->height()));
    }
    for (QWidget* widget : std::initializer_list<QWidget*>{valueField, lineEdit})
        if (!near(plateOf(*widget), kit::fieldColors(true, false, false, false).fill))
            fail(nameOf(*widget) + " does not rest in the Field well: " + plateOf(*widget).name());
    // Foreground ink, glyphs too -- never the Muted 0x99 an icon used to rest at.
    for (QWidget* widget : std::initializer_list<QWidget*>{button, iconButton, toggle, dropdown})
        if (brightestInk(*widget) < 240)
            fail(nameOf(*widget) + QStringLiteral(" inks at most %1, not Foreground")
                                       .arg(brightestInk(*widget)));
    // Hover is one rung up for every button kind.
    for (QWidget* widget : std::initializer_list<QWidget*>{button, iconButton, toggle, dropdown}) {
        const QPointF centre(widget->width() / 2.0, widget->height() / 2.0);
        QEnterEvent enter(centre, centre, widget->mapToGlobal(centre));
        QApplication::sendEvent(widget, &enter);
        if (!near(plateOf(*widget), secondary(kit::State::Hover).fill))
            fail(nameOf(*widget) + " does not hover one rung up: " + plateOf(*widget).name());
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(widget, &leave);
    }
    // Checked is the Accent plate on every kind.
    toggle->setChecked(true);
    iconButton->setCheckable(true);
    iconButton->setChecked(true);
    button->setCheckable(true);
    button->setChecked(true);
    for (QWidget* widget : std::initializer_list<QWidget*>{button, iconButton, toggle})
        if (!near(plateOf(*widget), secondary(kit::State::Selected).fill))
            fail(nameOf(*widget) + " checked is not the Accent plate: " + plateOf(*widget).name());

    int measured = 0;
    for (const auto& [kind, count] : counts)
        measured += count;
    std::cout << "Measured " << measured << " controls in " << panels.size() << " panels and "
              << cardRows << " node card rows at DPR " << fixture.window->devicePixelRatioF()
              << '\n';
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
