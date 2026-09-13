// Task FIX1 item A: the owner's own gestures, through the application's own wiring -- no test
// adapter on NodeGraphicsScene::setSubmit(). Tab, type, Enter to add a value node; drag its output
// onto a Layer card's operand socket; watch the composited frame change.
//
// The owner's report was "nodes exist but not usable ... plugging in something like scalar to the
// input socket". The failing step was the FIRST one: Tab -> "Scalar" -> Enter added a TEXT layer,
// because the add search listed its entries in category order and matched name and socket-kind
// keywords alike, so the Sources section's Text node (which carries a Scalar size socket) came
// before the node actually called Scalar. Everything downstream of that already worked, which is
// why this file pins the whole chain rather than only the search.
#include "node_editor_add.hpp"
#include "node_editor_items.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/search_popup.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/node_editor.hpp>

#include <QApplication>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>

#include <array>
#include <cmath>
#include <iostream>
#include <optional>

namespace {
using namespace bloom;
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

struct App final {
    document::NewProject initial =
        document::makeNewProject("FIX1", "Main", core::RationalTime::fromInteger(10));
    document::Document document{std::move(initial.project)};
    commands::CommandStack stack{document};
    ui::CompositionSession session{document, stack, initial.initialCompositionId};
    ui::NodeGraphEditor editor{session};

