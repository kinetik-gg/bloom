// Task FIX2, deliverable 2: the owner's "state synchronization is not reliable yet", stated as one
// question asked after every edit -- do ALL the surfaces still say what the document says?
//
// agree() below is that one question. It re-derives every visible answer from the session snapshot
// and compares it against what each surface is currently showing: the node card's own cells and
// keyframe diamonds, the Properties rows and theirs, the timeline's rows and animated lanes, and
// the selection. The edits around it come from each surface in turn -- a card cell, a Properties
// cell, a timeline row's Blending, undo and redo, connecting and disconnecting a driver, toggling a
// keyframe, adding and removing a layer -- so a surface that keeps a stale cache, misses a
// snapshotChanged rebuild, or holds a parameter id that no longer exists is caught by the very next
// call rather than by a reader noticing something looks wrong.
#include "surface_harness.hpp"

#include <bloom/commands/node_operations.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/node_editor.hpp>

#include "node_editor_add.hpp"
#include "node_editor_items.hpp"

#include <array>
#include <cmath>
#include <set>
#include <string_view>

using namespace bloom;
using namespace bloom::ui;
using namespace bloom::ui::surface_test;

namespace {

[[nodiscard]] bool same(const double left, const double right) {
    return std::abs(left - right) < 1e-6;
}

[[nodiscard]] kit::KValueField* cardCell(const Surfaces& surfaces, const document::NodeId node,
                                         const QString& name) {
    return qobject_cast<kit::KValueField*>(
        surfaces.nodes->graphScene()->nodeFieldForTest(node, name));
}

[[nodiscard]] kit::KValueField* panelCell(const Surfaces& surfaces, const QString& name) {
    return surfaces.properties->findChild<kit::KValueField*>(name);
}

// Each Properties row is [label, diamond, value]; a paired X/Y row wraps its two cells in a group,
// so its row widget is one level further up than a single cell's.
[[nodiscard]] KeyframeDiamond* panelDiamond(QWidget* field, const bool paired) {
    if (field == nullptr) {
        return nullptr;
    }
    auto* row = paired ? field->parentWidget()->parentWidget() : field->parentWidget();
    return row == nullptr
               ? nullptr
               : row->findChild<KeyframeDiamond*>(QStringLiteral("propertiesKeyframeIndicator"),
                                                  Qt::FindDirectChildrenOnly);
}

[[nodiscard]] const document::ParameterRecord*
parameterOf(const Surfaces& surfaces, const document::NodeId node, const std::string_view role) {
    const auto* record = surfaces.session.composition()->graph().findNode(node);
    if (record == nullptr) {
        return nullptr;
    }
    for (const auto& binding : record->parameters) {
        if (binding.role == role) {
            return surfaces.session.composition()->parameters().find(binding.parameterId);
        }
    }
    return nullptr;
}

// The card's own diamond for `role`, found by the parameter it is bound to rather than by row
// order: a card names every diamond "nodeKeyframeDiamond".
[[nodiscard]] KeyframeDiamond* cardDiamond(const Surfaces& surfaces, const document::NodeId node,
                                           const std::string_view role) {
    const auto* parameter = parameterOf(surfaces, node, role);
    auto* card =
        dynamic_cast<node_editor::NodeItem*>(surfaces.nodes->graphScene()->findNodeItem(node));
    if (parameter == nullptr || card == nullptr) {
        return nullptr;
    }
    for (const auto* child : card->childItems()) {
        const auto* proxy = qgraphicsitem_cast<const QGraphicsProxyWidget*>(child);
        auto* diamond =
            proxy == nullptr ? nullptr : qobject_cast<KeyframeDiamond*>(proxy->widget());
        if (diamond != nullptr && diamond->parameterId() == parameter->id) {
            return diamond;
        }
    }
    return nullptr;
}

struct Check final {
    const Surfaces& surfaces;
    std::string where;

