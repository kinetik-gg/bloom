// Task U4 (issue #123): the Kinetik node editor's navigation, selection, typed connectors,
// context menu and in-node editing contracts. Appended as its own executable rather than folded
// into composition_projection_test.cpp, which owns the cross-editor projection/selection contract
// this task must keep byte-equivalent.

#include <bloom/ui/node_editor.hpp>

#include <bloom/commands/command_stack.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QAction>
#include <QApplication>
#include <QGraphicsItem>
#include <QKeyEvent>
#include <QList>
#include <QMenu>
#include <QMouseEvent>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QWheelEvent>

#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <source_location>
#include <string>
#include <utility>
#include <variant>

namespace {

using namespace bloom;

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

[[nodiscard]] bool near(const double lhs, const double rhs, const double tolerance = 0.0001) {
    return std::abs(lhs - rhs) <= tolerance;
}

// One solid layer is enough to exercise every contract here: it produces a solid-source node (a
// color parameter), a layer-output boundary node (position and opacity parameters), the layer
// stack, and the composition output -- plus the edges between them.
struct GraphFixture final {
    document::Document document;
    commands::CommandStack commands;
    ui::CompositionSession session;
    ui::NodeGraphEditor editor;

    explicit GraphFixture(document::NewProject newProject)
        : document(std::move(newProject.project)), commands(document),
          session(document, commands, newProject.initialCompositionId), editor(session) {
        editor.resize(800, 600);
        editor.show();
        QCoreApplication::processEvents();
    }

