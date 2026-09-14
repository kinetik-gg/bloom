#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QScrollArea>
#include <QSettings>
#include <QThread>
#include <bloom/commands/command_stack.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <cmath>
#include <iostream>

using namespace bloom;
namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}
void settle() {
    for (int i = 0; i < 5; ++i)
        QCoreApplication::processEvents();
}
void idle(QWidget& widget) {
    widget.clearFocus();
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(&widget, &leave);
    settle();
}
QRect gridPointRect(const int index) {
    // Independent design metric: an 8 px dot on a 12 px pitch, inset by 2 px.
    return {index % 3 * 12 + 2, index / 3 * 12 + 2, 8, 8};
}
void waitForBounds(QWidget& grid) {
    QElapsedTimer timer;
    timer.start();
    while (!grid.isEnabled() && timer.elapsed() < 10000) {
        settle();
        QThread::msleep(5);
    }
    expect(grid.isEnabled(), "anchor bounds resolve asynchronously");
}
void click(QWidget& widget, QPoint point) {
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), QPointF(widget.mapToGlobal(point)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point),
                        QPointF(widget.mapToGlobal(point)), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &press);
    QCoreApplication::sendEvent(&widget, &release);
    settle();
}
QWidget* row(QWidget* field) {
    auto* parent = field->parentWidget();
    while (parent && parent->objectName() != "propertiesRow")
        parent = parent->parentWidget();
    return parent;
}
int xIn(QWidget* child, QWidget* panel) { return child->mapTo(panel, QPoint{}).x(); }
void run() {
    auto project =
        document::makeNewProject("Properties", "Composition", core::RationalTime::fromInteger(10));
    const auto compositionId = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, compositionId);
    expect(session.addSolidLayer("Solid", {0.65, 0.06, 0.03, 1}), "create Solid fixture");
    ui::EditorRegistry registry;
    (void)registry.registerEditor({"bloom.properties", "Properties", [&session](QWidget* parent) {
                                       return new ui::PropertiesEditor(session, parent);
                                   }});
    ui::EditorArea area(registry, "bloom.properties");
    area.resize(360, 800);
    area.show();
    settle();
    auto* panel = area.findChild<ui::PropertiesEditor*>();
    auto* grid = area.findChild<QWidget*>("propertiesAnchorGrid");
    expect(panel && grid, "panel and anchor grid exist");
    if (!panel || !grid)
        return;
    waitForBounds(*grid);
    auto* search = area.findChild<QLineEdit*>("propertiesSearchField");
    auto* header = area.findChild<QWidget*>("editorHeader");
    expect(search && header->isAncestorOf(search), "search is hosted in the panel header");
    expect(search && search->width() <= ui::kit::px(ui::kit::Size::PropertiesSearchWidth),
           "search width is compact");
    auto* positionX = panel->findChild<ui::kit::KValueField*>("positionXEditor");
    auto* positionY = panel->findChild<ui::kit::KValueField*>("positionYEditor");
    auto* positionLink = panel->findChild<ui::kit::KButton*>("positionLinkToggle");
    auto* opacity = panel->findChild<ui::kit::KValueField*>("opacityEditor");
    auto* visible = panel->findChild<ui::kit::KCheckBox*>("layerVisibleSwitch");
    auto* solo = panel->findChild<ui::kit::KCheckBox*>("layerSoloSwitch");
    expect(positionX && positionY && positionLink && opacity && visible && solo,
           "compact controls retain their identities");
    if (!positionX || !positionY || !positionLink || !opacity || !visible || !solo)
        return;
    auto* xRow = row(positionX);
    auto* opacityRow = row(opacity);
    auto* positionDiamond = xRow->findChild<ui::KeyframeDiamond*>();
    auto* opacityDiamond = opacityRow->findChild<ui::KeyframeDiamond*>();
    expect(xIn(positionX, panel) == xIn(opacityRow->findChild<ui::kit::KSlider*>(), panel),
           "control columns start at the same x");
    expect(xIn(positionDiamond, panel) == xIn(opacityDiamond, panel), "keyframe columns align");
    expect(xIn(positionDiamond, panel) > xIn(positionY, panel), "diamond follows controls");
    expect(xIn(positionX, panel) < xIn(positionLink, panel) &&
               xIn(positionLink, panel) < xIn(positionY, panel),
           "link is between X and Y fields");
    expect(positionX->cellRect().left() == 0 && positionX->labelRect().left() > 0,
           "axis prefix is inside the field");
    expect(positionX->width() >= 64 && positionX->width() <= 72 && opacity->width() >= 64 &&
               opacity->width() <= 72,
           "numeric widths are 64–72 px");
    const auto y = visible->mapTo(panel, QPoint{}).y();
    expect(solo->mapTo(panel, QPoint{}).y() - y == ui::kit::px(ui::kit::Size::PropertiesRowPitch),
           "checkbox row pitch is 28 px");
    expect(visible->isChecked() && !solo->isChecked() && visible->width() == visible->height(),
           "object flags are square checkboxes");
    idle(*visible);
    idle(*solo);
    const auto on = visible->grab().toImage();
    const auto off = solo->grab().toImage();
    const int inset =
        (ui::kit::px(ui::kit::Size::Control) - ui::kit::px(ui::kit::Size::PropertiesCheckBox)) / 2;
    const QPoint fill(inset + 3, inset + 3);
    expect(on.pixelColor(fill).lightness() > off.pixelColor(fill).lightness(),
           "checked checkbox is brighter than unchecked");
    expect(on.pixelColor(fill).blue() > on.pixelColor(fill).red(),
           "checked checkbox uses accent blue");
    expect(opacity->displayedValue() == "100", "whole values omit decimal zeroes");
    auto* parent = panel->findChild<ui::kit::KDropdown*>("propertiesParentDropdown");
    expect(parent && !parent->isEnabled() && parent->currentText() == "None" &&
               !parent->toolTip().isEmpty(),
           "Parent placeholder is honest and disabled");
    expect(xRow->findChild<QLabel*>("propertiesRowLabel")->font().capitalization() ==
               QFont::MixedCase,
           "property labels preserve title case");
    expect(grid->property("selectedPoint").toInt() == 4, "default anchor highlights centre");
    idle(*grid);
    const auto image = grid->grab().toImage();
    expect(image.pixelColor(gridPointRect(4).center()).lightness() >
               image.pixelColor(gridPointRect(0).center()).lightness(),
           "selected anchor point is brighter");
    const auto before = stack.size();
    click(*grid, gridPointRect(0).center());
    waitForBounds(*grid);
    expect(stack.size() == before + 1 && grid->property("selectedPoint").toInt() == 0,
           "anchor corner is one undoable edit");
    const auto anchor = session.effectiveVec2Value(document::kAnchorParameterRole);
    expect(anchor && anchor->x == -960 && anchor->y == -540, "corner uses half local dimensions");
    (void)session.setSelectedAnchor(13, 17);
    waitForBounds(*grid);
    expect(grid->property("selectedPoint").toInt() == -1, "custom anchor clears highlight");
    (void)session.setSelectedAnchor(0, 0);
    waitForBounds(*grid);
    const auto layerId = std::get<document::LayerId>(session.selection().primary);
    const auto sourceId = session.directSourceNodeForLayer(layerId);
    if (sourceId) {
        session.selectNode(*sourceId);
        settle();
        expect(!grid->isEnabled(),
               "a source-node selection cannot retain the layer's active anchor grid");
        session.selectLayer(layerId);
        waitForBounds(*grid);
        expect(grid->property("selectedPoint").toInt() == 4,
               "returning to the layer resolves its anchor again");
    }
    const auto* source = sourceId ? session.composition()->graph().findNode(*sourceId) : nullptr;
    document::ParameterId widthParameter;
    if (source)
        for (const auto& binding : source->parameters)
            if (binding.role == "width")
                widthParameter = binding.parameterId;
    expect(widthParameter.isValid(), "solid exposes local width");
    if (widthParameter.isValid()) {
        (void)session.setParameterValue(widthParameter, 200.0, "Resize Solid");
        waitForBounds(*grid);
        click(*grid, gridPointRect(5).center());
        waitForBounds(*grid);
        const auto resizedAnchor = session.effectiveVec2Value(document::kAnchorParameterRole);
        expect(resizedAnchor && resizedAnchor->x == 100 && resizedAnchor->y == 0,
               "grid uses source bounds rather than composition dimensions");
        (void)session.setParameterValue(widthParameter, 1920.0, "Restore Solid");
        (void)session.setSelectedAnchor(0, 0);
        waitForBounds(*grid);
    }
    for (auto* section : panel->findChildren<ui::kit::KSection*>()) {
        auto* reset = section->findChild<ui::kit::KButton*>("kSectionReset");
        expect(reset && reset->text().isEmpty() &&
                   reset->width() == ui::kit::px(ui::kit::Size::Control),
               "section reset is a compact icon");
        if (reset) {
            idle(*reset);
            expect(reset->grab().toImage().pixelColor(2, 2).lightness() < 80,
                   "reset icon has no light resting border");
        }
    }
    auto* expand = panel->findChild<ui::kit::KButton*>("propertiesSolidColorExpand");
    expect(expand, "color has a chevron disclosure");
    if (expand)
        expand->setChecked(true);
    settle();
    const auto channels =
        panel->findChild<QWidget*>("solidColorFieldGroup")->findChildren<ui::kit::KValueField*>();
    expect(channels.size() == 4, "RGBA disclosure retains four cells");
    if (channels.size() == 4)
        for (auto* field : channels)
            expect(field->y() == channels.front()->y(), "RGBA channels share one line");
    for (const int width : {300, 360}) {
        area.resize(width, 800);
        settle();
        auto* scroll = panel->findChild<QScrollArea*>("propertiesScrollArea");
        expect(area.width() == width && scroll->widget()->width() <= scroll->viewport()->width(),
               "compact panel holds the minimum width");
        auto* label = xRow->findChild<QLabel*>("propertiesRowLabel");
        expect(label->width() == (width == 300 ? 72 : 96), "label column uses responsive tokens");
    }
    if (expand)
        expand->setChecked(false);
    settle();
    for (auto* child : area.findChildren<QWidget*>())
        idle(*child);
    const auto screenshot = qEnvironmentVariable("BLOOM_PROPS_SCREENSHOT");
    if (!screenshot.isEmpty())
        expect(area.grab().save(screenshot), "save supervisor screenshot");
    idle(*opacityDiamond);
    const auto unkeyed = opacityDiamond->grab().toImage();
    expect(session.toggleKeyframe(document::kOpacityParameterRole), "key opacity");
    idle(*opacityDiamond);
    const auto keyed = opacityDiamond->grab().toImage();
    (void)session.setCurrentTime(core::RationalTime::fromInteger(1));
    idle(*opacityDiamond);
    const auto between = opacityDiamond->grab().toImage();
    const auto amberPixels = [](const QImage& image) {
        int count = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const auto color = image.pixelColor(x, y);
                if (color.red() > 150 && color.green() > 100 && color.blue() < 130)
                    ++count;
            }
        return count;
    };
    expect(amberPixels(keyed) > amberPixels(between) && amberPixels(between) > amberPixels(unkeyed),
           "keyed diamond is filled amber, between-key diamond is half, unkeyed is outline");
    expect(session.addTextLayer("Text", "Hello", 72, {1, 1, 1, 1}), "create Text fixture");
    settle();
    auto* font = panel->findChild<ui::kit::KDropdown*>("textFontName");
    auto* size = panel->findChild<ui::kit::KValueField*>("textSizeEditor");
    expect(font && !font->isEnabled() && font->currentText() == "DejaVu Sans",
           "text face is a disabled dropdown");
    expect(size && size->hasStepper(), "font size has steppers");
    auto* segments = panel->findChild<ui::kit::KRadioGroup*>("propertiesRegistryEnum");
    expect(segments && segments->count() == 3, "text alignment uses three icon segments");
    if (size) {
        const auto history = stack.size();
        click(*size, QPoint(size->width() - 3, 3));
        expect(size->value() == 73 && stack.size() == history + 1, "stepper writes one edit");
    }
    if (segments)
        segments->setCurrentIndex(2);
    for (auto* r : panel->findChildren<QWidget*>("propertiesRegistryRow")) {
        if (r->property("role") != "line-height" && r->property("role") != "letter-spacing")
            continue;
        auto* field = r->findChild<ui::kit::KValueField*>();
        expect(field && field->hasStepper() && field->unit() == "%",
               "text spacing maps to generic percentage steppers");
        if (field && r->property("role") == "letter-spacing") {
            field->setValue(100);
            const auto id =
                document::ParameterId::fromRaw(r->property("parameterId").toULongLong());
            expect(session.effectiveScalarValue(id) == 73,
                   "percentage spacing converts to pixel advance");
            (void)session.setSelectedTextSize(146);
            expect(field->value() == 50, "percentage spacing reprojects after font size edit");
        }
    }
    const auto textScreenshot = qEnvironmentVariable("BLOOM_PROPS_TEXT_SCREENSHOT");
    if (!textScreenshot.isEmpty()) {
        waitForBounds(*grid);
        for (auto* child : area.findChildren<QWidget*>())
            idle(*child);
        expect(area.grab().save(textScreenshot), "save text visual check");
    }
}
} // namespace
int main(int argc, char** argv) try {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("BloomPropertiesMockupTest");
    QSettings().clear();
    ui::kit::installKinetikTheme(app);
    run();
    QSettings().clear();
    return failures ? 1 : 0;
}

catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
}