    void say(const bool condition, const std::string& what) const {
        expect(condition, where + ": " + what);
    }
};

// THE question. Every answer below is re-derived from the session, never remembered from the edit
// that produced it.
void agree(const Surfaces& surfaces, const std::string& where) {
    const Check check{surfaces, where};
    const auto& session = surfaces.session;
    const auto* composition = session.composition();
    check.say(composition != nullptr, "the session still projects a composition");
    if (composition == nullptr) {
        return;
    }

    // --- Selection -----------------------------------------------------------------------------
    std::set<document::NodeId> sceneSelection;
    for (auto* item : surfaces.nodes->graphScene()->selectedItems()) {
        if (auto* card = dynamic_cast<node_editor::NodeItem*>(item)) {
            sceneSelection.insert(card->id());
        }
    }
    check.say(sceneSelection == session.selectedNodes(),
              "the canvas selects exactly the nodes the session says are selected");
    for (const auto id : session.selectedNodes()) {
        check.say(composition->graph().findNode(id) != nullptr,
                  "no selected node outlives the document");
    }

    // --- The timeline's layer column ------------------------------------------------------------
    auto* stack = surfaces.layerStack();
    check.say(stack != nullptr, "the timeline column exists");
    if (stack == nullptr) {
        return;
    }
    check.say(stack->rowCount() ==
                  static_cast<int>(composition->graph().layerStack().entries().size()),
              "the column draws one row per layer slot");
    const auto selectedLayer = [&]() -> std::optional<document::LayerId> {
        if (const auto* direct = std::get_if<document::LayerId>(&session.selection().primary)) {
            return *direct;
        }
        return session.selection().contextualLayer;
    }();
    int expectedRow = -1;
    for (int index = 0; index < stack->rowCount(); ++index) {
        const auto& entry = stack->entries()[static_cast<std::size_t>(index)];
        if (selectedLayer.has_value() && entry.layerId == *selectedLayer) {
            expectedRow = index;
        }
        auto* dropdown = surfaces.blendingDropdown(index);
        if (dropdown == nullptr) {
            continue;
        }
        const auto mode = session.blendModeForLayer(entry.layerId);
        check.say(dropdown->isEnabled() == mode.has_value(),
                  "a row's Blending is live exactly when its layer exposes one");
        if (mode.has_value()) {
            check.say(dropdown->itemData(dropdown->currentIndex()).value<std::int64_t>() ==
                          core::blendModeStoredValue(*mode),
                      "the row shows the blend mode its own layer holds");
        }
    }
    check.say(stack->currentRow() == expectedRow,
              "the column's current row is the selection's layer, or none");

    // --- The animated lanes ---------------------------------------------------------------------
    std::size_t animatedCurves = 0;
    if (selectedLayer.has_value()) {
        for (const auto nodeId : {session.boundaryNodeForLayer(*selectedLayer),
                                  session.directSourceNodeForLayer(*selectedLayer)}) {
            const auto* record =
                nodeId.has_value() ? composition->graph().findNode(*nodeId) : nullptr;
            if (record == nullptr) {
                continue;
            }
            for (const auto& binding : record->parameters) {
                const auto* parameter = composition->parameters().find(binding.parameterId);
                if (parameter != nullptr &&
                    std::holds_alternative<document::AnimationCurveSource>(parameter->source)) {
                    ++animatedCurves;
                }
            }
        }
    }
    auto* keyframePanel = surfaces.timeline->findChild<TimelineKeyframePanel*>();
    const auto laneRows =
        keyframePanel == nullptr
            ? std::size_t{0}
            : static_cast<std::size_t>(
                  keyframePanel->findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly)
                      .size());
    check.say(laneRows == animatedCurves,
              "the keyframe panel carries one lane per animated curve of the contextual layer");

    // --- The canvas's own inventory --------------------------------------------------------------
    std::size_t cards = 0;
    for (auto* item : surfaces.nodes->graphScene()->items()) {
        if (dynamic_cast<node_editor::NodeItem*>(item) != nullptr) {
            ++cards;
        }
    }
    check.say(cards == composition->graph().nodes().size(),
              "the canvas draws one card per node in the document, and no more");