    App() {
        editor.resize(1400, 900);
        editor.show();
        QCoreApplication::processEvents();
        editor.graphView()->zoomToActualSize();
        editor.graphView()->setTransform(QTransform::fromTranslate(40, 40));
        editor.graphView()->setFocus();
    }
    [[nodiscard]] document::NodeId nodeOfType(std::string_view typeId) const {
        for (const auto& node : session.composition()->graph().nodes())
            if (node.typeId == typeId)
                return node.id;
        throw std::runtime_error("node type not present");
    }
    [[nodiscard]] ui::node_editor::NodeItem* card(document::NodeId id) {
        return dynamic_cast<ui::node_editor::NodeItem*>(editor.graphScene()->findNodeItem(id));
    }
    [[nodiscard]] ui::node_editor::SocketItem* namedSocket(document::NodeId id, const QString& name,
                                                           bool input) {
        for (auto* socket : card(id)->sockets())
            if (socket->input.has_value() == input && socket->name == name)
                return socket;
        return nullptr;
    }
    [[nodiscard]] ui::node_editor::SocketItem* outputSocket(document::NodeId id) {
        for (auto* socket : card(id)->sockets())
            if (socket->output.has_value())
                return socket;
        return nullptr;
    }
    void mouse(QEvent::Type type, QPointF point, Qt::MouseButton button, Qt::MouseButtons buttons) {
        auto* view = editor.graphView();
        const QPoint viewport = view->mapFromScene(point);
        QMouseEvent event(type, viewport, view->viewport()->mapToGlobal(viewport), button, buttons,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(view->viewport(), &event);
    }
    void press(QPointF point) {
        mouse(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    }
    void move(QPointF point) { mouse(QEvent::MouseMove, point, Qt::NoButton, Qt::LeftButton); }
    void release(QPointF point) {
        mouse(QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    }
    void drag(QPointF start, QPointF end) {
        press(start);
        move(end);
        release(end);
    }
    document::NodeId addByType(std::string_view typeId, document::Vec2d position) {
        commands::Transaction transaction("Add Node", session.snapshot().revision());
        transaction.emplace<ui::node_editor::AddEditorNode>(session.compositionId(),
                                                            std::string(typeId), position);
        const auto result = session.executeNodeTransaction(std::move(transaction));
        const auto id = result.outputId<document::NodeId>("editorNode");
        if (!id)
            throw std::runtime_error("addByType failed");
        QCoreApplication::processEvents();
        return *id;
    }
    // Tab -> type -> Enter, the owner's own add gesture.
    std::optional<document::NodeId> addThroughSearch(const QString& query) {
        const auto before = session.composition()->graph().nodes().size();
        QTest::keyClick(editor.graphView(), Qt::Key_Tab);
        QCoreApplication::processEvents();
        auto* popup = editor.findChild<ui::kit::KSearchPopup*>();
        if (popup == nullptr || !popup->isVisible())
            return std::nullopt;
        auto* field = popup->findChild<QLineEdit*>();
        field->setText(query);
        QTest::keyClick(field, Qt::Key_Return);
        QCoreApplication::processEvents();
        if (session.composition()->graph().nodes().size() == before)
            return std::nullopt;
        const auto* selected = session.selectedNode();
        return selected == nullptr ? std::nullopt : std::optional(selected->id);
    }
};

const document::ParameterRecord* parameterFor(const App& app, document::NodeId node,
                                              std::string_view role) {
    const auto* record = app.session.composition()->graph().findNode(node);
    if (record == nullptr)
        return nullptr;
    for (const auto& binding : record->parameters)
        if (binding.role == role)
            return app.session.composition()->parameters().find(binding.parameterId);
    return nullptr;
}

[[nodiscard]] const document::DriverBindingSource* driverFor(const App& app, document::NodeId node,
                                                             std::string_view role) {
    const auto* parameter = parameterFor(app, node, role);
    return parameter == nullptr ? nullptr
                                : std::get_if<document::DriverBindingSource>(&parameter->source);
}

// The composited frame, straight through the production compiler and CPU evaluator.
struct Composited final {
    bool ok = false;
    std::array<float, 4> centerPixel{};
};
Composited composite(const App& app) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const runtime::SnapshotCompileRequest request{app.session.snapshot(),
                                                  app.session.compositionId()};
    const runtime::CancellationToken cancellation;
    const auto compiled = compiler.compile(request, cancellation);
    if (compiled.status != runtime::SnapshotCompileStatus::Compiled || !compiled.plan) {
        for (const auto& diagnostic : compiled.diagnostics)
            std::cerr << "DIAG: compile " << diagnostic.summary << " | " << diagnostic.detail
                      << '\n';
        return {};
    }
    const runtime::CpuCompositionEvaluator evaluator;
    const runtime::EvaluationRequest evaluation{
        .time = core::RationalTime::fromInteger(0),
        .output = compiled.plan->output(),
        .resolution = runtime::CompositionFormatResolution{},
        .quality = runtime::EvaluationQuality::Reference,
        .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
        .pixelStorageByteLimit = std::size_t{1} << 28U};
    const auto result = evaluator.evaluate(compiled.plan, evaluation, cancellation);
    if (result.status() != runtime::EvaluationStatus::Evaluated || !result.frame()) {
        for (const auto& diagnostic : result.diagnostics())
            std::cerr << "DIAG: evaluate " << diagnostic.summary << '\n';
        return {};
    }
    const auto read = result.frame()->processImage().read(0, 0);
    if (!read)
        return {};
    Composited out;
    out.ok = true;
    out.centerPixel = {read.value()->red(), read.value()->green(), read.value()->blue(),
                       read.value()->alpha()};
    return out;
}

[[nodiscard]] bool close(const float value, const double expected) {
    return std::abs(static_cast<double>(value) - expected) < 1e-4;
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        App a;
        expect(ui::addDefaultSolidLayer(a.session), "a solid layer exists to drive");
        QCoreApplication::processEvents();
        const auto layer = a.nodeOfType(document::kLayerOutputNodeType);
        const auto solid = a.nodeOfType(document::kSolidSourceNodeType);

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
        auto* valueField = qobject_cast<ui::kit::KValueField*>(
            a.editor.graphScene()->nodeFieldForTest(scalar, QStringLiteral("nodeOperandEditor")));
        expect(valueField != nullptr, "the Scalar card carries an inline editable value field");
        if (valueField == nullptr)
            return 1;
        expect(valueField->graphicsProxyWidget() != nullptr &&
                   valueField->graphicsProxyWidget()->isVisible(),
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
                if (item->data(ui::kNodeItemKindRole) == QStringLiteral("link-preview"))
                    return dynamic_cast<QGraphicsPathItem*>(item)->pen();
            return {};
        };
        expect(previewPen().color() ==
                   ui::kit::color(ui::socketColorToken(document::SocketValueKind::Scalar)),
               "a compatible hover keeps the link's own kind colour");
        expect(toOpacity->dragAffinity() == ui::node_editor::SocketItem::DragAffinity::Compatible,
               "a compatible operand socket brightens during the drag");
        a.move(toColor->scenePos());
        expect(previewPen().color() == ui::kit::color(ui::kit::Color::Error),
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
                if (const auto* link = dynamic_cast<ui::node_editor::NodeEdgeItem*>(item);
                    link != nullptr && link->edge.destination == opacityRef &&
                    link->edge.source.nodeId == scalar)
                    wired = true;
            expect(wired, "a driver binding is drawn on the canvas as a link");
        }

        // --- Step 4: the card hides the widget the link now drives. -----------------------------
        if (auto* opacityField =
                a.editor.graphScene()->nodeFieldForTest(layer, QStringLiteral("nodeOpacityEditor"));
            opacityField != nullptr && opacityField->graphicsProxyWidget() != nullptr)
            expect(!opacityField->graphicsProxyWidget()->isVisible(),
                   "a driven opacity hides its inline field on the Layer card");

        // --- Step 5: the viewer. ---------------------------------------------------------------
        const auto driven = composite(a);
        expect(driven.ok && close(driven.centerPixel[3], 0.25) &&
                   close(driven.centerPixel[0], 0.62 * 0.25),
               "the driven opacity reaches the composited frame");
        valueField->setValue(0.75);
        QCoreApplication::processEvents();
        const auto rebound = composite(a);
        expect(rebound.ok && close(rebound.centerPixel[3], 0.75),
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
