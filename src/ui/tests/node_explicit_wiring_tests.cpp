// Task FIX1 item B: the artist's own wiring. Adding a source from the canvas creates that source
// and nothing else -- no Layer, no Merge slot, no edges -- and connecting a Layer output to Merge
// is what creates the stack slot the timeline lists.
#include "node_production_harness.hpp"

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

        // --- Item B: adding a source from the canvas adds ONLY that source. ---------------------
        const auto nodesBefore = a.session.composition()->graph().nodes().size();
        // A new project already holds Merge wired into Output; the baseline is what the canvas's
        // Add must leave untouched.
        const auto edgesBefore = edges(a);
        const auto textNode = a.addThroughSearch(QStringLiteral("Text"));
        expect(textNode.has_value(), "Tab -> \"Text\" -> Enter adds a node");
        if (!textNode.has_value())
            return 1;
        expect(a.session.composition()->graph().findNode(*textNode) != nullptr &&
                   a.session.composition()->graph().findNode(*textNode)->typeId ==
                       document::kTextSourceNodeType,
               "and the node it adds is the Text source");
        expect(a.session.composition()->graph().nodes().size() == nodesBefore + 1 &&
                   stackSlotCount(a) == 0 && edges(a) == edgesBefore &&
                   a.session.composition()->graph().layerOutputs().empty(),
               "adding Text from the canvas creates no Layer, no Merge slot and no edges");

        // --- A bare Layer node gets its own identity, and the timeline does not list it yet. -----
        const auto layer = a.addByType(document::kLayerOutputNodeType, {300, 0});
        expect(a.session.composition()->graph().layerOutputs().size() == 1 &&
                   a.session.composition()->graph().layerOutputs().front().name == "Layer 1",
               "a Layer node added bare carries a layer boundary named Layer 1");
        expect(stackSlotCount(a) == 0, "but no stack slot, so the timeline lists nothing yet");

        // An unwired Layer still compiles: it simply draws nothing.
        expect(composite(a).ok, "a composition holding an unwired Layer node still compiles");

        // --- The artist wires source -> Layer image by hand. ------------------------------------
        a.drag(a.outputSocket(*textNode)->scenePos(),
               a.namedSocket(layer, QString::fromUtf8(document::kLayerOutputContentInputPort), true)
                   ->scenePos());
        QCoreApplication::processEvents();
        expect(edges(a) == edgesBefore + 1,
               "dragging the Text output onto the Layer's image input connects them");
        expect(stackSlotCount(a) == 0, "and still creates no stack slot");

        // --- And Layer output -> Merge, which is what creates the slot. -------------------------
        const auto merge = a.nodeOfType(document::kLayerStackNodeType);
        expect(merge.has_value(), "the composition holds a Merge node");
        if (!merge.has_value())
            return 1;
        auto* pill =
            a.namedSocket(*merge, QString::fromUtf8(document::kLayerStackContentInputRole), true);
        expect(pill != nullptr && pill->multiInput(),
               "Merge carries its ordered multi-input even with an empty stack");
        if (pill == nullptr)
            return 1;
        expect(pill->draggable(), "and it is a link end, not a structural dead end");
        a.drag(a.outputSocket(layer)->scenePos(), pill->scenePos());
        QCoreApplication::processEvents();
        expect(stackSlotCount(a) == 1,
               "connecting the Layer output to Merge creates the stack slot");
        expect(a.session.composition()->graph().layerStack().entries().front().layerId ==
                   a.session.composition()->graph().layerOutputs().front().layerId,
               "and the slot belongs to that Layer's own layer");

        // The wired layer now actually draws. The Text fixture's default content is empty, so the
        // honest assertion is that the frame evaluates -- the Solid path below proves coverage.
        expect(composite(a).ok, "the hand-wired composition evaluates");

        // --- A Solid wired by hand reaches the frame. -------------------------------------------
        {
            App fresh;
            const auto solid = fresh.addByType(document::kSolidSourceNodeType, {-400, 0});
            const auto boundary = fresh.addByType(document::kLayerOutputNodeType, {0, 0});
            const auto stack = fresh.nodeOfType(document::kLayerStackNodeType);
            if (!stack.has_value())
                return 1;
            fresh.drag(fresh.outputSocket(solid)->scenePos(),
                       fresh
                           .namedSocket(boundary,
                                        QString::fromUtf8(document::kLayerOutputContentInputPort),
                                        true)
                           ->scenePos());
            QCoreApplication::processEvents();
            fresh.drag(fresh.outputSocket(boundary)->scenePos(),
                       fresh
                           .namedSocket(*stack,
                                        QString::fromUtf8(document::kLayerStackContentInputRole),
                                        true)
                           ->scenePos());
            QCoreApplication::processEvents();
            const auto frame = composite(fresh);
            expect(frame.ok && closeTo(frame.centerPixel[3], 1.0),
                   "a Solid wired into a Layer and then into Merge covers the frame");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