    // --- Which half of the Properties panel is showing -------------------------------------------
    const bool hasSelection = !std::holds_alternative<std::monostate>(session.selection().primary);
    auto* selectionSection =
        surfaces.properties->findChild<QWidget*>(QStringLiteral("propertiesSelectionSection"));
    auto* documentSection =
        surfaces.properties->findChild<QWidget*>(QStringLiteral("propertiesDocumentSection"));
    check.say(selectionSection != nullptr && documentSection != nullptr,
              "the panel carries both of its sections");
    if (selectionSection != nullptr && documentSection != nullptr) {
        check.say(selectionSection->isVisibleTo(surfaces.properties) == hasSelection,
                  "the panel shows its selection rows exactly while something is selected");
        check.say(documentSection->isVisibleTo(surfaces.properties) != hasSelection,
                  "and the composition's own rows otherwise");
    }

    // --- The selection's own parameter rows, in both surfaces -----------------------------------
    const auto* selectedNode = session.selectedNode();
    if (selectedNode == nullptr) {
        return;
    }
    const auto nodeId = selectedNode->id;

    struct Row final {
        std::string_view role;
        QString cardName;
        QString panelName;
        bool paired;
        double displayScale;
        bool vector;
        int component;
    };
    const std::array<Row, 7> rows{
        Row{document::kPositionParameterRole, QStringLiteral("nodePositionXEditor"),
            QStringLiteral("positionXEditor"), true, 1.0, true, 0},
        Row{document::kPositionParameterRole, QStringLiteral("nodePositionYEditor"),
            QStringLiteral("positionYEditor"), true, 1.0, true, 1},
        Row{document::kAnchorParameterRole, QStringLiteral("nodeAnchorXEditor"),
            QStringLiteral("anchorXEditor"), true, 1.0, true, 0},
        Row{document::kScaleParameterRole, QStringLiteral("nodeScaleXEditor"),
            QStringLiteral("scaleXEditor"), true, 100.0, true, 0},
        Row{document::kRotationParameterRole, QStringLiteral("nodeRotationEditor"),
            QStringLiteral("rotationEditor"), false, 1.0, false, 0},
        Row{document::kOpacityParameterRole, QStringLiteral("nodeOpacityEditor"),
            QStringLiteral("opacityEditor"), false, 100.0, false, 0},
        Row{document::kTextSizeParameterRole, QStringLiteral("nodeTextSizeEditor"),
            QStringLiteral("textSizeEditor"), false, 1.0, false, 0},
    };

    for (const auto& row : rows) {
        const auto* parameter = parameterOf(surfaces, nodeId, row.role);
        auto* card = cardCell(surfaces, nodeId, row.cardName);
        auto* panel = panelCell(surfaces, row.panelName);
        if (parameter == nullptr) {
            check.say(card == nullptr, "a card builds no cell for a parameter its node lacks");
            continue;
        }
        const auto value = [&]() -> std::optional<double> {
            if (!row.vector) {
                return session.effectiveScalarValue(parameter->id);
            }
            const auto pair = session.effectiveVec2Value(parameter->id);
            if (!pair.has_value()) {
                return std::nullopt;
            }
            return row.component == 0 ? pair->x : pair->y;
        }();
        // A driven row shows its socket and nothing else; an unlinked one shows its control. The
        // card decides that by ROLE on every refresh, so a connect or a disconnect that did not
        // reach it leaves a control the artist can type into over a value the graph owns.
        const bool driven =
            std::holds_alternative<document::DriverBindingSource>(parameter->source);
        if (card != nullptr) {
            const auto* proxy = card->graphicsProxyWidget();
            check.say(proxy != nullptr && proxy->isVisible() != driven,
                      "a card's control is shown exactly while its input is unlinked");
        }
        if (card != nullptr) {
            check.say(card->isEnabled() == value.has_value(),
                      "a card cell is live exactly when its parameter has a readable value");
            if (value.has_value()) {
                check.say(same(card->value(), *value * row.displayScale),
                          "the card shows the document's value");
            }
        }
        if (panel != nullptr && panel->isVisible()) {
            check.say(panel->isEnabled() == value.has_value(),
                      "a Properties cell is live exactly when its parameter has a readable value");
            if (value.has_value()) {
                check.say(same(panel->value(), *value * row.displayScale),
                          "Properties shows the document's value");
            }
        }
        // The diamonds: both surfaces ask the session, so both must land on its answer.
        const auto expectedState = session.keyframeDiamondStateForParameter(parameter->id);
        if (auto* diamond = panelDiamond(panel, row.paired); diamond != nullptr) {
            check.say(diamond->state() == expectedState,
                      "the Properties diamond paints the session's own keyframe state");
        }
        if (auto* diamond = cardDiamond(surfaces, nodeId, row.role); diamond != nullptr) {
            check.say(diamond->state() == expectedState,
                      "the card diamond paints the same keyframe state");
        }
    }
}

} // namespace

