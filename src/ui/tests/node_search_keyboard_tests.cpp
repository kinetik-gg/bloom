#include "node_interaction_test_support.hpp"
#include <QAbstractItemModel>
#include <QAction>
#include <QModelIndex>
#include <QShortcut>

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
kit::KSearchPopup* search(Fixture& f) {
    auto* popup = f.editor.findChild<kit::KSearchPopup*>();
    if (!popup)
        throw std::runtime_error("search popup missing");
    return popup;
}
} // namespace
void testSearchKeyboardAndMenus() {
    Fixture f;
    const auto a = f.add(document::kSolidSourceNodeType, {100, 100});
    const auto b = f.add(document::kLayerOutputNodeType, {400, 100});
    f.session.selectNodes({a, b}, b);
    auto history = f.stack.size();
    f.key(Qt::Key_M);
    expect(f.session.composition()->nodeLayout().at(a).muted &&
               f.session.composition()->nodeLayout().at(b).muted && f.stack.size() == history + 1,
           "M mutes the entire selection in one command transaction");
    f.key(Qt::Key_M);
    expect(!f.session.composition()->nodeLayout().at(a).muted,
           "M toggles selection back to unmuted");
    f.key(Qt::Key_H);
    expect(f.session.composition()->nodeLayout().at(a).collapsed &&
               f.session.composition()->nodeLayout().at(b).collapsed,
           "H collapses the entire selection");
    f.key(Qt::Key_H);
    expect(!f.session.composition()->nodeLayout().at(b).collapsed, "H expands selected cards");
    QSignalSpy refused(&f.session, &CompositionSession::commandRejected);
    f.key(Qt::Key_X, Qt::ControlModifier);
    expect(!refused.empty(), "multi-selection dissolve refuses with status");
    f.session.selectNode(a);
    history = f.stack.size();
    f.key(Qt::Key_D, Qt::ShiftModifier);
    const auto* selected = f.session.selectedNode();
    expect(selected && selected->id != a && f.scene()->gestureActive() &&
               f.stack.size() == history + 1,
           "Shift D duplicates, selects the new IDs and starts a floating move");
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
    f.key(Qt::Key_X);
    expect(!f.session.composition()->graph().findNode(copy), "X removes selected copies");
    expect(f.session.undo() && f.session.composition()->graph().findNode(copy),
           "X removal undoes once");
    f.session.selectNode(copy);
    f.key(Qt::Key_Delete);
    expect(!f.session.composition()->graph().findNode(copy), "Delete aliases remove selection");
    f.session.selectNode(a);
    f.key(Qt::Key_X, Qt::ControlModifier);
    expect(!refused.empty() && f.session.composition()->graph().findNode(a),
           "source without Image input refuses dissolve");
    auto* menu = f.editor.contextMenuForTest(true);
    expect(named(menu, "nodeDuplicateAction") && named(menu, "nodeDeleteAction") &&
               !named(menu, "nodeDissolveAction") && named(menu, "nodeMuteAction") &&
               named(menu, "nodeCollapseAction") && !named(menu, "nodeRenameAction"),
           "source node menu shows exactly the commands accepted for its selection");
    delete menu;
    const auto stackId = f.session.composition()->graph().layerStack().nodeId();
    f.session.selectNode(stackId);
    history = f.stack.size();
    const auto refusalCount = refused.size();
    f.key(Qt::Key_Delete);
    f.key(Qt::Key_X, Qt::ControlModifier);
    expect(f.stack.size() == history && refused.size() == refusalCount + 2,
           "protected remove/dissolve keyboard paths report refusal without mutation");
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
    f.key(Qt::Key_X, Qt::ControlModifier);
    expect(!f.session.composition()->graph().findNode(b), "Ctrl X dissolves a valid single node");
    f.key(Qt::Key_A, Qt::ControlModifier);
    expect(f.session.selectedNodes().size() == f.session.composition()->graph().nodes().size(),
           "Ctrl A selects every graph node through session");
    f.view()->zoomStep(2);
    f.key(Qt::Key_Home);
    const auto fit = f.view()->transform();
    f.view()->zoomStep(1);
    f.key(Qt::Key_F);
    expect(f.view()->transform() == fit && !f.view()->viewAdjusted(), "Home aliases Fit F");
    f.key(Qt::Key_Z);
    expect(f.view()->zoomFactor() == 1, "Z remains actual size");
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
    f.key(Qt::Key_A, Qt::ShiftModifier);
    auto* popup = search(f);
    auto* field = popup->findChild<QLineEdit*>(QStringLiteral("kSearchFilter"));
    auto* list = popup->findChild<QListView*>();
    expect(popup->isVisible() &&
               list->model()->rowCount() ==
                   static_cast<int>(document::builtInNodeDefinitions().definitions().size()),
           "Shift A opens all registered node kinds at the cursor");
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
    expect(list->model()->rowCount() == 1 &&
               !list->model()->index(0, 0).flags().testFlag(Qt::ItemIsEnabled) &&
               list->model()->index(0, 0).data().toString().contains(QStringLiteral("CPU")),
           "text remains listed with actual command refusal as a disabled row");
    history = f.stack.size();
    QTest::keyClick(field, Qt::Key_Return);
    expect(f.stack.size() == history && popup->isVisible(),
           "Enter cannot activate a disabled text result");
    field->setText(QStringLiteral("layer output image"));
    expect(list->model()->rowCount() == 1, "search combines display-name and socket-kind filters");
    QTest::keyClick(field, Qt::Key_Return);
    selected = f.session.selectedNode();
    expect(selected && selected->typeId == document::kLayerOutputNodeType &&
               f.stack.size() == history + 1 &&
               f.session.composition()->nodeLayout().at(selected->id).position ==
                   document::Vec2d{cursorScene.x(), cursorScene.y()},
           "Enter adds the selected registry kind at the Shift A cursor scene position");
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
    for (const auto key : {Qt::Key_M, Qt::Key_H, Qt::Key_Delete})
        f.key(key);
    f.key(Qt::Key_D, Qt::ShiftModifier);
    expect(refused.size() == emptyRefusals + 4,
           "mute, collapse, removal and duplication refuse empty selection with status");
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
    f.key(Qt::Key_D, Qt::ShiftModifier);
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
