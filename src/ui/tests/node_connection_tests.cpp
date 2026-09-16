#include "node_interaction_test_support.hpp"
#include <QImage>
#include <QPainter>

namespace bloom::ui::test {
namespace {
const document::EdgeRecord* incoming(Fixture& fixture, const document::NodeId id) {
    const auto edges = fixture.session.composition()->graph().edges();
    const auto found = std::ranges::find_if(edges, [&](const auto& edge) {
        return edge.destination == document::InputPortRef(document::NodeInputRef{id, "image"});
    });
    return found == edges.end() ? nullptr : &*found;
}
node_editor::NodeEdgeItem* edgeItem(Fixture& fixture, const document::NodeId destination) {
    for (auto* item : fixture.scene()->items())
        if (auto* edge = dynamic_cast<node_editor::NodeEdgeItem*>(item);
            edge && node_editor::destinationNodeId(edge->edge.destination) == destination)
            return edge;
    throw std::runtime_error("fixture link item");
}
} // namespace
void testConnectionsCutAndInsertion() {
    {
        Fixture mergeFixture;
        const auto nested = mergeFixture.add(document::kLayerStackNodeType, {650, 300});
        const auto first = mergeFixture.add(document::kSolidSourceNodeType, {100, 100});
        const auto second = mergeFixture.add(document::kSolidSourceNodeType, {100, 400});
        mergeFixture.drag(mergeFixture.socket(first, false)->scenePos(),
                          mergeFixture.socket(nested, true)->scenePos());
        expect(mergeFixture.session.composition()->graph().merge(nested)->entries().size() == 1,
               "a new Merge pill accepts a plain source");
        auto* pill = mergeFixture.socket(nested, true);
        mergeFixture.drag(mergeFixture.socket(second, false)->scenePos(),
                          pill->scenePos() - QPointF(0, 5));
        const auto entries = mergeFixture.session.composition()->graph().merge(nested)->entries();
        expect(entries.size() == 2, "insertion drop creates a second slot");
        if (entries.size() == 2) {
            const auto firstSlot = entries.front().slotId;
            pill = mergeFixture.socket(nested, true);
            const auto height = pill->rowHeight();
            mergeFixture.drag(pill->scenePos() - QPointF(0, height / 4),
                              pill->scenePos() + QPointF(0, height / 2 - 1));
            expect(mergeFixture.session.composition()
                           ->graph()
                           .merge(nested)
                           ->entries()
                           .back()
                           .slotId == firstSlot,
                   "dragging within a Merge reorders the same stable slot");
        }
    }

    Fixture f;
    const auto source = f.add(document::kSolidSourceNodeType, {100, 100});
    const auto second = f.add(document::kSolidSourceNodeType, {100, 350});
    const auto target = f.add(document::kLayerOutputNodeType, {700, 100});
    const auto pass = f.add(document::kLayerOutputNodeType, {350, 350});
    auto history = f.stack.size();
    auto start = f.socket(source, false)->scenePos();
    auto end = f.socket(target, true)->scenePos();
    f.press(start);
    f.move(end);
    auto* preview = static_cast<QGraphicsPathItem*>(nullptr);
    for (auto* item : f.scene()->items())
        if (item->data(kNodeItemKindRole) == QStringLiteral("link-preview"))
            preview = dynamic_cast<QGraphicsPathItem*>(item);
    expect(preview && !preview->path().isEmpty() && !incoming(f, target),
           "socket drag draws a live Bezier without publishing an edge");
    f.release(end);
    expect(incoming(f, target) && incoming(f, target)->source.nodeId == source &&
               f.stack.size() == history + 1,
           "socket release connects through one real command");
    if (!incoming(f, target))
        return;
    const auto wireImage = [&] {
        QImage image(1000, 600, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        edgeItem(f, target)->paint(&painter, nullptr, nullptr);
        painter.end();
        return image;
    };
    f.session.clearSelection();
    const auto idleWire = wireImage();
    f.session.selectNode(source);
    expect(wireImage() != idleWire, "links brighten when either endpoint is selected");
    f.session.clearSelection();
    QGraphicsSceneHoverEvent linkHover(QEvent::GraphicsSceneHoverEnter);
    f.scene()->sendEvent(edgeItem(f, target), &linkHover);
    expect(wireImage() != idleWire && edgeItem(f, target)->data(kNodeHoveredRole).toBool(),
           "wire hover changes its rendered emphasis and hit state");
    QGraphicsSceneHoverEvent linkLeave(QEvent::GraphicsSceneHoverLeave);
    f.scene()->sendEvent(edgeItem(f, target), &linkLeave);
    const auto edgeId = incoming(f, target)->id;
    f.drag(f.socket(second, false)->scenePos(), f.socket(target, true)->scenePos());
    expect(incoming(f, target) && incoming(f, target)->source.nodeId == second &&
               incoming(f, target)->id == edgeId,
           "occupied input rewires and retains its edge ID");
    history = f.stack.size();
    QSignalSpy refusals(&f.session, &CompositionSession::commandRejected);
    f.drag(f.socket(target, false)->scenePos(), f.socket(target, true)->scenePos());
    expect(f.stack.size() == history && !refusals.empty() &&
               incoming(f, target)->source.nodeId == second,
           "cycle refusal preserves the prior link and surfaces a transient session status");
    // ADAPTED (task S7): no fixture-only socket any more. Every parameter role is a real linkable
    // operand socket, so the Layer Output's own Scalar opacity port is what pins the UI's kind
    // refusal -- an Image output dropped on it must paint red and change nothing.
    auto* scalarSocket = [&]() -> node_editor::SocketItem* {
        for (auto* candidate : f.card(target)->sockets()) {
            if (candidate->input &&
                candidate->name == QString::fromUtf8(document::kOpacityParameterRole)) {
                return candidate;
            }
        }
        return nullptr;
    }();
    expect(scalarSocket != nullptr,
           "the Layer Output card carries a Scalar opacity operand socket");
    if (scalarSocket == nullptr)
        return;
    end = scalarSocket->scenePos();
    f.press(f.socket(source, false)->scenePos());
    f.move(end);
    for (auto* item : f.scene()->items())
        if (item->data(kNodeItemKindRole) == QStringLiteral("link-preview"))
            preview = dynamic_cast<QGraphicsPathItem*>(item);
    expect(preview && preview->pen().color() == kit::color(kit::Color::Error),
           "incompatible kind hover paints the link Error red");
    f.release(end);
    expect(f.stack.size() == history && incoming(f, target)->source.nodeId == second,
           "incompatible release leaves graph unchanged");
    f.press(f.socket(target, true)->scenePos());
    f.move({850, 500});
    expect(!edgeItem(f, target)->isVisible() && incoming(f, target),
           "input pickup detaches only the preview until release");
    f.key(Qt::Key_Escape);
    expect(edgeItem(f, target)->isVisible() && f.stack.size() == history,
           "Escape restores a picked-up link without commands");
    f.release({850, 500});
    f.drag(f.socket(target, true)->scenePos(), {900, 600});
    expect(!incoming(f, target) && f.stack.size() == history + 1,
           "input pickup dropped on empty canvas disconnects once");
    expect(f.session.undo() && incoming(f, target), "picked-up disconnect restores in one undo");
    f.drag(f.socket(target, true)->scenePos(), f.socket(pass, true)->scenePos());
    expect(!incoming(f, target) && incoming(f, pass) && incoming(f, pass)->source.nodeId == second,
           "input pickup can transfer its source to another input atomically");
    expect(f.session.undo() && incoming(f, target) && !incoming(f, pass),
           "one undo restores both ends of pickup transfer");
    f.drag(f.socket(source, false)->scenePos(), f.socket(target, true)->scenePos());
    auto midpoint = edgeItem(f, target)->path().pointAtPercent(0.5);
    history = f.stack.size();
    const QPointF press = f.card(pass)->pos() + QPointF(20, 15);
    f.press(press);
    f.move(midpoint);
    f.release(midpoint);
    expect(incoming(f, pass) && incoming(f, pass)->source.nodeId == source && incoming(f, target) &&
               incoming(f, target)->source.nodeId == pass && f.stack.size() == history + 1,
           "dropping a single unconnected Image pair on a wire inserts it in the move transaction");
    expect(f.session.undo() && !incoming(f, pass) && incoming(f, target)->source.nodeId == source,
           "one undo restores layout and both insertion connections");
    const auto cutTarget = f.add(document::kLayerOutputNodeType, {700, 350});
    f.drag(f.socket(second, false)->scenePos(), f.socket(cutTarget, true)->scenePos());
    history = f.stack.size();
    f.press({480, 100}, Qt::ControlModifier, Qt::RightButton);
    f.move({480, 700}, Qt::RightButton, Qt::ControlModifier);
    for (auto* candidate : f.scene()->items()) {
        if (candidate->data(kNodeItemKindRole) == QStringLiteral("cut-preview")) {
            const auto path = static_cast<QGraphicsPathItem*>(candidate)->path();
            expect(path.elementCount() == 2 && path.elementAt(0).x == 480 &&
                       path.elementAt(0).y == 100,
                   "first cut segment starts at the press point rather than the scene origin");
        }
    }
    f.release({480, 700}, Qt::ControlModifier, Qt::RightButton);
    expect(!incoming(f, target) && !incoming(f, cutTarget) && f.stack.size() == history + 1,
           "Ctrl RMB cut disconnects every crossed ordinary link in one transaction");
    expect(f.session.undo() && incoming(f, target) && incoming(f, cutTarget),
           "one cut undo restores all crossed links");
    history = f.stack.size();
    const auto addRevision = f.session.snapshot().revision();
    f.drag(f.socket(source, false)->scenePos(), {950, 650});
    auto* popup = f.editor.findChild<kit::KSearchPopup*>();
    expect(popup && popup->isVisible() && f.stack.size() == history,
           "output released on empty canvas arms Add search without a premature command");
    if (popup) {
        auto* field = popup->findChild<QLineEdit*>();
        field->setText(QStringLiteral("layer output"));
        QTest::keyClick(field, Qt::Key_Return);
        const auto* selected = f.session.selectedNode();
        expect(selected && incoming(f, selected->id) &&
                   incoming(f, selected->id)->source.nodeId == source &&
                   f.session.snapshot().revision().value() == addRevision.value() + 1,
               "choosing from drag-armed search adds and connects one first-compatible port "
               "transaction");
    }
    // ADAPTED (task FIX1, item C): no link is structural any more. The two that could not be
    // detached
    // -- a Layer's boundary output into Merge, and the stack slot it lands in -- are now created by
    // connecting and removed by disconnecting, so the cut gesture reaches them like any other wire
    // and the layer row goes with the slot.
    expect(f.session.addSolidLayer(QStringLiteral("Boundary"), core::Color4d{1, 0, 0, 1}),
           "boundary layer fixture");
    auto* slotLink = static_cast<node_editor::NodeEdgeItem*>(nullptr);
    for (auto* item : f.scene()->items())
        if (auto* edge = dynamic_cast<node_editor::NodeEdgeItem*>(item);
            edge != nullptr &&
            std::holds_alternative<document::LayerStackInputRef>(edge->edge.destination))
            slotLink = edge;
    expect(slotLink != nullptr, "the layer's slot link is projected");
    if (slotLink != nullptr) {
        expect(!slotLink->structural, "a stack-slot link is an ordinary link now");
        expect(slotLink->toolTip().contains(QStringLiteral("Merge")) ||
                   slotLink->toolTip().contains(QStringLiteral("content")),
               "and its tooltip names where it lands");
        const auto slotsBefore = f.session.composition()->graph().layerStack().entries().size();
        midpoint = slotLink->path().pointAtPercent(0.5);
        f.drag(midpoint - QPointF(0, 20), midpoint + QPointF(0, 20), Qt::ControlModifier,
               Qt::RightButton);
        expect(f.session.composition()->graph().layerStack().entries().size() == slotsBefore - 1,
               "cutting a stack-slot link removes the slot it fed");
        expect(f.session.undo() &&
                   f.session.composition()->graph().layerStack().entries().size() == slotsBefore,
               "one undo restores the slot");
    }
}

// Task FOLLOW-1: the Merge card's second, audio-typed stack pill (docs/architecture/
// layer-graph-model.md, Audio Sources And Layers). A Layer's own audio output -- the port the
// existing AddAudioLayer wiring (asset_operations.cpp) already feeds from an Audio source -- shares
// the Layer's existing stack row when dragged onto the pill by hand, rather than opening a second
// one; dropped on the content pill instead, the release refuses the kind mismatch with a specific
// message before any transaction is built.
void testMergeAudioPill() {
    Fixture f;
    const auto merge = f.add(document::kLayerStackNodeType, {650, 300});
    const auto layer = f.add(document::kLayerOutputNodeType, {100, 100});
    const auto audioSource = f.add(document::kAudioSourceNodeType, {100, 400});

    const auto findSocket = [&](const document::NodeId id, const bool input,
                                document::SocketValueKind kind) -> node_editor::SocketItem* {
        for (auto* candidate : f.card(id)->sockets())
            if (candidate->input.has_value() == input && candidate->kind == kind)
                return candidate;
        return nullptr;
    };
    auto* layerAudioInput = findSocket(layer, true, document::SocketValueKind::Audio);
    expect(layerAudioInput != nullptr, "the Layer card carries an audio input socket");
    if (layerAudioInput == nullptr)
        return;
    f.drag(f.socket(audioSource, false)->scenePos(), layerAudioInput->scenePos());

    f.drag(f.socket(layer, false)->scenePos(), f.socket(merge, true)->scenePos());
    const auto entries = f.session.composition()->graph().merge(merge)->entries();
    expect(entries.size() == 1, "the Layer's image edge opens its stack row");
    if (entries.size() != 1)
        return;
    const auto slotId = entries.front().slotId;

    auto* audioPill = findSocket(merge, true, document::SocketValueKind::Audio);
    expect(audioPill != nullptr, "the Merge card exposes a dedicated audio stack pill");
    auto* layerAudioOutput = findSocket(layer, false, document::SocketValueKind::Audio);
    expect(layerAudioOutput != nullptr, "the Layer card carries an audio output socket");
    if (audioPill == nullptr || layerAudioOutput == nullptr)
        return;

    auto history = f.stack.size();
    f.drag(layerAudioOutput->scenePos(), audioPill->scenePos());
    const auto afterAudioDrop = f.session.composition()->graph().edges();
    const auto audioEdge = std::ranges::find_if(afterAudioDrop, [&](const auto& edge) {
        return edge.destination ==
                   document::InputPortRef(document::LayerStackInputRef{
                       merge, slotId, std::string(document::kLayerStackAudioInputRole)}) &&
               edge.source.nodeId == layer;
    });
    expect(audioEdge != afterAudioDrop.end() &&
               f.session.composition()->graph().merge(merge)->entries().size() == 1 &&
               f.stack.size() == history + 1,
           "dragging the Layer's audio output onto the audio pill attaches its edge to the "
           "Layer's own slot rather than opening a second one");

    history = f.stack.size();
    QSignalSpy refusals(&f.session, &CompositionSession::commandRejected);
    f.drag(layerAudioOutput->scenePos(), f.socket(merge, true)->scenePos());
    expect(f.stack.size() == history && refusals.size() == 1 &&
               refusals.constFirst().constFirst().toString() ==
                   QStringLiteral("An Audio output cannot feed the image stack"),
           "an Audio output dropped on the content pill is refused at release with the kind "
           "message and no history entry");
}
} // namespace bloom::ui::test
