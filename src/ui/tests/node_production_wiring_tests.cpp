// Task N3 (W3): the node editor constructed exactly as the application constructs it -- no test
// adapter injected -- mutates the document through CompositionSession::executeNodeTransaction().
#include "node_editor_items.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/node_editor.hpp>

#include <QApplication>
#include <QMouseEvent>
#include <QTest>

#include <iostream>

namespace {
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}

struct App final {
    bloom::document::NewProject initial = bloom::document::makeNewProject(
        "N3 wiring", "Main", bloom::core::RationalTime::fromInteger(10));
    bloom::document::Document document{std::move(initial.project)};
    bloom::commands::CommandStack stack{document};
    bloom::ui::CompositionSession session{document, stack, initial.initialCompositionId};
    bloom::ui::NodeGraphEditor editor{session};

    App() {
        editor.resize(1200, 800);
        editor.show();
        QCoreApplication::processEvents();
        editor.graphView()->zoomToActualSize();
        editor.graphView()->setTransform(QTransform::fromTranslate(40, 40));
        editor.graphView()->setFocus();
    }
    [[nodiscard]] bloom::document::NodeId nodeOfType(std::string_view typeId) const {
        for (const auto& node : session.composition()->graph().nodes()) {
            if (node.typeId == typeId) {
                return node.id;
            }
        }
        throw std::runtime_error("node type not present");
    }
    [[nodiscard]] bloom::ui::node_editor::NodeItem* card(bloom::document::NodeId id) {
        return dynamic_cast<bloom::ui::node_editor::NodeItem*>(
            editor.graphScene()->findNodeItem(id));
    }
    [[nodiscard]] bloom::ui::node_editor::SocketItem* socket(bloom::document::NodeId id,
                                                             bool input) {
        for (auto* socket : card(id)->sockets()) {
            if (socket->input.has_value() == input) {
                return socket;
            }
        }
        throw std::runtime_error("socket not present");
    }
    void mouse(QEvent::Type type, QPointF point, Qt::MouseButton button, Qt::MouseButtons buttons) {
        auto* view = editor.graphView();
        const QPoint viewport = view->mapFromScene(point);
        QMouseEvent event(type, viewport, view->viewport()->mapToGlobal(viewport), button, buttons,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(view->viewport(), &event);
    }
    void drag(QPointF start, QPointF end) {
        mouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    }
};
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        App a;
        expect(bloom::ui::addDefaultSolidLayer(a.session), "a solid layer is added");
        const auto solid = a.nodeOfType(bloom::document::kSolidSourceNodeType);
        const auto before = a.session.composition()->nodeLayout().at(solid).position;

        // One drag on the card header -> exactly one MoveNodes transaction through the session.
        auto* solidCard = a.card(solid);
        expect(solidCard != nullptr, "the solid card exists");
        const QPointF grab = solidCard->scenePos() + QPointF(24, 12);
        a.drag(grab, grab + QPointF(120, 60));
        const auto after = a.session.composition()->nodeLayout().at(solid).position;
        expect(after.x > before.x && after.y > before.y,
               "a production drag persists the node layout through the session");
        expect(a.session.undo(), "the drag is one undoable session entry");
        expect(a.session.composition()->nodeLayout().at(solid).position == before,
               "undo restores the pre-drag layout");

        // One connect: a generic Layer Output added through the session, then a socket drag from
        // the solid's output to its input creates exactly one new edge.
        bloom::commands::Transaction addOutput("Add Layer Output", a.session.snapshot().revision());
        addOutput.emplace<bloom::commands::AddNode>(
            a.session.compositionId(), std::string(bloom::document::kLayerOutputNodeType),
            bloom::document::Vec2d{700, 400});
        const auto added = a.session.executeNodeTransaction(std::move(addOutput));
        const auto output =
            added.outputId<bloom::document::NodeId>(bloom::commands::kAddNodeOutput);
        expect(added.succeeded() && output.has_value(),
               "AddNode executes through the public session seam and reports its id");
        if (!output.has_value()) {
            throw std::runtime_error("AddNode reported no id");
        }
        QCoreApplication::processEvents();
        const auto edgesBefore = a.session.composition()->graph().edges().size();
        a.drag(a.socket(solid, false)->scenePos(), a.socket(output.value(), true)->scenePos());
        expect(a.session.composition()->graph().edges().size() == edgesBefore + 1,
               "a production socket drag connects through the session");

        // One Shift+D: duplicates the selection through the session.
        a.session.selectNodes({solid}, solid);
        QCoreApplication::processEvents();
        const auto nodesBefore = a.session.composition()->graph().nodes().size();
        QTest::keyClick(a.editor.graphView(), Qt::Key_D, Qt::ShiftModifier);
        expect(a.session.composition()->graph().nodes().size() > nodesBefore,
               "Shift+D duplicates through the session");
        expect(a.session.canUndo(), "duplication is undoable through the session");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