namespace {

int run(int argc, char** argv) {
    QApplication application(argc, argv);
    Surfaces surfaces;
    auto& session = surfaces.session;

    expect(addDefaultSolidLayer(session), "the fixture adds its first layer");
    expect(addDefaultSolidLayer(session), "the fixture adds its second layer");
    QCoreApplication::processEvents();
    agree(surfaces, "after two layers exist");

    const auto* const layer = std::get_if<document::LayerId>(&session.selection().primary);
    expect(layer != nullptr, "the second layer is selected");
    if (layer == nullptr) {
        return 1;
    }
    const auto boundary = session.boundaryNodeForLayer(*layer);
    const auto source = session.directSourceNodeForLayer(*layer);
    expect(boundary.has_value() && source.has_value(), "the layer resolves both of its nodes");
    if (!boundary.has_value() || !source.has_value()) {
        return 1;
    }
    session.selectNode(*boundary);
    QCoreApplication::processEvents();
    agree(surfaces, "after selecting the layer's boundary card");

    // --- An edit from the node card --------------------------------------------------------------
    if (auto* cell = cardCell(surfaces, *boundary, QStringLiteral("nodePositionXEditor"))) {
        cell->setValue(120.0);
        QCoreApplication::processEvents();
        agree(surfaces, "after a node card edit");
    }

    // --- An edit from the Properties panel
    // --------------------------------------------------------
    if (auto* cell = panelCell(surfaces, QStringLiteral("opacityEditor"))) {
        cell->setValue(40.0);
        QCoreApplication::processEvents();
        agree(surfaces, "after a Properties edit");
    }

    // --- An edit from the timeline row
    // --------------------------------------------------------------
    if (auto* dropdown = surfaces.blendingDropdown(0)) {
        click(dropdown);
        pickPopupRow(*dropdown, dropdown->currentIndex() == 0 ? 1 : 0);
        agree(surfaces, "after a timeline row edit");
    }

    // --- Undo and redo -------------------------------------------------------------------------
    expect(session.undo(), "undo runs");
    QCoreApplication::processEvents();
    agree(surfaces, "after undo");
    expect(session.redo(), "redo runs");
    QCoreApplication::processEvents();
    agree(surfaces, "after redo");

    // --- A keyframe toggle ---------------------------------------------------------------------
    expect(session.toggleKeyframe(document::kOpacityParameterRole), "opacity becomes animated");
    QCoreApplication::processEvents();
    agree(surfaces, "after a keyframe toggle");
    expect(session.setCurrentTime(core::RationalTime::fromInteger(3)), "the time moves");
    QCoreApplication::processEvents();
    agree(surfaces, "after moving to a time with no key");
    expect(session.toggleKeyframe(document::kOpacityParameterRole), "a second key is added");
    QCoreApplication::processEvents();
    agree(surfaces, "after a second key");

    // Two keys with DIFFERENT values, so "the surfaces follow the playhead" is a question with an
    // observable answer rather than one both times happen to agree on.
    expect(session.setSelectedOpacity(0.9), "the key at this time takes another value");
    QCoreApplication::processEvents();
    agree(surfaces, "after changing the value at the second key");
    auto* opacityCell = cardCell(surfaces, *boundary, QStringLiteral("nodeOpacityEditor"));
    auto* opacityRow = panelCell(surfaces, QStringLiteral("opacityEditor"));
    expect(opacityCell != nullptr && opacityRow != nullptr, "both opacity rows resolve");
    const double atSecondKey = opacityCell == nullptr ? 0.0 : opacityCell->value();
    expect(session.setCurrentTime(core::RationalTime::fromInteger(0)),
           "the playhead moves back to the first key");
    QCoreApplication::processEvents();
    agree(surfaces, "after moving the playhead back");
    expect(opacityCell != nullptr && !same(opacityCell->value(), atSecondKey),
           "the card's animated cell followed the playhead to another key's value");
    expect(opacityRow != nullptr && opacityCell != nullptr &&
               same(opacityRow->value(), opacityCell->value()),
           "and the Properties row followed it to the same value");

    // --- Connecting and disconnecting a driver -------------------------------------------------
    commands::Transaction add("Add Scalar", session.snapshot().revision());
    add.emplace<node_editor::AddEditorNode>(session.compositionId(),
                                            std::string(document::kScalarValueNodeType),
                                            document::Vec2d{-900.0, 600.0});
    const auto added = session.executeNodeTransaction(std::move(add));
    const auto scalar = added.outputId<document::NodeId>("editorNode");
    expect(scalar.has_value(), "a Scalar value node is added");
    QCoreApplication::processEvents();
    agree(surfaces, "after adding a value node");
    if (scalar.has_value()) {
        session.selectNode(*boundary);
        QCoreApplication::processEvents();
        commands::Transaction connect("Connect", session.snapshot().revision());
        connect.emplace<commands::ConnectPorts>(
            session.compositionId(),
            document::OutputPortRef{*scalar, std::string(document::kValuePortName)},
            document::NodeInputRef{*boundary, std::string(document::kRotationParameterRole)});
        expect(session.executeNodeTransaction(std::move(connect)).succeeded(),
               "the driver connects");
        QCoreApplication::processEvents();
        agree(surfaces, "after connecting a driver");

        commands::Transaction disconnect("Disconnect", session.snapshot().revision());
        disconnect.emplace<commands::DisconnectInput>(
            session.compositionId(),
            document::NodeInputRef{*boundary, std::string(document::kRotationParameterRole)});
        expect(session.executeNodeTransaction(std::move(disconnect)).succeeded(),
               "the driver disconnects");
        QCoreApplication::processEvents();
        agree(surfaces, "after disconnecting a driver");
    }

    // --- Adding and removing a layer -----------------------------------------------------------
    for (int extra = 0; extra < 8; ++extra) {
        expect(addDefaultSolidLayer(session), "more layers are added");
    }
    QCoreApplication::processEvents();
    session.selectNode(*boundary);
    QCoreApplication::processEvents();
    agree(surfaces, "after adding layers");

    // A scrolled column re-points its pooled rows at other layers; every row must still draw the
    // layer it is now bound to.
    if (auto* scrollBar = surfaces.timeline->verticalScrollBarForTest();
        scrollBar != nullptr && scrollBar->maximum() > 0) {
        scrollBar->setValue(scrollBar->maximum());
        QCoreApplication::processEvents();
        agree(surfaces, "after scrolling the layer column");
        scrollBar->setValue(0);
        QCoreApplication::processEvents();
        agree(surfaces, "after scrolling back");
    }

    // Remove the layer whose card is SELECTED, which is what makes selection normalization the
    // question: every surface has to stop pointing at it in the same breath.
    session.selectNode(*boundary);
    QCoreApplication::processEvents();
    commands::Transaction remove("Remove Nodes", session.snapshot().revision());
    remove.emplace<commands::RemoveNodes>(session.compositionId(),
                                          std::set<document::NodeId>{*boundary});
    expect(session.executeNodeTransaction(std::move(remove)).succeeded(), "the layer is removed");
    QCoreApplication::processEvents();
    agree(surfaces, "after removing the selected layer");

    expect(session.undo(), "the removal undoes");
    QCoreApplication::processEvents();
    agree(surfaces, "after undoing the removal");

    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
