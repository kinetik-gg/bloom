#include "node_interaction_test_support.hpp"
#include <QAbstractItemModel>
#include <QAction>
#include <QModelIndex>
#include <QRegion>
#include <QShortcut>
#include <QStringList>
#include <memory>

namespace bloom::ui::test {
namespace {
QAction* named(QMenu* menu, const char* name) {
    return menu->findChild<QAction*>(QString::fromLatin1(name));
}
// A search result addressed by the node type it would add, rather than by its row number: the rows
// a filter produces are the popup's business and a row index pins nothing worth pinning.
QModelIndex rowForKey(const QAbstractItemModel* model, const std::string_view typeId) {
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto index = model->index(row, 0);
        if (index.data(Qt::UserRole).toString() == QString::fromUtf8(typeId))
            return index;
    }
    return {};
}
int resultRows(const QAbstractItemModel* model) {
    int rows = 0;
    for (int row = 0; row < model->rowCount(); ++row)
        if (!model->index(row, 0).data(kit::kSearchSectionRole).toBool())
            ++rows;
    return rows;
}
QStringList sectionHeadings(const QAbstractItemModel* model) {
    QStringList headings;
    for (int row = 0; row < model->rowCount(); ++row)
        if (model->index(row, 0).data(kit::kSearchSectionRole).toBool())
            headings.push_back(model->index(row, 0).data().toString());
    return headings;
}
kit::KSearchPopup* search(Fixture& f) {
    auto* popup = f.editor.findChild<kit::KSearchPopup*>();
    if (!popup)
        throw std::runtime_error("search popup missing");
    return popup;
}
// Task S1, item 8 moved mute, collapse and dissolve out of the keyboard and into the context menu
// alone, so the way to exercise one is now the menu item itself.
bool triggerNodeMenuAction(Fixture& f, const char* name) {
    std::unique_ptr<QMenu> menu(f.editor.contextMenuForTest(true));
    auto* action = named(menu.get(), name);
    if (action == nullptr)
        return false;
    action->trigger();
    return true;
}
} // namespace
void testSearchKeyboardAndMenus() {
    Fixture f;
    const auto a = f.add(document::kSolidSourceNodeType, {100, 100});
    const auto b = f.add(document::kLayerOutputNodeType, {400, 100});
    f.session.selectNodes({a, b}, b);
    auto history = f.stack.size();
    expect(triggerNodeMenuAction(f, "nodeMuteAction"), "the node menu offers Mute");
    expect(f.session.composition()->nodeLayout().at(a).muted &&
               f.session.composition()->nodeLayout().at(b).muted && f.stack.size() == history + 1,
           "Mute mutes the entire selection in one command transaction");
    expect(triggerNodeMenuAction(f, "nodeMuteAction"), "and offers Unmute next");
    expect(!f.session.composition()->nodeLayout().at(a).muted, "Unmute toggles the selection back");
    expect(triggerNodeMenuAction(f, "nodeCollapseAction"), "the node menu offers Collapse");
    expect(f.session.composition()->nodeLayout().at(a).collapsed &&
               f.session.composition()->nodeLayout().at(b).collapsed,
           "Collapse collapses the entire selection");
    expect(triggerNodeMenuAction(f, "nodeCollapseAction"), "and offers Expand next");
    expect(!f.session.composition()->nodeLayout().at(b).collapsed, "Expand expands selected cards");
    QSignalSpy refused(&f.session, &CompositionSession::commandRejected);
    // A multi-selection has no Dissolve to reach at all: the menu is the only route to it and the
    // menu only offers operations the command layer accepts for the current selection.
    {
        std::unique_ptr<QMenu> multiMenu(f.editor.contextMenuForTest(true));
        expect(named(multiMenu.get(), "nodeDissolveAction") == nullptr,
               "a multi-selection is offered no Dissolve to refuse");
    }
    // And the keys these used to own are bound by nothing.
    const auto beforeRetired = f.stack.size();
    for (const auto retired : {Qt::Key_M, Qt::Key_H, Qt::Key_X})
        f.key(retired);
    f.key(Qt::Key_X, Qt::ControlModifier);
    f.key(Qt::Key_D, Qt::ShiftModifier);
    f.key(Qt::Key_A, Qt::ShiftModifier);
    expect(f.stack.size() == beforeRetired && refused.empty() &&
               f.editor.findChild<kit::KSearchPopup*>() == nullptr,
           "every retired node key is bound by nothing: no command, no refusal, no popup");
    f.session.selectNode(a);
    history = f.stack.size();
    f.key(Qt::Key_D, Qt::ControlModifier);
    const auto* selected = f.session.selectedNode();
    expect(selected && selected->id != a && f.scene()->gestureActive() &&
               f.stack.size() == history + 1,
           "Ctrl D duplicates, selects the new IDs and starts a floating move");
    if (!selected)
        return;
    const auto copy = selected->id;
    expect(f.session.composition()->nodeLayout().at(copy).position == document::Vec2d{124, 124},
           "duplicate offset is exactly 24px");
    const auto duplicateOrigin =
        f.view()->sceneFromViewport(f.view()->viewport()->mapFromGlobal(QCursor::pos()));
    f.move(duplicateOrigin + QPointF(30, 40), Qt::NoButton);
    expect(f.card(copy)->pos() == QPointF(154, 164) && f.stack.size() == history + 1,
           "floating duplicate follows mouse movement without a held button or per-pixel command");
    f.key(Qt::Key_Escape);
    expect(f.card(copy)->pos() == QPointF(124, 124),
           "cancelling duplicate placement restores its 24px offset");
    expect(!f.scene()->gestureActive(),
           "Escape stops duplicate placement while retaining the undoable copies");
    f.key(Qt::Key_Backspace);
    expect(!f.session.composition()->graph().findNode(copy), "Backspace removes selected copies");
    expect(f.session.undo() && f.session.composition()->graph().findNode(copy),
           "the removal undoes once");
    f.session.selectNode(copy);
    f.key(Qt::Key_Delete);
    expect(!f.session.composition()->graph().findNode(copy), "Delete aliases Backspace");
    f.session.selectNode(a);
    auto* menu = f.editor.contextMenuForTest(true);
    expect(named(menu, "nodeDuplicateAction") && named(menu, "nodeDeleteAction") &&
               !named(menu, "nodeDissolveAction") && named(menu, "nodeMuteAction") &&
               named(menu, "nodeCollapseAction") && !named(menu, "nodeRenameAction"),
           "source node menu shows exactly the commands accepted for its selection");
    expect(!named(menu, "nodeDissolveAction"),
           "a source with no Image input is offered no Dissolve -- the menu is its only route");
    delete menu;
    const auto stackId = f.session.composition()->graph().layerStack().nodeId();
    f.session.selectNode(stackId);
    history = f.stack.size();
    const auto refusalCount = refused.size();
    f.key(Qt::Key_Delete);
    expect(f.stack.size() == history && refused.size() == refusalCount + 1,
           "the protected remove keyboard path reports refusal without mutation");
    menu = f.editor.contextMenuForTest(true);
    expect(!named(menu, "nodeDeleteAction") && !named(menu, "nodeDissolveAction"),
           "protected node menu omits removal and dissolve");
    delete menu;
    expect(f.edit<commands::ConnectPorts>(document::OutputPortRef{a, "image"},
                                          document::NodeInputRef{b, "image"})
               .changed(),
           "dissolve fixture wire");
    f.session.selectNode(b);
    menu = f.editor.contextMenuForTest(true);
    expect(named(menu, "nodeDissolveAction") != nullptr,
           "connected graph-only Image pair offers dissolve");
    delete menu;
    expect(triggerNodeMenuAction(f, "nodeDissolveAction") &&
               !f.session.composition()->graph().findNode(b),
           "Dissolve dissolves a valid single node");
    f.key(Qt::Key_A, Qt::ControlModifier);
    expect(f.session.selectedNodes().size() == f.session.composition()->graph().nodes().size(),
           "Ctrl A selects every graph node through session");
    f.view()->zoomStep(2);
    f.key(Qt::Key_Home);
    const auto fit = f.view()->transform();
    f.view()->zoomStep(1);
    f.key(Qt::Key_0, Qt::ControlModifier);
    expect(f.view()->transform() == fit && !f.view()->viewAdjusted(), "Home aliases Ctrl+0 Fit");
    f.key(Qt::Key_1, Qt::ControlModifier);
    expect(f.view()->zoomFactor() == 1, "Ctrl+1 is actual size");
    int stolen = 0;
    QShortcut windowDelete(QKeySequence(Qt::Key_Delete), &f.editor);
    QObject::connect(&windowDelete, &QShortcut::activated, &f.editor, [&] { ++stolen; });
    f.session.selectNode(stackId);
    f.key(Qt::Key_Delete);
    expect(stolen == 0, "canvas ShortcutOverride prevents the window from stealing its command");
    f.session.clearSelection();
    const auto cursor = QPointF(600, 500);
    const auto global = f.view()->viewport()->mapToGlobal(f.view()->mapFromScene(cursor));
    const auto cursorScene =
        f.view()->sceneFromViewport(f.view()->viewport()->mapFromGlobal(global));
    QCursor::setPos(global);
    f.key(Qt::Key_Tab);
    auto* popup = search(f);
    auto* field = popup->findChild<QLineEdit*>(QStringLiteral("kSearchFilter"));
    auto* list = popup->findChild<QListView*>();
    expect(popup->isVisible() &&
               resultRows(list->model()) ==
                   static_cast<int>(document::builtInNodeDefinitions().definitions().size()),
           "Tab opens all registered node kinds at the cursor");
    // Task S1, item 4: the list is sectioned, in the pipeline's own reading order, and carries a
    // heading only for a section that actually has results under it.
    expect(sectionHeadings(list->model()) ==
               QStringList{QStringLiteral("Sources"), QStringLiteral("Layers"),
                           QStringLiteral("Compositing"), QStringLiteral("Output")},
           "every populated section is headed, in category order, and the empty ones are absent");
    expect(list->model()->index(0, 0).data(kit::kSearchSectionRole).toBool() &&
               list->model()->index(0, 0).flags() == Qt::NoItemFlags,
           "a heading is not a row the artist can reach");
    // Exactly the rows and headings, with no dead surface under the last one.
    int expectedHeight = 0;
    for (int row = 0; row < list->model()->rowCount(); ++row)
        expectedHeight += list->model()->index(row, 0).data(Qt::SizeHintRole).toSize().height();
    expect(list->height() == expectedHeight,
           "the list is exactly as tall as the rows and headings it holds");
    // The popup's corners come from the shared dropdown surface alone: the list sits below the
    // filter field, so its own top edge must stay square rather than carving notches under it.
    const QRegion mask = list->mask();
    expect(mask.contains(QPoint(0, 0)) && mask.contains(QPoint(list->width() - 1, 0)),
           "the list is not rounded again where it meets the filter field");
    // Task S1, item 5: the composition's one evaluation endpoint and its one Layer Stack are
    // already present, so search offers them as refusals rather than as commands.
    for (const auto typeId :
         {document::kCompositionOutputNodeType, document::kLayerStackNodeType}) {
        const auto row = rowForKey(list->model(), typeId);
        expect(row.isValid() && !row.flags().testFlag(Qt::ItemIsEnabled) &&
                   row.data(Qt::ToolTipRole).toString().contains(QStringLiteral("Only one")),
               "a one-per-composition kind is listed disabled, with the command's own refusal");
    }
    expect(rowForKey(list->model(), document::kLayerOutputNodeType)
               .flags()
               .testFlag(Qt::ItemIsEnabled),
           "while a kind a composition may hold many of stays addable");
    field->setText(QStringLiteral("text"));
    const auto textRow = rowForKey(list->model(), document::kTextSourceNodeType);
    expect(resultRows(list->model()) == 1 && textRow.isValid() &&
               !textRow.flags().testFlag(Qt::ItemIsEnabled) &&
               textRow.data(Qt::ToolTipRole).toString().contains(QStringLiteral("CPU")),
           "text remains listed with actual command refusal as a disabled row");
    // Item 4: the reason is in the tooltip and NOWHERE else. A refusal appended to the label would
    // make the list's widest row an error message and read as part of the node's name.
    expect(textRow.data().toString() ==
                   node_editor::nodeTypeDisplayName(document::kTextSourceNodeType) &&
               !textRow.data().toString().contains(QStringLiteral("CPU")),
           "and its label is the node's name alone");
    expect(sectionHeadings(list->model()) == QStringList{QStringLiteral("Sources")},
           "a filter that empties a section drops that section's heading with it");
    history = f.stack.size();
    QTest::keyClick(field, Qt::Key_Return);
    expect(f.stack.size() == history && popup->isVisible(),
           "Enter cannot activate a disabled text result");
    field->setText(QStringLiteral("layer output image"));
    expect(resultRows(list->model()) == 1, "search combines display-name and socket-kind filters");
    QTest::keyClick(field, Qt::Key_Return);
    selected = f.session.selectedNode();
    expect(selected && selected->typeId == document::kLayerOutputNodeType &&
               f.stack.size() == history + 1 &&
               f.session.composition()->nodeLayout().at(selected->id).position ==
                   document::Vec2d{cursorScene.x(), cursorScene.y()},
           "Enter adds the selected registry kind at the Tab cursor scene position");
    f.editor.openAddSearch({550, 450}, global, document::NodeInputRef{selected->id, "image"}, {});
    popup = search(f);
    field = popup->findChild<QLineEdit*>();
    field->setText(QStringLiteral("solid"));
    history = f.stack.size();
    QTest::keyClick(field, Qt::Key_Return);
    expect(f.stack.size() == history + 1 &&
               f.session.composition()->graph().layerOutputs().size() == 1,
           "armed search adds a structured solid layer and connects its compatible port in one "
           "transaction");
    const auto layerId = f.session.composition()->graph().layerOutputs().front().layerId;
    f.session.selectLayer(layerId);
    const auto boundary = f.session.selectedNode()->id;
    menu = f.editor.contextMenuForTest(true);
    expect(named(menu, "nodeRenameAction") && !named(menu, "nodeDissolveAction"),
           "participating Layer Output offers rename and refuses dissolve");
    if (auto* rename = named(menu, "nodeRenameAction"))
        rename->trigger();
    auto* renameField = qobject_cast<QLineEdit*>(
        f.scene()->nodeFieldForTest(boundary, QStringLiteral("nodeRenameEditor")));
    expect(renameField != nullptr, "Rename opens an inline header field");
    if (renameField) {
        renameField->setText(QStringLiteral("Renamed layer"));
        QTest::keyClick(renameField, Qt::Key_Return);
        expect(f.session.composition()->graph().layerOutputs().front().name == "Renamed layer",
               "header rename executes RenameLayer");
    }
    delete menu;
    f.editor.openAddSearch({500, 500}, global);
    popup = search(f);
    field = popup->findChild<QLineEdit*>();
    list = popup->findChild<QListView*>();
    field->clear();
    const auto initialRow = list->currentIndex().row();
    QTest::keyClick(field, Qt::Key_Down);
    expect(list->currentIndex().row() > initialRow,
           "search Down moves the active result while retaining typed filter focus");
    QTest::keyClick(field, Qt::Key_Up);
    expect(list->currentIndex().row() == initialRow, "search Up returns to the prior result");
    QTest::keyClick(field, Qt::Key_Escape);
    expect(!popup->isVisible(), "Escape closes Add search");
    f.editor.openAddSearch({520, 520}, global);
    popup = search(f);
    field = popup->findChild<QLineEdit*>();
    list = popup->findChild<QListView*>();
    // Solid rather than the composition output this case used before task S1: the composition
    // output is one per composition (item 5), so the open composition's existing one now makes its
    // row a refusal -- which the cardinality case above is what pins, and a disabled row is not
    // something a click can add.
    field->setText(QStringLiteral("solid"));
    history = f.stack.size();
    QTest::mouseClick(
        list->viewport(), Qt::LeftButton, Qt::NoModifier,
        list->visualRect(rowForKey(list->model(), document::kSolidSourceNodeType)).center());
    expect(f.stack.size() == history + 1 && !popup->isVisible(),
           "clicking a search result adds and closes the popup");
    f.session.clearSelection();
    const auto emptyRefusals = refused.size();
    f.key(Qt::Key_Delete);
    f.key(Qt::Key_Backspace);
    f.key(Qt::Key_D, Qt::ControlModifier);
    f.key(Qt::Key_Return);
    expect(refused.size() == emptyRefusals + 4,
           "removal, duplication and rename refuse an empty selection with status");
    {
        // Mute and collapse share that guard but are unreachable with an empty selection at all:
        // the node menu offers nothing when nothing is selected, and the menu is their only route.
        std::unique_ptr<QMenu> emptyMenu(f.editor.contextMenuForTest(true));
        expect(emptyMenu->actions().isEmpty(),
               "an empty selection has no node menu to reach mute or collapse through");
    }
    // A fixture-owned durable driver ID pins the N2 limitation; there is no driver record to copy.
    auto beforeDriver = f.document.snapshot();
    auto draft = f.document.draft(beforeDriver);
    const auto driver = draft.ids().allocateDriverBinding();
    const auto* source =
        draft.project().findComposition(f.session.compositionId())->graph().findNode(a);
    if (!driver || !source || source->parameters.empty())
        throw std::runtime_error("driver fixture");
    expect(draft.project()
               .findComposition(f.session.compositionId())
               ->parameters()
               .setSource(source->parameters.front().parameterId,
                          document::DriverBindingSource{*driver}),
           "driver source fixture");
    expect(f.document.commit(beforeDriver.revision(), std::move(draft)).committed(),
           "driver source publication");
    f.stack.clear();
    f.session.rebind(f.document, f.stack, f.session.compositionId());
    f.session.selectNode(a);
    const auto driverRefusals = refused.size();
    f.key(Qt::Key_D, Qt::ControlModifier);
    expect(f.stack.size() == 0 && refused.size() == driverRefusals + 1 &&
               !f.scene()->gestureActive(),
           "driven-parameter duplication refuses without copies or a dangling move gesture");
    menu = f.editor.contextMenuForTest(true);
    expect(!named(menu, "nodeDuplicateAction"), "driver duplication is absent from the node menu");
    delete menu;
    menu = f.editor.contextMenuForTest();
    expect(named(menu, "nodeAddSearchAction") && named(menu, "nodeSelectAllAction") &&
               named(menu, "nodeFitAction") && named(menu, "nodeActualSizeAction") &&
               named(menu, "nodeZoomInAction") && named(menu, "nodeZoomOutAction"),
           "canvas menu offers Add search, view actions and Select All");
    const auto aliasRevision = f.session.snapshot().revision();
    if (auto* solid = named(menu, "nodeAddSolidLayerAction"))
        solid->trigger();
    expect(f.session.snapshot().revision().value() == aliasRevision.value() + 1,
           "preserved Add action contracts capture the revision when their menu is constructed");
    delete menu;
}
} // namespace bloom::ui::test
