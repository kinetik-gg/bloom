// Task FIX1 item A: the owner's own gestures, through the application's own wiring -- no test
// adapter on NodeGraphicsScene::setSubmit(). Tab, type, Enter to add a value node; drag its output
// onto a Layer card's operand socket; watch the composited frame change.
//
// The owner's report was "nodes exist but not usable ... plugging in something like scalar to the
// input socket". The failing step was the FIRST one: Tab -> "Scalar" -> Enter added a TEXT layer,
// because the add search listed its entries in category order and matched name and socket-kind
// keywords alike, so the Sources section's Text node (which carries a Scalar size socket) came
// before the node actually called Scalar. Two more things then made a driver link unusable once it
// existed: nothing DREW it, and the pick-up gesture only knew how to find an edge.
#include "node_production_harness.hpp"

#include <bloom/ui/kit/value_field.hpp>

#include <QSignalSpy>

using namespace bloom;
using namespace bloom::ui;
using namespace bloom::ui::production_test;

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        App a;
        expect(addDefaultSolidLayer(a.session), "a solid layer exists to drive");
        QCoreApplication::processEvents();
        const auto layerNode = a.nodeOfType(document::kLayerOutputNodeType);
        const auto solidNode = a.nodeOfType(document::kSolidSourceNodeType);
        if (!layerNode.has_value() || !solidNode.has_value())
            return 1;
        const auto layer = *layerNode;
        const auto solid = *solidNode;

        // --- Step 1: the owner's add gesture. THE failing step before this task. ---------------
        const auto searched = a.addThroughSearch(QStringLiteral("Scalar"));
        const auto* searchedRecord =
            searched ? a.session.composition()->graph().findNode(*searched) : nullptr;
        expect(searchedRecord != nullptr &&
                   searchedRecord->typeId == document::kScalarValueNodeType,
               "Tab -> \"Scalar\" -> Enter adds the node CALLED Scalar, not the first node "
               "carrying a Scalar socket");
        if (searchedRecord == nullptr || !searched.has_value())
            return 1;
        const auto scalar = searched.value();
        const auto text = a.addThroughSearch(QStringLiteral("Text"));
        const auto* textRecord = text ? a.session.composition()->graph().findNode(*text) : nullptr;
        expect(textRecord != nullptr && textRecord->typeId == document::kTextSourceNodeType,
               "an exact name still wins its own query");

        // --- Step 2: the value node's own inline editor is editable on the card. ----------------
        auto* valueField = qobject_cast<kit::KValueField*>(
            a.editor.graphScene()->nodeFieldForTest(scalar, QStringLiteral("nodeOperandEditor")));
        expect(valueField != nullptr, "the Scalar card carries an inline editable value field");
        if (valueField == nullptr)
            return 1;
        expect(valueField->window()->graphicsProxyWidget() != nullptr && valueField->isVisible(),
               "the Scalar card's inline field is visible at rest");
        valueField->setValue(0.25);
        QCoreApplication::processEvents();
        {
            const auto* parameter = parameterFor(a, scalar, document::kValueParameterRole);
            const auto* constant =
                parameter == nullptr
                    ? nullptr
                    : std::get_if<document::ConstantValueSource>(&parameter->source);
            const auto* held =
                constant == nullptr ? nullptr : std::get_if<double>(&constant->value);
            expect(held != nullptr && *held == 0.25,
                   "editing the Scalar card's inline field writes the document");
        }

        // --- Step 3: operand sockets at rest, and the link gesture binds a driver. --------------
        auto* from = a.outputSocket(scalar);
        auto* toOpacity =
            a.namedSocket(layer, QString::fromUtf8(document::kOpacityParameterRole), true);
        auto* toPosition =
            a.namedSocket(layer, QString::fromUtf8(document::kPositionParameterRole), true);
        auto* toColor =
            a.namedSocket(solid, QString::fromUtf8(document::kSolidColorParameterRole), true);
        expect(from != nullptr && toOpacity != nullptr && toPosition != nullptr &&
                   toColor != nullptr,
               "Layer and Solid cards carry their operand sockets");
        if (from == nullptr || toOpacity == nullptr || toPosition == nullptr || toColor == nullptr)
            return 1;
        expect(from->isVisible() && toOpacity->isVisible() && toColor->isVisible(),
               "operand sockets are visible at rest, not only on hover");
        expect(from->draggable() && toOpacity->draggable(),
               "a value output and an operand socket are both draggable");

        a.press(from->scenePos());
        a.move(toOpacity->scenePos());
        const auto previewPen = [&a]() -> QPen {
            for (auto* item : a.editor.graphScene()->items())
                if (item->data(kNodeItemKindRole) == QStringLiteral("link-preview"))
                    return dynamic_cast<QGraphicsPathItem*>(item)->pen();
            return {};
        };
        expect(previewPen().color() ==
                   kit::color(socketColorToken(document::SocketValueKind::Scalar)),
               "a compatible hover keeps the link's own kind colour");
        expect(toOpacity->dragAffinity() == node_editor::SocketItem::DragAffinity::Compatible,
               "a compatible operand socket brightens during the drag");
        a.move(toColor->scenePos());
        expect(previewPen().color() == kit::color(kit::Color::Error),
               "an incompatible hover paints the preview Error red");
        expect(toColor->toolTip().contains(QStringLiteral("does not connect to")),
               "an incompatible socket says why it refuses while the drag is in flight");
        a.release(toOpacity->scenePos());
        QCoreApplication::processEvents();
        expect(driverFor(a, layer, document::kOpacityParameterRole) != nullptr &&
                   driverFor(a, layer, document::kOpacityParameterRole)->sourceNodeId == scalar,
               "dragging the Scalar output onto the opacity socket binds the driver");
        expect(!a.namedSocket(solid, QString::fromUtf8(document::kSolidColorParameterRole), true)
                    ->toolTip()
                    .contains(QStringLiteral("does not connect to")),
               "the refusal line leaves the tooltip when the drag ends");

        // The driver is DRAWN. Before this task nothing rendered it: the widget vanished and no
        // wire appeared, which is most of what "nodes exist but not usable" meant.
        {
            const document::InputPortRef opacityRef =
                document::NodeInputRef{layer, std::string(document::kOpacityParameterRole)};
            bool wired = false;
            for (auto* item : a.editor.graphScene()->items())
                if (const auto* link = dynamic_cast<node_editor::NodeEdgeItem*>(item);
                    link != nullptr && link->edge.destination == opacityRef &&
                    link->edge.source.nodeId == scalar)
                    wired = true;
            expect(wired, "a driver binding is drawn on the canvas as a link");
        }

        // --- Step 4: the card hides the widget the link now drives. -----------------------------
        if (auto* opacityField =
                a.editor.graphScene()->nodeFieldForTest(layer, QStringLiteral("nodeOpacityEditor"));
            opacityField != nullptr && opacityField->window()->graphicsProxyWidget() != nullptr)
            expect(!opacityField->isVisible(),
                   "a driven opacity hides its inline field on the Layer card");

        // --- Step 5: the viewer. ---------------------------------------------------------------
        const auto driven = composite(a);
        expect(driven.ok && closeTo(driven.centerPixel[3], 0.25) &&
                   closeTo(driven.centerPixel[0], 0.62 * 0.25),
               "the driven opacity reaches the composited frame");
        valueField->setValue(0.75);
        QCoreApplication::processEvents();
        const auto rebound = composite(a);
        expect(rebound.ok && closeTo(rebound.centerPixel[3], 0.75),
               "changing the value node's own number re-evaluates the driven frame");

        // --- Scalar -> Vector2: the documented splat promotion. --------------------------------
        a.drag(a.outputSocket(scalar)->scenePos(),
               a.namedSocket(layer, QString::fromUtf8(document::kPositionParameterRole), true)
                   ->scenePos());
        QCoreApplication::processEvents();
        expect(driverFor(a, layer, document::kPositionParameterRole) != nullptr,
               "a Scalar drives a Vector2 operand through the splat promotion");

        // --- Scalar -> Color: refused, and the preview already said so. ------------------------
        {
            const auto revision = a.session.snapshot().revision().value();
            a.drag(a.outputSocket(scalar)->scenePos(),
                   a.namedSocket(solid, QString::fromUtf8(document::kSolidColorParameterRole), true)
                       ->scenePos());
            QCoreApplication::processEvents();
            expect(a.session.snapshot().revision().value() == revision &&
                       driverFor(a, solid, document::kSolidColorParameterRole) == nullptr,
                   "Scalar does not promote into a Color operand");
        }

        // --- A Math node's output drives opacity too. ------------------------------------------
        const auto math = a.addByType(document::kScalarMathNodeType, {-900, 200});
        a.drag(a.outputSocket(math)->scenePos(),
               a.namedSocket(layer, QString::fromUtf8(document::kOpacityParameterRole), true)
                   ->scenePos());
        QCoreApplication::processEvents();
        expect(driverFor(a, layer, document::kOpacityParameterRole) != nullptr &&
                   driverFor(a, layer, document::kOpacityParameterRole)->sourceNodeId == math,
               "a Math node's output rewires the opacity driver");

        // --- Disconnect restores the registered constant; undo restores the driver. ------------
        {
            a.drag(a.namedSocket(layer, QString::fromUtf8(document::kOpacityParameterRole), true)
                       ->scenePos(),
                   QPointF(-1400, 900));
            QCoreApplication::processEvents();
            const auto* parameter = parameterFor(a, layer, document::kOpacityParameterRole);
            const auto* constant =
                parameter == nullptr
                    ? nullptr
                    : std::get_if<document::ConstantValueSource>(&parameter->source);
            const auto* held =
                constant == nullptr ? nullptr : std::get_if<double>(&constant->value);
            expect(held != nullptr && *held == 1.0,
                   "dropping a driven operand's link on empty canvas restores its default");
            expect(a.session.undo() &&
                       driverFor(a, layer, document::kOpacityParameterRole) != nullptr,
                   "one undo restores the driver");
        }

        // --- The grab radius is an ARTIST's radius: 12 device px at any zoom. ------------------
        {
            a.editor.graphView()->setTransform(QTransform::fromScale(0.25, 0.25) *
                                               QTransform::fromTranslate(40, 40));
            QCoreApplication::processEvents();
            auto* socket =
                a.namedSocket(solid, QString::fromUtf8(document::kSolidColorParameterRole), true);
            // 12 device px out at a quarter zoom is 48 scene px -- far outside the socket's own
            // painted hit shape.
            a.press(socket->scenePos() - QPointF(48, 0));
            const bool grabbed = a.editor.graphScene()->gestureActive();
            a.editor.graphScene()->cancelGesture();
            expect(grabbed, "a socket is grabbable 12 device px out on a zoomed-out canvas");
            a.editor.graphView()->setTransform(QTransform::fromTranslate(40, 40));
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
