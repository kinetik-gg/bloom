// The Timeline layer table (lane T2): the Label column and its menu, Select Label Group and the
// label filter; the Shy switch and the master Shy toggle beside Solo, Lock, Video and Audio; one
// row height and icon-sized switch cells with the name taking the extra width; one hover and
// selection highlight on every expanded row, a property row selecting its keys; After Effects'
// Timeline keys; layer names that follow their source; and #401's bounded per-tick cost.
//
// Review captures: BLOOM_TIMELINE_LAYERS_CAPTURE_DIR saves each state at 2000 and 900 pixels wide,
// suffixed with the device pixel ratio (run once with QT_SCALE_FACTOR=1.5 for the second ratio).
#include "surface_harness.hpp"

#include "timeline_layer_fields.hpp"
#include "timeline_layer_labels.hpp"
#include "timeline_property_rows.hpp"
#include "timeline_reveal.hpp"

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/composition_settings_operations.hpp>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/node_editor_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/swatch_button.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/tool_controller.hpp>

#include <QDir>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>

#include <set>

using namespace bloom;
using namespace bloom::ui;
using namespace bloom::ui::surface_test;

namespace {

core::RationalTime frame(const std::int64_t index) {
    return *core::RationalTime::create(index, 24);
}

document::NewProject makeProject() {
    auto project = document::makeNewProject("T2", "Main", core::RationalTime::fromInteger(10));
    document::AssetRecord asset;
    asset.id = document::AssetId::fromRaw(80);
    asset.kind = document::AssetKind::Image;
    asset.name = "Plate";
    asset.locator = {"file", "project-relative", "plate.png", "file:///plate.png"};
    asset.width = asset.height = 16;
    expect(project.project.addAsset(asset), "the image asset fixture is valid");
    return project;
}

struct Table final {
    document::NewProject initial = makeProject();
    document::Document document{std::move(initial.project)};
    commands::CommandStack stack{document};
    CompositionSession session{document, stack, initial.initialCompositionId};
    runtime::TaskScheduler scheduler{schedulerConfig()};
    TaskUiBridge bridge{scheduler, nullptr, std::chrono::milliseconds{1}};
    Pipeline pipeline;
    CompositionPreviewController controller{session, scheduler, bridge, pipeline.prepare};
    QWidget window;
    TimelineEditor* timeline = nullptr;

