// Task FIX1 item C: every link detaches, and the node it left survives. The two that could not be
// detached before -- a source into a Layer's image input is ordinary, but a Layer's output into
// Merge and the stack slot it lands in were structural -- are now created by connecting and removed
// by disconnecting, so one gesture and one menu serve every link kind.
#include "node_production_harness.hpp"

#include <QMenu>

using namespace bloom;
using namespace bloom::ui;
using namespace bloom::ui::production_test;

namespace {
[[nodiscard]] std::size_t stackSlotCount(const App& app) {
    return app.session.composition()->graph().layerStack().entries().size();
}
[[nodiscard]] std::size_t edges(const App& app) {
    return app.session.composition()->graph().edges().size();
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        App a;
        // A hand-wired layer: Solid -> Layer -> Merge, exactly as item B's pin builds one.
        const auto source = a.addByType(document::kSolidSourceNodeType, {-400, 0});
        const auto layer = a.addByType(document::kLayerOutputNodeType, {0, 0});
        const auto merge = a.nodeOfType(document::kLayerStackNodeType);
        if (!merge.has_value())
            return 1;
        const auto textNode = source;
        a.drag(a.outputSocket(source)->scenePos(),
               a.namedSocket(layer, QString::fromUtf8(document::kLayerOutputContentInputPort), true)
                   ->scenePos());
        QCoreApplication::processEvents();
        a.drag(a.outputSocket(layer)->scenePos(),
               a.namedSocket(*merge, QString::fromUtf8(document::kLayerStackContentInputRole), true)
                   ->scenePos());
        QCoreApplication::processEvents();
        expect(stackSlotCount(a) == 1, "the detach fixture has one participating layer");
        // TimelineEditor::rebuild() builds one row per LayerStack entry and nothing else, so the
        // stack's own entry list IS the timeline's row list; asserting it here keeps this pin free
        // of a preview pipeline it has no other use for.

        // --- Item C: the slot link detaches, and the node survives. ------------------------------
        {
            auto* slotPill = a.namedSocket(
                *merge, QString::fromUtf8(document::kLayerStackContentInputRole), true);
            a.drag(slotPill->scenePos(), QPointF(-1200, 800));
            QCoreApplication::processEvents();
            expect(stackSlotCount(a) == 0,
                   "dropping the slot's link on empty canvas removes the slot");
            expect(a.session.composition()->graph().findNode(layer) != nullptr &&
                       a.session.composition()->graph().layerOutputs().size() == 1,
                   "the Layer node and its boundary survive, so reconnecting is one gesture");
            expect(stackSlotCount(a) == 0, "so the timeline has no row for it any more");
            expect(a.session.undo() && stackSlotCount(a) == 1,
                   "one undo restores the slot, and the timeline row with it");
        }

        // --- Item C: the source -> Layer link detaches too. -------------------------------------
        {
            auto* imageInput = a.namedSocket(
                layer, QString::fromUtf8(document::kLayerOutputContentInputPort), true);
            const auto before = edges(a);
            a.drag(imageInput->scenePos(), QPointF(-1200, -800));
            QCoreApplication::processEvents();
            expect(edges(a) == before - 1,
                   "dropping the image link on empty canvas disconnects it");
            expect(a.session.composition()->graph().findNode(textNode) != nullptr,
                   "and the source node survives");
            expect(a.session.undo() && edges(a) == before, "one undo restores the image link");
        }

        // --- Item C: a link's own context menu disconnects it. ----------------------------------
        {
            const auto before = edges(a);
            const document::InputPortRef imageRef =
                document::NodeInputRef{layer, std::string(document::kLayerOutputContentInputPort)};
            auto* link = static_cast<node_editor::NodeEdgeItem*>(nullptr);
            for (auto* item : a.editor.graphScene()->items())
                if (auto* candidate = dynamic_cast<node_editor::NodeEdgeItem*>(item);
                    candidate != nullptr && candidate->edge.destination == imageRef)
                    link = candidate;
            expect(link != nullptr, "the image link is projected");
            if (link != nullptr) {
                expect(link->toolTip().contains(QStringLiteral("Layer 1")),
                       "a link's tooltip names both of its ends");
                const auto midpoint = link->path().pointAtPercent(0.5);
                const QPoint viewport = a.editor.graphView()->mapFromScene(midpoint);
                auto* menu = a.editor.linkContextMenuForTest(viewport);
                expect(menu != nullptr, "right-clicking a link offers a menu");
                if (menu != nullptr) {
                    auto* disconnect =
                        menu->findChild<QAction*>(QStringLiteral("nodeLinkDisconnectAction"));
                    expect(disconnect != nullptr, "whose action is Disconnect");
                    if (disconnect != nullptr) {
                        disconnect->trigger();
                        QCoreApplication::processEvents();
                        expect(edges(a) == before - 1, "and it disconnects the link");
                        expect(a.session.undo() && edges(a) == before, "one undo restores it");
                    }
                    delete menu;
                }
            }
        }

    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