    [[nodiscard]] ui::NodeGraphicsView* view() const { return editor.graphView(); }
    [[nodiscard]] ui::NodeGraphicsScene* scene() const { return editor.graphScene(); }
};

document::NewProject makeProject(std::string name) {
    return document::makeNewProject(std::move(name), "Main", core::RationalTime::fromInteger(10));
}

// Pointer events go to the VIEWPORT, which is where QAbstractScrollArea expects them and the only
// route by which QGraphicsView's own mouse/wheel handlers run (QAbstractScrollArea::event()
// deliberately ignores pointer events delivered to the scroll area itself). Key events go to the
// view, which is what holds focus. The canvas has no frame and no scroll bars, so viewport and view
// coordinates coincide.
void sendWheel(ui::NodeGraphicsView* view, const QPointF position, const int notches) {
    QWheelEvent event(position, view->viewport()->mapToGlobal(position.toPoint()), QPoint(),
                      QPoint(0, notches * 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                      false);
    QCoreApplication::sendEvent(view->viewport(), &event);
}

void sendKey(ui::NodeGraphicsView* view, const QEvent::Type type, const int key) {
    QKeyEvent event(type, key, Qt::NoModifier);
    QCoreApplication::sendEvent(view, &event);
}

void sendMouse(ui::NodeGraphicsView* view, const QEvent::Type type, const QPointF position,
               const Qt::MouseButton button, const Qt::MouseButtons buttons) {
    QMouseEvent event(type, position, view->viewport()->mapToGlobal(position.toPoint()), button,
                      buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(view->viewport(), &event);
}

[[nodiscard]] std::optional<document::LayerId> addSolid(ui::CompositionSession& session) {
    if (!session.addSolidLayer(QStringLiteral("Solid 1"), core::Color4d{0.62, 0.08, 0.04, 1.0})) {
        return std::nullopt;
    }
    const auto* layerId = std::get_if<document::LayerId>(&session.selection().primary);
    return layerId == nullptr ? std::nullopt : std::optional<document::LayerId>(*layerId);
}

const document::ParameterRecord* parameterForRole(const ui::CompositionSession& session,
                                                  const document::NodeId nodeId,
                                                  const std::string_view role) {
    const auto* composition = session.composition();
    const auto* node = composition == nullptr ? nullptr : composition->graph().findNode(nodeId);
    if (node == nullptr) {
        return nullptr;
    }
    for (const auto& binding : node->parameters) {
        if (binding.role == role) {
            return composition->parameters().find(binding.parameterId);
        }
    }
    return nullptr;
}

// --- Decision 1: navigation matches the Viewer's conventions -----------------------------------

// The invariant the whole zoom design exists to guarantee, stated exactly as the Viewer's own test
// states it: the DOCUMENT point under the cursor does not move across a zoom step.
void testZoomAboutCursorHoldsTheScenePointUnderTheCursor(Expectations& expectations) {
    GraphFixture fixture(makeProject("Node Zoom Cursor Test"));
    fixture.view()->zoomToActualSize();

    const QPointF cursor(517.0, 233.0); // arbitrary, and not the viewport's own center
    const QPointF before = fixture.view()->sceneFromViewport(cursor);
    sendWheel(fixture.view(), cursor, 1);
    const QPointF after = fixture.view()->sceneFromViewport(cursor);

    expectations.expect(near(fixture.view()->zoomFactor(), ui::kZoomStepFactor),
                        "one wheel notch multiplies the zoom by the Viewer's own step factor");
    expectations.expect(near(before.x(), after.x(), 0.0005) && near(before.y(), after.y(), 0.0005),
                        "the scene point under the cursor is fixed across the zoom step");

    // Stepping back by the same notch about the same cursor restores the original mapping -- the
    // stronger form of the same pin.
    sendWheel(fixture.view(), cursor, -1);
    const QPointF restored = fixture.view()->sceneFromViewport(cursor);
    expectations.expect(near(fixture.view()->zoomFactor(), 1.0, 0.0005) &&
                            near(before.x(), restored.x(), 0.01) &&
                            near(before.y(), restored.y(), 0.01),
                        "zooming in then out about the same cursor restores the view exactly");
}

void testZoomClampsToTheViewersOwnBounds(Expectations& expectations) {
    GraphFixture fixture(makeProject("Node Zoom Bounds Test"));
    const QPointF center = QRectF(fixture.view()->viewport()->rect()).center();

    for (int step = 0; step < 200; ++step) {
        sendWheel(fixture.view(), center, 1);
    }
    expectations.expect(near(fixture.view()->zoomFactor(), ui::ViewTransform::kMaxZoom),
                        "zooming in clamps at the Viewer's kMaxZoom, never above");

    for (int step = 0; step < 400; ++step) {
        sendWheel(fixture.view(), center, -1);
    }
    expectations.expect(near(fixture.view()->zoomFactor(), ui::ViewTransform::kMinZoom),
                        "zooming out clamps at the Viewer's kMinZoom, never below");
}

void testFitFramesTheGraphAndActualSizeIsExactlyOneHundredPercent(Expectations& expectations) {
    GraphFixture fixture(makeProject("Node Fit Test"));
    expectations.expect(addSolid(fixture.session).has_value(), "the fixture can add a solid layer");

    fixture.view()->frameGraph();
    const QRectF bounds = fixture.scene()->itemsBoundingRect();
    const QRectF framed = fixture.view()->viewportTransform().mapRect(bounds);
    const QRectF viewport = QRectF(fixture.view()->viewport()->rect());
    expectations.expect(framed.width() <= viewport.width() + 1.0 &&
                            framed.height() <= viewport.height() + 1.0,
                        "Fit scales the whole graph to fit inside the viewport");
    expectations.expect(near(framed.center().x(), viewport.center().x(), 0.5) &&
                            near(framed.center().y(), viewport.center().y(), 0.5),
                        "Fit centers the graph, exactly as the Viewer's Fit centers its frame");
    expectations.expect(!fixture.view()->viewAdjusted(),
                        "Fit leaves the canvas following the graph until the artist moves it");

    // Z, through the real key event, not the method.
    sendKey(fixture.view(), QEvent::KeyPress, Qt::Key_Z);
    expectations.expect(near(fixture.view()->zoomFactor(), 1.0),
                        "Z is exactly 100%, one scene unit per screen pixel");
    const QRectF actual = fixture.view()->viewportTransform().mapRect(bounds);
    expectations.expect(near(actual.center().x(), viewport.center().x(), 0.5) &&
                            near(actual.center().y(), viewport.center().y(), 0.5),
                        "100% centers the graph rather than jumping to its corner");
    expectations.expect(fixture.view()->viewAdjusted(),
                        "an explicit 100% counts as the artist having moved the view");

    // F, through the real key event.
    sendKey(fixture.view(), QEvent::KeyPress, Qt::Key_F);
    expectations.expect(!fixture.view()->viewAdjusted() && !near(fixture.view()->zoomFactor(), 1.0),
                        "F re-frames the graph");
}

void testSpaceHoldAndMiddleDragBothPan(Expectations& expectations) {
    GraphFixture fixture(makeProject("Node Pan Test"));
    fixture.view()->zoomToActualSize();

    const QPointF press(300.0, 220.0);
    const QPointF move(360.0, 190.0);
    const QPointF anchorBefore = fixture.view()->sceneFromViewport(press);

    sendKey(fixture.view(), QEvent::KeyPress, Qt::Key_Space);
    sendMouse(fixture.view(), QEvent::MouseButtonPress, press, Qt::LeftButton, Qt::LeftButton);
    sendMouse(fixture.view(), QEvent::MouseMove, move, Qt::NoButton, Qt::LeftButton);

    const QPointF anchorAfter = fixture.view()->sceneFromViewport(move);
    expectations.expect(near(anchorBefore.x(), anchorAfter.x(), 0.01) &&
                            near(anchorBefore.y(), anchorAfter.y(), 0.01),
                        "a space-hold left drag pans by the TOTAL displacement from the press "
                        "point: the grabbed scene point stays under the pointer");
    expectations.expect(near(fixture.view()->zoomFactor(), 1.0), "panning never changes the zoom");

    sendMouse(fixture.view(), QEvent::MouseButtonRelease, move, Qt::LeftButton, Qt::NoButton);
    sendKey(fixture.view(), QEvent::KeyRelease, Qt::Key_Space);
    const QPointF afterRelease = fixture.view()->sceneFromViewport(move);
    expectations.expect(near(afterRelease.x(), anchorAfter.x()) &&
                            near(afterRelease.y(), anchorAfter.y()),
                        "releasing ends the pan without resetting the accumulated pan");

    // The middle button pans with no modifier at all -- the Viewer's second pan entry point.
    const QPointF middlePress(200.0, 200.0);
    const QPointF middleMove(140.0, 260.0);
    const QPointF middleAnchor = fixture.view()->sceneFromViewport(middlePress);
    sendMouse(fixture.view(), QEvent::MouseButtonPress, middlePress, Qt::MiddleButton,
              Qt::MiddleButton);
    sendMouse(fixture.view(), QEvent::MouseMove, middleMove, Qt::NoButton, Qt::MiddleButton);
    const QPointF middleAfter = fixture.view()->sceneFromViewport(middleMove);
    expectations.expect(near(middleAnchor.x(), middleAfter.x(), 0.01) &&
                            near(middleAnchor.y(), middleAfter.y(), 0.01),
                        "a middle drag pans exactly like the space-hold drag");
    sendMouse(fixture.view(), QEvent::MouseButtonRelease, middleMove, Qt::MiddleButton,
              Qt::NoButton);
}

// --- Decision 2: selection stays the session's one truth ---------------------------------------

void testSelectionFollowsTheSessionInBothDirections(Expectations& expectations) {
    GraphFixture fixture(makeProject("Node Selection Test"));
    const auto layerId = addSolid(fixture.session);
    expectations.expect(layerId.has_value(), "the fixture can add a solid layer");
    if (!layerId.has_value()) {
        return;
    }
    const auto boundaryNodeId = fixture.session.boundaryNodeForLayer(*layerId);
    const auto sourceNodeId = fixture.session.directSourceNodeForLayer(*layerId);
    expectations.expect(boundaryNodeId.has_value() && sourceNodeId.has_value(),
                        "the solid layer resolves both its boundary and its source node");
    if (!boundaryNodeId.has_value() || !sourceNodeId.has_value()) {
        return;
    }

    // Scene -> session: selecting a card makes that NodeId the session's primary selection.
    auto* sourceItem = fixture.scene()->findNodeItem(*sourceNodeId);
    expectations.expect(sourceItem != nullptr, "the scene projects the solid source node");
    if (sourceItem == nullptr) {
        return;
    }
    fixture.scene()->clearSelection();
    sourceItem->setSelected(true);
    expectations.expect(fixture.session.selection().primary == ui::SelectionTarget{*sourceNodeId},
                        "selecting a card in the scene selects that node in the session");
    expectations.expect(fixture.session.selection().contextualLayer == layerId,
                        "and carries the node's own contextual layer, unchanged");

    // Session -> scene: selecting the LAYER elsewhere highlights its boundary card, and nothing
    // else stays selected.
    fixture.session.selectLayer(*layerId);
    auto* boundaryItem = fixture.scene()->findNodeItem(*boundaryNodeId);
    expectations.expect(boundaryItem != nullptr && boundaryItem->isSelected(),
                        "selecting the layer elsewhere highlights its boundary card");
    expectations.expect(!sourceItem->isSelected(),
                        "and the previously selected card is no longer highlighted");
    expectations.expect(fixture.scene()->selectedItems().size() == 1,
                        "exactly one card carries the selection edge -- there is no scene-local "
                        "second selection truth");

    fixture.session.clearSelection();
    expectations.expect(fixture.scene()->selectedItems().isEmpty(),
                        "clearing the session's selection clears the scene's");
}

// --- Decision 3: typed connectors, only for the types that exist -------------------------------

void testConnectorTypingIsPinnedPerSocketKind(Expectations& expectations) {
    // Image is the ONE transport kind runtime::SocketValueKind declares, and the only one any edge
    // in a document::CanonicalGraph can carry today (see socketColorToken()'s comment in
    // node_editor.hpp). Pinning the mapping here is what makes adding a second kind a deliberate
    // decision rather than an accident: socketColorToken()'s switch stops being exhaustive.
    expectations.expect(ui::socketColorToken(runtime::SocketValueKind::Image) ==
                            ui::kit::Color::DataImage,
                        "an Image socket takes the data palette's own Image token");
    expectations.expect(ui::kit::color(ui::socketColorToken(runtime::SocketValueKind::Image)) ==
                            ui::kit::color(ui::kit::Color::DataImage),
                        "and resolves to exactly that token's color, not a look-alike");
}

void testEveryProjectedEdgeIsAPathItemBetweenTwoCards(Expectations& expectations) {
    GraphFixture fixture(makeProject("Node Connector Test"));
    expectations.expect(addSolid(fixture.session).has_value(), "the fixture can add a solid layer");
    const auto* composition = fixture.session.composition();
    expectations.expect(composition != nullptr, "the fixture has a live composition");
    if (composition == nullptr) {
        return;
    }

    int cards = 0;
    int wires = 0;
    for (const auto* item : fixture.scene()->items()) {
        if (item->data(ui::kNodeItemKindRole).toString() == QStringLiteral("node")) {
            ++cards;
        } else if (item->type() == QGraphicsPathItem::Type) {
            ++wires;
        }
    }
    expectations.expect(
        cards == static_cast<int>(composition->graph().nodes().size()),
        "the scene projects exactly one card per graph node -- no more, and none invented");
    expectations.expect(wires == static_cast<int>(composition->graph().edges().size()),
                        "and exactly one wire per graph edge");
}

// A card in the middle of the chain is BOTH an edge source and an edge destination, so it carries
// both port dots. Sockets accumulate across the edges that touch a node; assigning both flags from
// each edge in turn would let whichever edge was visited last erase the other end.
void testMidChainCardsCarryBothPortDots(Expectations& expectations) {
    GraphFixture fixture(makeProject("Node Socket Test"));
    const auto layerId = addSolid(fixture.session);
    if (!layerId.has_value()) {
        expectations.expect(false, "the fixture can add a solid layer");
        return;
    }
    const auto boundaryNodeId = fixture.session.boundaryNodeForLayer(*layerId);
    const auto sourceNodeId = fixture.session.directSourceNodeForLayer(*layerId);
    const auto* composition = fixture.session.composition();
    if (!boundaryNodeId.has_value() || !sourceNodeId.has_value() || composition == nullptr) {
        expectations.expect(false, "the solid layer resolves its boundary and source nodes");
        return;
    }

    expectations.expect(fixture.scene()->nodeSocketsForTest(*sourceNodeId) ==
                            ui::NodeSockets{false, true},
                        "a source card carries only an output dot: nothing feeds into it");
    expectations.expect(fixture.scene()->nodeSocketsForTest(*boundaryNodeId) ==
                            ui::NodeSockets{true, true},
                        "the layer boundary card carries both: it consumes the source and feeds "
                        "the stack");
    expectations.expect(
        fixture.scene()->nodeSocketsForTest(composition->graph().layerStack().nodeId()) ==
            ui::NodeSockets{true, true},
        "and so does the layer stack");
}

// --- Decision 4: context menu offers only what the command layer can do -------------------------

void testContextMenuOffersOnlyRealCommands(Expectations& expectations) {
    GraphFixture fixture(makeProject("Node Menu Test"));
    auto* menu = fixture.editor.contextMenuForTest();
    expectations.expect(menu != nullptr, "the canvas builds a context menu");
    if (menu == nullptr) {
        return;
    }

    const auto actions = menu->findChildren<QAction*>();
    const auto named = [&actions](const QString& objectName) -> QAction* {
        for (auto* action : actions) {
            if (action->objectName() == objectName) {
                return action;
            }
        }
        return nullptr;
    };

    expectations.expect(named(QStringLiteral("nodeAddSolidLayerAction")) != nullptr &&
                            named(QStringLiteral("nodeAddTextLayerAction")) != nullptr,
                        "Add offers exactly the two layer kinds the command layer has: Solid and "
                        "Text -- the same pair the timeline's Add menu offers");
    expectations.expect(named(QStringLiteral("nodeFitAction")) != nullptr &&
                            named(QStringLiteral("nodeActualSizeAction")) != nullptr &&
                            named(QStringLiteral("nodeZoomInAction")) != nullptr &&
                            named(QStringLiteral("nodeZoomOutAction")) != nullptr,
                        "and the same canvas view items the Viewer's own menu offers: Fit, 100%, "
                        "Zoom In, Zoom Out");

    // The honesty rule, pinned: no delete-layer, remove-node or duplicate command exists anywhere
    // in src/commands, so no such item may appear -- not even a disabled one.
    for (const auto* action : actions) {
        const QString text = action->text();
        expectations.expect(!text.contains(QStringLiteral("Duplicate"), Qt::CaseInsensitive) &&
                                !text.contains(QStringLiteral("Delete"), Qt::CaseInsensitive) &&
                                !text.contains(QStringLiteral("Remove"), Qt::CaseInsensitive),
                            "no menu item claims an operation the command layer cannot perform: " +
                                text.toStdString());
    }
    menu->deleteLater();
}

void testAddFromTheCanvasIsOneUndoableCommand(Expectations& expectations) {
    GraphFixture fixture(makeProject("Node Menu Add Test"));
    auto* menu = fixture.editor.contextMenuForTest();
    if (menu == nullptr) {
        expectations.expect(false, "the canvas builds a context menu");
        return;
    }
    QAction* addSolidAction = nullptr;
    for (auto* action : menu->findChildren<QAction*>()) {
        if (action->objectName() == QStringLiteral("nodeAddSolidLayerAction")) {
            addSolidAction = action;
        }
    }
    if (addSolidAction == nullptr) {
        expectations.expect(false, "the canvas menu offers Add > Solid");
        return;
    }

    const auto nodesBefore = fixture.session.composition()->graph().nodes().size();
    addSolidAction->trigger();
    expectations.expect(fixture.session.composition()->graph().layerStack().entries().size() == 1,
                        "Add > Solid on the canvas creates one layer through the command layer");
    expectations.expect(fixture.session.composition()->graph().nodes().size() > nodesBefore,
                        "and the projection grows with the new nodes");
    expectations.expect(fixture.session.undoLabel() == QStringLiteral("Add Solid Layer"),
                        "through the exact command the timeline's own Add menu uses");

    expectations.expect(fixture.session.undo(), "the canvas Add is undoable");
    expectations.expect(fixture.session.composition()->graph().layerStack().entries().empty() &&
                            fixture.session.composition()->graph().nodes().size() == nodesBefore,
                        "ONE undo step removes the whole layer the gesture created");
    menu->deleteLater();
}

// --- Decision 5: in-node edits take the Properties panel's own path -----------------------------

void testInNodeValueFieldsCommitThroughThePropertiesPath(Expectations& expectations) {
    GraphFixture fixture(makeProject("Node Field Edit Test"));
    const auto layerId = addSolid(fixture.session);
    expectations.expect(layerId.has_value(), "the fixture can add a solid layer");
    if (!layerId.has_value()) {
        return;
    }
    const auto boundaryNodeId = fixture.session.boundaryNodeForLayer(*layerId);
    if (!boundaryNodeId.has_value()) {
        expectations.expect(false, "the solid layer resolves its boundary node");
        return;
    }

    auto* positionX = qobject_cast<ui::kit::KValueField*>(
        fixture.scene()->nodeFieldForTest(*boundaryNodeId, QStringLiteral("nodePositionXEditor")));
    auto* positionY = qobject_cast<ui::kit::KValueField*>(
        fixture.scene()->nodeFieldForTest(*boundaryNodeId, QStringLiteral("nodePositionYEditor")));
    auto* opacity = qobject_cast<ui::kit::KValueField*>(
        fixture.scene()->nodeFieldForTest(*boundaryNodeId, QStringLiteral("nodeOpacityEditor")));
    expectations.expect(positionX != nullptr && positionY != nullptr && opacity != nullptr,
                        "the layer boundary card carries live kit fields for the parameters it "
                        "actually binds");
    if (positionX == nullptr || positionY == nullptr || opacity == nullptr) {
        return;
    }
    expectations.expect(positionX->isEnabled() && positionY->isEnabled() && opacity->isEnabled(),
                        "constant parameters are editable in the card, exactly as in Properties");

    const auto* positionParameter =
        parameterForRole(fixture.session, *boundaryNodeId, document::kPositionParameterRole);
    const auto* opacityParameter =
        parameterForRole(fixture.session, *boundaryNodeId, document::kOpacityParameterRole);
    if (positionParameter == nullptr || opacityParameter == nullptr) {
        expectations.expect(false, "the boundary node binds position and opacity");
        return;
    }
    const auto positionId = positionParameter->id;
    const auto opacityId = opacityParameter->id;
    const auto originalPosition = fixture.session.constantVec2Value(positionId);
    const auto originalOpacity = fixture.session.constantValue(opacityId);
    expectations.expect(originalPosition.has_value() && originalOpacity.has_value(),
                        "the fixture starts from constant position and opacity values");
    if (!originalPosition.has_value() || !originalOpacity.has_value()) {
        return;
    }
    const document::Vec2d editedPosition{24.0, originalPosition.value().y};

    // One gesture on the card: one command, one undo step, one snapshot signal, on the SAME
    // ParameterId the Properties panel writes.
    int snapshotSignals = 0;
    QObject::connect(&fixture.session, &ui::CompositionSession::snapshotChanged, &fixture.session,
                     [&snapshotSignals] { ++snapshotSignals; });
    positionX->setValue(24.0);
    expectations.expect(snapshotSignals == 1,
                        "one field gesture produces exactly one committed snapshot -- the session "
                        "signal flow is intact and the edit is not split across commands");
    expectations.expect(fixture.session.constantVec2Value(positionId) == editedPosition,
                        "an in-node X edit writes the canonical position parameter");
    expectations.expect(fixture.session.undoLabel() == QStringLiteral("Set Position"),
                        "through PropertiesEditor's own \"Set Position\" command path");
    expectations.expect(fixture.session.selection().primary == ui::SelectionTarget{*boundaryNodeId},
                        "and the edit routed through the session's one selection truth first");
    expectations.expect(fixture.session.undo() &&
                            fixture.session.constantVec2Value(positionId) == originalPosition,
                        "ONE undo step reverts the whole in-node edit");

    // The field survived the snapshot change its own edit produced -- the reconciliation contract
    // that makes an in-node field usable at all (a rebuilt card would delete the widget mid-signal
    // and lose keyboard focus on every step).
    expectations.expect(fixture.scene()->nodeFieldForTest(
                            *boundaryNodeId, QStringLiteral("nodePositionXEditor")) == positionX,
                        "the card is reconciled in place, so the field keeps its identity");

    opacity->setValue(50.0);
    const auto editedOpacity = fixture.session.constantValue(opacityId);
    expectations.expect(editedOpacity.has_value() && near(editedOpacity.value(), 0.5),
                        "an in-node opacity edit writes the canonical 0..1 opacity parameter");
    expectations.expect(fixture.session.undoLabel() == QStringLiteral("Set Opacity"),
                        "through PropertiesEditor's own \"Set Opacity\" command path");
    expectations.expect(fixture.session.undo() &&
                            fixture.session.constantValue(opacityId) == originalOpacity,
                        "and one undo step reverts it");
}

void testColorIsAReadOnlyChipAndParameterlessNodesStayClean(Expectations& expectations) {
    GraphFixture fixture(makeProject("Node Color Chip Test"));
    const auto layerId = addSolid(fixture.session);
    if (!layerId.has_value()) {
        expectations.expect(false, "the fixture can add a solid layer");
        return;
    }
    const auto sourceNodeId = fixture.session.directSourceNodeForLayer(*layerId);
    if (!sourceNodeId.has_value()) {
        expectations.expect(false, "the solid layer resolves its source node");
        return;
    }

    auto* chip = qobject_cast<ui::kit::KColorChip*>(
        fixture.scene()->nodeFieldForTest(*sourceNodeId, QStringLiteral("nodeColorChip")));
    expectations.expect(chip != nullptr, "the solid source card carries a kit color chip");
    if (chip == nullptr) {
        return;
    }
    // No command anywhere sets a color, so the chip must not offer a picker whose result nothing
    // could commit -- the honesty rule, pinned.
    expectations.expect(!chip->isEnabled(),
                        "the chip is read-only: no command in src/commands sets a color");
    expectations.expect(!chip->isPickerOpen(), "and it never opens its picker");
    expectations.expect(chip->toolTip().contains(QStringLiteral("R 0.62")),
                        "the exact, unclipped authoring value travels in the tooltip, because an "
                        "8-bit swatch cannot show one honestly");
    expectations.expect(chip->toolTip().contains(QStringLiteral("Read-only")),
                        "and the tooltip says why the chip does not open");

    // A node that binds no parameters carries no rows at all.
    const auto stackNodeId = fixture.session.composition()->graph().layerStack().nodeId();
    expectations.expect(fixture.scene()->nodeFieldForTest(
                            stackNodeId, QStringLiteral("nodePositionXEditor")) == nullptr &&
                            fixture.scene()->nodeFieldForTest(
                                stackNodeId, QStringLiteral("nodeColorChip")) == nullptr,
                        "a node with no parameter bindings stays clean -- no placeholder rows");
}

} // namespace

namespace {

int runAll() {
    Expectations expectations;
    testZoomAboutCursorHoldsTheScenePointUnderTheCursor(expectations);
    testZoomClampsToTheViewersOwnBounds(expectations);
    testFitFramesTheGraphAndActualSizeIsExactlyOneHundredPercent(expectations);
    testSpaceHoldAndMiddleDragBothPan(expectations);
    testSelectionFollowsTheSessionInBothDirections(expectations);
    testConnectorTypingIsPinnedPerSocketKind(expectations);
    testEveryProjectedEdgeIsAPathItemBetweenTwoCards(expectations);
    testMidChainCardsCarryBothPortDots(expectations);
    testContextMenuOffersOnlyRealCommands(expectations);
    testAddFromTheCanvasIsOneUndoableCommand(expectations);
    testInNodeValueFieldsCommitThroughThePropertiesPath(expectations);
    testColorIsAReadOnlyChipAndParameterlessNodesStayClean(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    try {
        return runAll();
    } catch (const std::exception& error) {
        std::cerr << "node editor test failed with an exception: " << error.what() << '\n';
        return 1;
    }
}