    Table() {
        auto* layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        timeline = new TimelineEditor(session, controller, &window);
        layout->addWidget(timeline);
        resize(1400);
        window.show();
        window.activateWindow();
        (void)QTest::qWaitForWindowExposed(&window);
        QCoreApplication::processEvents();
    }
    ~Table() {
        controller.beginShutdown();
        bridge.beginShutdown();
        QElapsedTimer timer;
        timer.start();
        while (!scheduler.isQuiescent() && timer.elapsed() < 4'000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;

    void resize(const int width) {
        window.resize(width, kit::px(kit::Size::ListRow) * 14);
        QCoreApplication::processEvents();
        timeline->applyDefaultLayerColumnWidth();
        QCoreApplication::processEvents();
    }
    [[nodiscard]] TimelineLayerStack& rows() const { return *timeline->layerStackForTest(); }
    [[nodiscard]] kit::KRow* layerRow(const int index) const {
        for (auto* row : rows().findChildren<kit::KRow*>(QStringLiteral("timelineLayerRow")))
            if (row->isVisible() && row->y() == rows().rowTop(index))
                return row;
        return nullptr;
    }
    [[nodiscard]] QWidget* propertyRow(const int index) const {
        for (auto* row : rows().findChildren<QWidget*>(QStringLiteral("timelinePropertyRow")))
            if (row->isVisible() && row->y() == rows().rowTop(index))
                return row;
        return nullptr;
    }
    [[nodiscard]] document::LayerId layer(const int index) const {
        return rows().entries()[static_cast<std::size_t>(index)].layerId;
    }
    [[nodiscard]] int layerRowCount() const {
        return static_cast<int>(std::ranges::count(
            rows().entries(), TimelineLayerEntry::Kind::Layer, &TimelineLayerEntry::rowKind));
    }
    [[nodiscard]] const document::LayerOutputBoundary* record(const document::LayerId id) const {
        return session.composition()->graph().findLayer(id);
    }
    [[nodiscard]] int rowOf(const document::LayerId id, const std::string_view role) const {
        const auto& entries = rows().entries();
        for (std::size_t index = 0; index < entries.size(); ++index)
            if (entries[index].layerId == id && entries[index].role == role &&
                entries[index].rowKind == TimelineLayerEntry::Kind::Parameter)
                return static_cast<int>(index);
        return -1;
    }
    [[nodiscard]] std::vector<TimelineLayerEntry> childrenOf(const document::LayerId id) const {
        std::vector<TimelineLayerEntry> result;
        for (const auto& entry : rows().entries())
            if (entry.layerId == id && entry.rowKind != TimelineLayerEntry::Kind::Layer)
                result.push_back(entry);
        return result;
    }
    void key(const Qt::Key key, const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        rows().setFocus(Qt::OtherFocusReason);
        QTest::keyClick(&rows(), key, modifiers);
        QCoreApplication::processEvents();
    }
    void execute(commands::Transaction transaction) {
        expect(session.executeTransaction(std::move(transaction)).changed(), "fixture command");
        QCoreApplication::processEvents();
    }
};

// The one visible popup menu called `name` (closed menus delete themselves later).
QMenu* openMenu(const QObject& parent, const char* name) {
    QCoreApplication::processEvents();
    for (auto* menu : parent.findChildren<QMenu*>(QString::fromLatin1(name)))
        if (menu->isVisible())
            return menu;
    return nullptr;
}

QAction* actionNamed(QMenu& menu, const QString& text) {
    for (auto* action : menu.actions())
        if (action->text() == text)
            return action;
    return nullptr;
}

void closeMenu(QMenu* menu) {
    if (menu != nullptr)
        menu->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

QString ratioSuffix(const QWidget& widget) {
    return widget.devicePixelRatioF() > 1.0 ? QStringLiteral("dpr15") : QStringLiteral("dpr1");
}

// Saves the Timeline, with `menu` (when open) painted where it pops up.
void capture(Table& table, const QString& name, QMenu* menu = nullptr) {
    const auto directory = qEnvironmentVariable("BLOOM_TIMELINE_LAYERS_CAPTURE_DIR");
    if (directory.isEmpty())
        return;
    QTest::qWait(50);
    QPixmap shot = table.window.grab();
    if (menu != nullptr) {
        QPainter painter(&shot);
        painter.drawPixmap(table.window.mapFromGlobal(menu->pos()), menu->grab());
    }
    (void)QDir().mkpath(directory);
    (void)shot.save(QDir(directory).filePath(QStringLiteral("%1-w%2-%3.png")
                                                 .arg(name)
                                                 .arg(table.window.width())
                                                 .arg(ratioSuffix(table.window))));
}

void addSolids(Table& table, const int count) {
    for (int index = 0; index < count; ++index)
        expect(addDefaultSolidLayer(table.session), "the fixture adds a solid layer");
    QCoreApplication::processEvents();
}

void keyPosition(Table& table, const document::LayerId layer, std::initializer_list<int> frames) {
    const auto boundary = table.session.boundaryNodeForLayer(layer);
    const auto* node = table.session.composition()->graph().findNode(*boundary);
    const auto binding = std::ranges::find(node->parameters, document::kPositionParameterRole,
                                           &document::ParameterBinding::role);
    for (const int at : frames) {
        (void)table.session.setCurrentTime(frame(at));
        expect(table.session.toggleKeyframeForParameter(binding->parameterId),
               "the fixture keys Position");
    }
    (void)table.session.setCurrentTime(frame(0));
    QCoreApplication::processEvents();
}

// Item 1: the Label square, its menu with Select Label Group, and the label filter.
void testLabels() {
    Table table;
    addSolids(table, 3);
    auto& rows = table.rows();
    const auto first = table.layer(0), second = table.layer(1), third = table.layer(2);
    auto* square = table.layerRow(0)->findChild<kit::KSwatchButton*>("timelineLayerLabel");
    expect(square != nullptr && square->width() == kit::px(kit::Size::ToggleCell),
           "every layer row carries a ToggleCell Label square");
    expect(square && square->swatch() == kit::color(rows.entries()[0].clipColor),
           "without a label the square shows the kind colour the bar wears");
    click(square);
    auto* menu = openMenu(rows, "timelineLayerLabelMenu");
    expect(menu != nullptr, "clicking the square opens the label menu");
    expect(menu && actionNamed(*menu, TimelineEditor::tr("Select Label Group")) != nullptr &&
               actionNamed(*menu, TimelineEditor::tr("Label 8")) != nullptr &&
               actionNamed(*menu, TimelineEditor::tr("Kind Default")) != nullptr,
           "the label menu lists the existing label colours and Select Label Group");
    if (menu)
        actionNamed(*menu, TimelineEditor::tr("Label 3"))->trigger();
    closeMenu(menu);
    const auto label3 = layer_labels::Label{std::array<std::uint8_t, 3>{
        static_cast<std::uint8_t>(layer_labels::presetColor(3).red()),
        static_cast<std::uint8_t>(layer_labels::presetColor(3).green()),
        static_cast<std::uint8_t>(layer_labels::presetColor(3).blue())}};
    expect(layer_labels::labelOf(table.session, first) == label3 &&
               table.session.undoLabel() == QStringLiteral("Set Layer Label Color"),
           "a label from the menu commits one Set Layer Label Color step");
    square = table.layerRow(0)->findChild<kit::KSwatchButton*>("timelineLayerLabel");
    expect(square && square->swatch().rgb() == layer_labels::presetColor(3).rgb(),
           "the square shows the new label colour");
    click(table.layerRow(1)->findChild<kit::KSwatchButton*>("timelineLayerLabel"));
    menu = openMenu(rows, "timelineLayerLabelMenu");
    if (menu)
        actionNamed(*menu, TimelineEditor::tr("Label 3"))->trigger();
    closeMenu(menu);
    click(table.layerRow(0)->findChild<kit::KSwatchButton*>("timelineLayerLabel"));
    menu = openMenu(rows, "timelineLayerLabelMenu");
    if (menu)
        actionNamed(*menu, TimelineEditor::tr("Select Label Group"))->trigger();
    closeMenu(menu);
    const auto& nodes = table.session.selectedNodes();
    expect(nodes.contains(*table.session.boundaryNodeForLayer(first)) &&
               nodes.contains(*table.session.boundaryNodeForLayer(second)) &&
               !nodes.contains(*table.session.boundaryNodeForLayer(third)),
           "Select Label Group selects exactly the layers sharing the label");
    // On a selected layer the menu labels the whole selection, in one step.
    table.session.selectNodes(
        {*table.session.boundaryNodeForLayer(first), *table.session.boundaryNodeForLayer(third)},
        *table.session.boundaryNodeForLayer(first));
    QCoreApplication::processEvents();
    click(table.layerRow(0)->findChild<kit::KSwatchButton*>("timelineLayerLabel"));
    menu = openMenu(rows, "timelineLayerLabelMenu");
    if (menu)
        actionNamed(*menu, TimelineEditor::tr("Label 5"))->trigger();
    closeMenu(menu);
    expect(layer_labels::filterKey(layer_labels::labelOf(table.session, first)) == 5 &&
               layer_labels::filterKey(layer_labels::labelOf(table.session, third)) == 5 &&
               layer_labels::filterKey(layer_labels::labelOf(table.session, second)) == 3,
           "the label applies to every selected layer");
    expect(table.session.undo() &&
               layer_labels::filterKey(layer_labels::labelOf(table.session, first)) == 3 &&
               layer_labels::filterKey(layer_labels::labelOf(table.session, third)) ==
                   layer_labels::kKindDefault,
           "one undo restores both");
    QCoreApplication::processEvents();

    // The header's label filter.
    auto* filter = table.timeline->findChild<kit::KIconToggle*>("timelineLabelFilterButton");
    expect(filter != nullptr && !filter->isChecked(), "the header carries the label filter");
    click(filter);
    menu = openMenu(*table.timeline, "timelineLabelFilterMenu");
    expect(menu != nullptr, "the filter opens its menu");
    if (menu)
        actionNamed(*menu, TimelineEditor::tr("Label 3"))->trigger();
    closeMenu(menu);
    expect(table.layerRowCount() == 2 && filter->isChecked(),
           "the filter shows only the layers with the chosen labels and lights up");
    expect(table.session.composition()->graph().layerOutputs().size() == 3,
           "filtering never removes a layer");
    click(filter);
    menu = openMenu(*table.timeline, "timelineLabelFilterMenu");
    if (menu)
        actionNamed(*menu, TimelineEditor::tr("All Labels"))->trigger();
    closeMenu(menu);
    expect(table.layerRowCount() == 3 && !filter->isChecked(), "All Labels shows every layer");
}

// Item 2: the switch cells, Shy and the master Shy toggle.
void testSwitches() {
    Table table;
    addSolids(table, 3);
    auto* row = table.layerRow(1);
    for (const auto* name : {"timelineLayerToggle0", "timelineLayerToggle2", "timelineLayerToggle3",
                             "timelineLayerShy"})
        expect(row && row->findChild<kit::KIconToggle*>(QString::fromLatin1(name)) != nullptr,
               std::string("the row carries the switch ") + name);
    const auto shyLayer = table.layer(1);
    auto* shy = row->findChild<kit::KIconToggle*>("timelineLayerShy");
    click(shy);
    expect(layer_fields::shy(table.session, shyLayer) &&
               table.session.undoLabel() == QStringLiteral("Set Layer Shy"),
           "the Shy switch marks its layer shy in one Set Layer Shy step");
    shy = table.layerRow(1)->findChild<kit::KIconToggle*>("timelineLayerShy");
    expect(shy && shy->isChecked(), "and shows the Shy glyph");
    auto* master = table.timeline->findChild<kit::KIconToggle*>("timelineHideShyToggle");
    expect(master != nullptr && !master->isChecked(), "the header carries the master Shy toggle");
    click(master);
    expect(master->isChecked() && table.layerRowCount() == 2 &&
               std::ranges::none_of(table.rows().entries(),
                                    [&](const auto& e) { return e.layerId == shyLayer; }),
           "the master toggle hides shy layers from the list");
    expect(table.session.composition()->graph().layerOutputs().size() == 3,
           "a hidden shy layer stays in the composition");
    click(master);
    expect(!master->isChecked() && table.layerRowCount() == 3, "and shows them again");
    expect(table.session.undo() && !layer_fields::shy(table.session, shyLayer),
           "undo takes the Shy switch back");
    QCoreApplication::processEvents();
    click(table.layerRow(1)->findChild<kit::KIconToggle*>("timelineLayerShy"));
    // Solo and lock still commit from their cells.
    click(table.layerRow(0)->findChild<kit::KIconToggle*>("timelineLayerToggle2"));
    expect(table.record(table.layer(0))->solo, "Solo still commits");
}

// Item 3: one row height, icon-sized switch cells aligned with the header, the name taking the
// extra width, and nothing truncated at about 900 pixels.
void testSizing() {
    Table table;
    addSolids(table, 3);
    int wideName = 0;
    for (const int width : {2000, 900}) {
        table.resize(width);
        auto* headers = table.timeline->findChild<QWidget*>("timelineColumnHeaders");
        const auto inEditor = [&](const QWidget* widget) {
            return widget->mapTo(table.timeline, QPoint{});
        };
        for (int index = 0; index < 3; ++index) {
            auto* row = table.layerRow(index);
            expect(row && row->height() == kit::px(kit::Size::ListRow),
                   "every layer row is one ListRow tall");
            if (row == nullptr)
                continue;
            for (auto* cell : row->findChildren<QWidget*>())
                if (cell->property("rowCell").toString() == QStringLiteral("toggle") &&
                    cell->parentWidget() == row)
                    expect(cell->size() == QSize(kit::px(kit::Size::ToggleCell),
                                                 kit::px(kit::Size::ToggleCell)),
                           "every switch cell is one ToggleCell");
            QWidget* previous = nullptr;
            for (auto* cell : row->findChildren<QWidget*>()) {
                if (cell->parentWidget() != row || !cell->isVisible() ||
                    cell->property("rowCell").toString().isEmpty())
                    continue;
                expect(cell->geometry().right() < row->width(), "no cell leaves the row");
                expect(previous == nullptr || previous->geometry().right() < cell->x() ||
                           previous->x() > cell->geometry().right(),
                       "no two cells overlap");
                previous = cell;
            }
            expect(row->nameLabel()->text() == table.rows().entries()[static_cast<std::size_t>(index)].name,
                   "the layer name is not truncated");
        }
        auto* row = table.layerRow(0);
        expect(inEditor(table.timeline->findChild<QWidget*>("timelineLabelFilterButton")).x() ==
                       inEditor(row->findChild<QWidget*>("timelineLayerLabel")).x() &&
                   inEditor(table.timeline->findChild<QWidget*>("timelineHideShyToggle")).x() ==
                       inEditor(row->findChild<QWidget*>("timelineLayerShy")).x(),
               "the header's label filter and Shy toggle stand over their columns");
        for (auto* label : headers->findChildren<kit::KLabel*>())
            expect(label->fontMetrics().horizontalAdvance(label->text()) <= label->width(),
                   "no column heading is truncated");
        if (width == 2000)
            wideName = row->nameLabel()->width();
        else
            expect(row->nameLabel()->width() < wideName && row->nameLabel()->width() > 0,
                   "the name column is what gives up width");
    }
}

// Item 4: hover and selection on every expanded row; a property row selects itself and its keys.
void testExpandedRows() {
    Table table;
    addSolids(table, 2);
    const auto layer = table.layer(0);
    keyPosition(table, layer, {5, 15});
    click(table.layerRow(0)->disclosureButton());
    const int position = table.rowOf(layer, document::kPositionParameterRole);
    expect(position > 0, "the twirl shows the Position row");
    auto* positionRow = table.propertyRow(position);
    expect(positionRow != nullptr, "the Position row is on screen");
    if (positionRow == nullptr)
        return;
    QEnterEvent enter(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
    QCoreApplication::sendEvent(positionRow, &enter);
    expect(table.rows().hoveredRowForTest() == position, "a property row takes the hover step");
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(positionRow, &leave);
    QCoreApplication::sendEvent(table.layerRow(0), &enter);
    expect(table.layerRow(0)->hovered(), "a layer row takes the same hover step");
    // A click on the row's own label selects the row and every one of its keys.
    auto* label = positionRow->findChild<QLabel*>("timelinePropertyLabel");
    click(label);
    const auto keys = timeline_reveal::rowKeys(
        table.session, table.rows().entries()[static_cast<std::size_t>(position)]);
    expect(keys.size() == 4 && table.session.selection().keyframes.size() == keys.size(),
           "clicking a property row selects all of its keys");
    expect(table.rows().rowHighlighted(position), "and highlights the row");
    bool laneHighlighted = false;
    for (auto* lane : table.timeline->findChildren<QWidget*>("timelineKeyframeRow"))
        laneHighlighted = laneHighlighted || lane->property("rowSelected").toBool();
    expect(laneHighlighted, "its key lane wears the same selected-row wash");
    expect(table.rows().selectedRows() == std::vector<int>{position},
           "the column paints exactly that row selected");
    // A group row is clickable too; it selects its layer.
    const auto transform = position - 1;
    expect(table.rows().entries()[static_cast<std::size_t>(transform)].rowKind ==
               TimelineLayerEntry::Kind::Group,
           "the Transform heading precedes Position");
    table.rows().selectRow(transform);
    QCoreApplication::processEvents();
    expect(table.rows().rowHighlighted(transform) && !table.rows().rowHighlighted(position) &&
               table.session.selection().contextualLayer == layer,
           "a group row selects its layer and highlights itself");
    table.session.clearSelection();
    QCoreApplication::processEvents();
    expect(!table.rows().rowHighlighted(transform), "a selection made elsewhere clears it");
}

// Item 5: After Effects' Timeline keys.
void testKeys() {
    Table table;
    addSolids(table, 2);
    const auto first = table.layer(0), second = table.layer(1);
    keyPosition(table, first, {5, 15});
    table.session.clearSelection();
    QCoreApplication::processEvents();
    const auto roles = [&](const document::LayerId layer) {
        std::vector<std::string> result;
        for (const auto& row : table.childrenOf(layer))
            result.push_back(row.rowKind == TimelineLayerEntry::Kind::Group
                                 ? "group:" + row.group.toStdString()
                                 : row.role);
        return result;
    };
    const std::vector<std::string> positionOnly{std::string(document::kPositionParameterRole)};
    table.key(Qt::Key_P);
    expect(roles(first) == positionOnly && roles(second) == positionOnly,
           "P with nothing selected shows Position on every layer, directly under it");
    table.session.selectLayer(first);
    QCoreApplication::processEvents();
    table.key(Qt::Key_S, Qt::ShiftModifier);
    expect(roles(first) == std::vector<std::string>{std::string(document::kPositionParameterRole),
                                                    std::string(document::kScaleParameterRole)} &&
               roles(second) == positionOnly,
           "Shift+S adds Scale on the selected layer only");
    table.key(Qt::Key_P);
    expect(roles(first) == positionOnly, "P alone shows Position alone again");
    table.key(Qt::Key_P);
    expect(roles(first).empty(), "P again closes it");
    for (const auto& [key, role] : {std::pair{Qt::Key_A, document::kAnchorParameterRole},
                                    std::pair{Qt::Key_R, document::kRotationParameterRole},
                                    std::pair{Qt::Key_T, document::kOpacityParameterRole}}) {
        table.key(key);
        expect(roles(first) == std::vector<std::string>{std::string(role)},
               std::string("each letter reveals its property: ") + std::string(role));
    }
    expect(table.session.tools().tool() == ViewerTool::Select,
           "with the Timeline focused R, E, P and T never choose a tool");
    table.key(Qt::Key_U);
    expect(roles(first) == positionOnly, "U shows the keyed properties");
    QTest::qWait(QApplication::doubleClickInterval() + 50);
    table.key(Qt::Key_U);
    table.key(Qt::Key_U);
    const auto modified = roles(first);
    expect(std::ranges::find(modified, std::string(document::kPositionParameterRole)) !=
                   modified.end() &&
               std::ranges::find(modified, std::string(document::kRotationParameterRole)) ==
                   modified.end(),
           "UU shows what differs from a new layer, never an untouched default");
    commands::Transaction mask("Add Mask", table.session.snapshot().revision());
    document::PathValue path;
    path.closed = true;
    path.anchors = {{{0, 0}, {}, {}}, {{10, 0}, {}, {}}, {{10, 10}, {}, {}}};
    mask.emplace<commands::AddLayerMask>(table.session.compositionId(), first, path);
    table.execute(std::move(mask));
    table.key(Qt::Key_M);
    const auto masks = table.childrenOf(first);
    expect(masks.size() == 2 && masks[0].rowKind == TimelineLayerEntry::Kind::Group &&
               masks[1].role == "mask-path",
           "M shows each mask's path in the Masks group");
    table.key(Qt::Key_E);
    const auto effects = table.childrenOf(first);
    expect(!effects.empty() && effects.front().group == QObject::tr("Source") &&
               std::ranges::none_of(
                   effects,
                   [](const auto& row) { return row.role == document::kPositionParameterRole; }),
           "E shows the node feeding the layer (Bloom's effects)");
    table.key(Qt::Key_E);
    expect(table.childrenOf(first).empty(), "E again closes it");

    // J / K step through the keys the Timeline shows.
    (void)table.session.setCurrentTime(frame(0));
    table.key(Qt::Key_K);
    expect(table.session.currentTime() == frame(5), "K goes to the next key");
    table.key(Qt::Key_K);
    expect(table.session.currentTime() == frame(15), "K again goes to the one after");
    table.key(Qt::Key_J);
    expect(table.session.currentTime() == frame(5), "J goes back");

    // [ and ] move the layer to the playhead; Alt trims.
    (void)table.session.setCurrentTime(frame(10));
    table.key(Qt::Key_BracketLeft);
    expect(table.record(first)->inPoint == frame(10) &&
               table.session.undoLabel() == QStringLiteral("Move Layer"),
           "[ moves the in point to the playhead");
    table.key(Qt::Key_BracketRight);
    expect(table.record(first)->endPoint(table.session.composition()->duration()) == frame(11),
           "] moves the layer so its last frame is the playhead's");
    (void)table.session.setCurrentTime(frame(4));
    table.key(Qt::Key_BracketLeft, Qt::AltModifier);
    expect(table.record(first)->inPoint == frame(4) &&
               table.session.undoLabel() == QStringLiteral("Trim Layer"),
           "Alt+[ trims the in point to the playhead");
    (void)table.session.setCurrentTime(frame(8));
    table.key(Qt::Key_BracketRight, Qt::AltModifier);
    expect(table.record(first)->endPoint(table.session.composition()->duration()) == frame(9),
           "Alt+] trims the out point, keeping the playhead's frame");
    expect(table.record(second)->inPoint == frame(0), "only the selected layer moves");

    // A field being typed in keeps its letters.
    const auto before = roles(second);
    int secondRow = 0;
    while (table.layer(secondRow) != second)
        ++secondRow;
    QTest::mouseDClick(
        &table.rows(), Qt::LeftButton, Qt::NoModifier,
        QPoint(kit::px(kit::Size::TimelineToggleColumn) + kit::px(kit::Size::TimelineNameMin) / 2,
               table.rows().rowTop(secondRow) + kit::px(kit::Size::ListRow) / 2));
    QCoreApplication::processEvents();
    auto* field = table.rows().findChild<QLineEdit*>("timelineLayerRenameEditor");
    expect(field != nullptr && field->hasFocus(), "double-clicking the name opens its field");
    if (field != nullptr) {
        QTest::keyClick(field, Qt::Key_P);
        QCoreApplication::processEvents();
        expect(field->text() == QStringLiteral("p") && roles(second) == before,
               "P typed into a field is text, not a reveal");
        QTest::keyClick(field, Qt::Key_Escape);
        QCoreApplication::processEvents();
    }
}

// Item 6: a layer made from an asset or a composition shows its source's name until renamed.
void testSourceNames() {
    Table table;
    commands::Transaction add("Add Layer", table.session.snapshot().revision());
    add.emplace<commands::AddImageLayer>(table.session.compositionId(),
                                         document::AssetId::fromRaw(80));
    table.execute(std::move(add));
    const auto layer = table.layer(0);
    expect(table.rows().entries()[0].name == QStringLiteral("Plate") &&
               layer_fields::nameFollowsSource(table.session, layer),
           "a new image layer shows its asset's name");
    commands::Transaction rename("Rename Asset", table.session.snapshot().revision());
    rename.emplace<commands::RenameAsset>(document::AssetId::fromRaw(80), "Hero plate");
    table.execute(std::move(rename));
    expect(table.rows().entries()[0].name == QStringLiteral("Hero plate"),
           "renaming the asset renames the layer that uses it");
    commands::Transaction own("Rename Layer", table.session.snapshot().revision());
    own.emplace<commands::RenameLayer>(table.session.compositionId(), layer, "Artist layer");
    table.execute(std::move(own));
    commands::Transaction again("Rename Asset", table.session.snapshot().revision());
    again.emplace<commands::RenameAsset>(document::AssetId::fromRaw(80), "Villain plate");
    table.execute(std::move(again));
    expect(table.rows().entries()[0].name == QStringLiteral("Artist layer") &&
               !layer_fields::nameFollowsSource(table.session, layer),
           "a layer the artist renamed keeps its own name");
    // A nested composition layer follows its composition's name the same way.
    commands::Transaction nested("Add Composition", table.session.snapshot().revision());
    nested.emplace<commands::AddComposition>("Nested", table.session.composition()->format(),
                                             core::RationalTime::fromInteger(2));
    table.execute(std::move(nested));
    const auto nestedId = table.session.snapshot().project().compositions().back().id();
    commands::Transaction place("Add Layer", table.session.snapshot().revision());
    place.emplace<commands::AddCompositionSource>(table.session.compositionId(), nestedId,
                                                  document::Vec2d{}, true);
    table.execute(std::move(place));
    const auto nestedLayer = table.session.composition()->graph().layerOutputs().back().layerId;
    expect(layer_fields::displayName(table.session, nestedLayer) == QStringLiteral("Nested"),
           "a nested composition layer shows the composition's name");
    {
        commands::Transaction settings("Composition Settings", table.session.snapshot().revision());
        commands::CompositionSettingsChange change;
        change.name = "Renamed";
        settings.emplace<commands::UpdateCompositionSettings>(nestedId, change);
        table.execute(std::move(settings));
        expect(layer_fields::displayName(table.session, nestedLayer) == QStringLiteral("Renamed"),
               "renaming the composition renames the layer");
    }
}

// #401: playback ticks touch only what is on screen, a bounded amount per tick.
void testPlaybackCost() {
    Table table;
    addSolids(table, 50);
    table.session.clearSelection();
    table.timeline->verticalScrollBarForTest()->setValue(0);
    QCoreApplication::processEvents();
    expect(table.layerRow(0) != nullptr, "the first layer is on screen");
    if (table.layerRow(0) == nullptr)
        return;
    click(table.layerRow(0)->disclosureButton());
    table.timeline->verticalScrollBarForTest()->setValue(0);
    QCoreApplication::processEvents();
    keyPosition(table, table.layer(0), {2, 20});
    // Playback with a layer selected, as an artist plays it. Let every deferred refresh of the
    // setup land before counting.
    table.session.selectLayer(table.layer(0));
    QTest::qWait(100);
    int visibleProperties = 0;
    for (auto* row : table.rows().findChildren<QWidget*>(QStringLiteral("timelinePropertyRow")))
        visibleProperties += row->isVisible() ? 1 : 0;
    const auto lanes = table.timeline->laneRegionForTest()->work();
    const auto stackWork = table.rows().work();
    const auto binds = TimelinePropertyRow::bindCount();
    constexpr int kTicks = 24;
    for (int tick = 1; tick <= kTicks; ++tick) {
        (void)table.session.setCurrentTime(frame(tick));
        QCoreApplication::processEvents();
    }
    const auto after = table.timeline->laneRegionForTest()->work();
    expect(after.fullInvalidations == lanes.fullInvalidations, "a tick never repaints every lane");
    expect(after.playheadInvalidations - lanes.playheadInvalidations <= kTicks,
           "a tick repaints the playhead's strips once");
    expect(table.rows().work().layerRowBinds == stackWork.layerRowBinds,
           "a tick re-binds no layer row");
    expect(visibleProperties > 0 && TimelinePropertyRow::bindCount() - binds <=
                                        static_cast<std::uint64_t>(kTicks * visibleProperties),
           "a tick re-reads at most the property rows on screen, once each");
    expect(visibleProperties < 50, "and the rows on screen are far fewer than the layers");
}

// #401 in the composition panels together (Nodes, Properties, Timeline), 25 layers, one of them
// animated, selected and expanded: 120 playback ticks request no layout at all, and repaint only
// the value cells whose numbers move.
class PlaybackChurn final : public QObject {
  public:
    int layoutRequests = 0;
    int paints = 0;
    int colorChipPaints = 0;
    std::set<const QWidget*> paintedFields;
    bool counting = false;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (counting && event->type() == QEvent::LayoutRequest)
            ++layoutRequests;
        if (counting && event->type() == QEvent::Paint)
            ++paints;
        if (counting && event->type() == QEvent::Paint &&
            qobject_cast<const kit::KColorChip*>(watched) != nullptr)
            ++colorChipPaints;
        if (counting && event->type() == QEvent::Paint)
            if (const auto* field = qobject_cast<const kit::KValueField*>(watched))
                paintedFields.insert(field);
        return false;
    }
};

void testPlaybackChurn() {
    Surfaces surfaces;
    for (int index = 0; index < 25; ++index)
        expect(addDefaultSolidLayer(surfaces.session), "the fixture adds a solid layer");
    QCoreApplication::processEvents();
    auto* stack = surfaces.layerStack();
    const auto layer = stack->entries().front().layerId;
    surfaces.session.selectLayer(layer);
    const auto boundary = surfaces.session.boundaryNodeForLayer(layer);
    const auto* node = surfaces.session.composition()->graph().findNode(*boundary);
    const auto binding = std::ranges::find(node->parameters, document::kPositionParameterRole,
                                           &document::ParameterBinding::role);
    for (const int at : {0, 100}) {
        (void)surfaces.session.setCurrentTime(frame(at));
        expect(surfaces.session.toggleKeyframeForParameter(binding->parameterId),
               "the fixture keys Position");
    }
    (void)surfaces.session.setCurrentTime(frame(100));
    expect(surfaces.session.setSelectedPosition(10, 20), "the second key moves the layer");
    (void)surfaces.session.setCurrentTime(frame(0));
    QCoreApplication::processEvents();
    if (auto* row = surfaces.layerRow(0))
        if (auto* disclosure = qobject_cast<kit::KRow*>(row)->disclosureButton())
            click(disclosure);
    QTest::qWait(100);
    PlaybackChurn churn;
    QCoreApplication::instance()->installEventFilter(&churn);
    const auto structure = TimelinePropertyRow::structureBindCount();
    const auto rebuilds = surfaces.properties->work().fullRebuilds;
    churn.counting = true;
    constexpr int kTicks = 120;
    for (int tick = 1; tick <= kTicks; ++tick) {
        (void)surfaces.session.setCurrentTime(frame(tick % 100));
        QCoreApplication::processEvents();
    }
    QCoreApplication::processEvents();
    churn.counting = false;
    QCoreApplication::instance()->removeEventFilter(&churn);
    expect(churn.layoutRequests == 0,
           "playback ticks request no layout (got " + std::to_string(churn.layoutRequests) + ")");
    expect(TimelinePropertyRow::structureBindCount() == structure,
           "a tick never rebuilds a timeline row's structure");
    expect(surfaces.properties->work().fullRebuilds == rebuilds,
           "a tick never rebuilds Properties");
    // Position X and Y, in Properties and in the timeline row: the only numbers that move.
    expect(churn.colorChipPaints == 0, "a colour that does not change never repaints");
    expect(churn.paintedFields.size() <= 4, "only the animated value cells repaint (got " +
                                                std::to_string(churn.paintedFields.size()) + ")");
}

// The review captures: every state at 2000 and at 900 pixels wide.
void captureStates(const int width) {
    if (qEnvironmentVariable("BLOOM_TIMELINE_LAYERS_CAPTURE_DIR").isEmpty())
        return;
    Table table;
    addSolids(table, 6);
    table.resize(width);
    commands::Transaction setup("Setup", table.session.snapshot().revision());
    const auto id = table.session.compositionId();
    for (int index = 0; index < 5; ++index) {
        const auto color = layer_labels::presetColor(index * 2 % 8 + 1);
        setup.emplace<commands::SetLayerLabelColor>(
            id, table.layer(index),
            std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(color.red()),
                                        static_cast<std::uint8_t>(color.green()),
                                        static_cast<std::uint8_t>(color.blue())});
    }
    setup.emplace<commands::SetLayerSolo>(id, table.layer(1), true);
    setup.emplace<commands::SetLayerLocked>(id, table.layer(3), true);
    setup.emplace<commands::RenameLayer>(id, table.layer(2), "Background plate with a long name");
    table.execute(std::move(setup));
    (void)layer_fields::setShy(table.session, {table.layer(4)}, true);
    table.timeline->findChild<kit::KIconToggle*>("timelineHideShyToggle")->click();
    table.timeline->findChild<kit::KIconToggle*>("timelineHideShyToggle")->click();
    QCoreApplication::processEvents();
    table.session.selectLayer(table.layer(0));
    QCoreApplication::processEvents();
    capture(table, QStringLiteral("layers"));
    capture(table, QStringLiteral("shy-master-off"));
    click(table.layerRow(1)->findChild<kit::KSwatchButton*>("timelineLayerLabel"));
    auto* menu = openMenu(table.rows(), "timelineLayerLabelMenu");
    capture(table, QStringLiteral("label-menu"), menu);
    closeMenu(menu);
    click(table.timeline->findChild<kit::KIconToggle*>("timelineLabelFilterButton"));
    menu = openMenu(*table.timeline, "timelineLabelFilterMenu");
    capture(table, QStringLiteral("label-filter-menu"), menu);
    if (menu)
        actionNamed(*menu, TimelineEditor::tr("Label 1"))->trigger();
    closeMenu(menu);
    capture(table, QStringLiteral("label-filter-on"));
    table.timeline->findChild<kit::KIconToggle*>("timelineLabelFilterButton")->click();
    menu = openMenu(*table.timeline, "timelineLabelFilterMenu");
    if (menu)
        actionNamed(*menu, TimelineEditor::tr("All Labels"))->trigger();
    closeMenu(menu);
    click(table.timeline->findChild<kit::KIconToggle*>("timelineHideShyToggle"));
    capture(table, QStringLiteral("shy-master-on"));
    click(table.timeline->findChild<kit::KIconToggle*>("timelineHideShyToggle"));
    keyPosition(table, table.layer(0), {3, 9, 16});
    click(table.layerRow(0)->disclosureButton());
    const int position = table.rowOf(table.layer(0), document::kPositionParameterRole);
    if (auto* row = table.propertyRow(position))
        click(row->findChild<QLabel*>("timelinePropertyLabel"));
    QEnterEvent enter(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
    if (auto* row = table.propertyRow(position + 1))
        QCoreApplication::sendEvent(row, &enter);
    capture(table, QStringLiteral("expanded-selected"));
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("BloomTimelineLayerTable"));
    QApplication::setApplicationName(QStringLiteral("TimelineLayerTable"));
    kit::installKinetikTheme(application);
    testLabels();
    testSwitches();
    testSizing();
    testExpandedRows();
    testKeys();
    testSourceNames();
    testPlaybackCost();
    testPlaybackChurn();
    captureStates(2000);
    captureStates(900);
    if (failures != 0)
        std::cerr << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}
