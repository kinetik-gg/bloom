// Task S1: the node card's own primitives -- what an in-card field looks like, how narrow a card
// may get, and what a socket is. Its own executable rather than more cases inside
// node_interaction_tests.cpp, which owns the gesture/command contract this task keeps intact.

#include "node_interaction_test_support.hpp"

#include <QFontMetricsF>
#include <QGraphicsProxyWidget>

#include <algorithm>
#include <cmath>

namespace bloom::ui::test {
namespace {
using node_editor::kCardLabelGap;
using node_editor::kCardMinimumWidth;
using node_editor::kCardPadding;
using node_editor::NodeItem;

// Every kit control the card hosts, with the proxy that carries it.
std::vector<std::pair<QGraphicsProxyWidget*, QWidget*>> hostedControls(NodeItem& card) {
    std::vector<std::pair<QGraphicsProxyWidget*, QWidget*>> hosted;
    for (auto* child : card.childItems()) {
        auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(child);
        if (proxy != nullptr && proxy->widget() != nullptr && proxy->isVisible())
            hosted.emplace_back(proxy, proxy->widget());
    }
    return hosted;
}
} // namespace

// Item 1. A hosted field is the card's own row: full width inside the card's padding, at its own
// unscaled size, with nothing opaque behind it.
void testHostedFieldsAreFullWidthAndUnscaled() {
    Fixture f;
    const auto boundary = f.add(document::kLayerOutputNodeType, {100, 100});
    auto* card = f.card(boundary);
    expect(card != nullptr, "the layer output card exists");
    if (card == nullptr)
        return;
    const auto hosted = hostedControls(*card);
    // ADAPTED (task S4): the Layer Output card now hosts position X/Y, anchor X/Y, scale X/Y,
    // rotation and opacity -- eight fields, one per transform component plus opacity.
    expect(hosted.size() == 8, "the layer output card hosts its transform and opacity fields");
    for (const auto& [proxy, widget] : hosted) {
        expect(proxy->scale() == 1.0,
               "a hosted control is never scaled: a fractional scale resampled its hairlines, "
               "padding and text into a blurred, smaller copy of itself");
        expect(widget->testAttribute(Qt::WA_TranslucentBackground) &&
                   widget->testAttribute(Qt::WA_NoSystemBackground),
               "and it paints no opaque window background behind its own cell");
        expect(widget->height() == kit::px(kit::Size::Control),
               "a value cell is exactly the control height -- no strip above or below it that "
               "nothing paints");
        const qreal right = proxy->pos().x() + static_cast<qreal>(widget->width());
        expect(std::abs(right - (card->cardWidth() - kCardPadding)) <= 1.0,
               "and it reaches the card's own padding on the right: the span is ceiled, so no "
               "sliver of card surface is left beside a full-width field");
    }
    auto* field = qobject_cast<kit::KValueField*>(
        f.scene()->nodeFieldForTest(boundary, QStringLiteral("nodePositionXEditor")));
    expect(field != nullptr, "the X cell is a kit value field");
    if (field == nullptr)
        return;
    expect(field->cellRect().top() == 0.0 &&
               field->cellRect().height() == static_cast<qreal>(field->height()),
           "the cell spans the whole control, so the card shows no darker band around it");
    expect(field->cellRect().left() == 0.0 &&
               field->cellRect().right() == static_cast<qreal>(field->width()),
           "a card field carries no label column of its own: the card paints the row's name");
}

// Item 2. The card's floor is its content. Nothing it carries is ever clipped by it, and the header
// never reaches the rows the sockets live in.
void testCardMinimumWidthIsItsContent() {
    Fixture f;
    const auto boundary = f.add(document::kLayerOutputNodeType, {100, 100});
    auto* card = f.card(boundary);
    expect(card != nullptr, "the layer output card exists");
    if (card == nullptr)
        return;

    // Asked for an impossible width: the card refuses to go below its own content.
    card->setPreviewWidth(1.0);
    const qreal minimum = card->minimumCardWidth();
    expect(minimum >= kCardMinimumWidth,
           "the content floor never drops below the card's spelled minimum");
    expect(card->cardWidth() == minimum, "and a narrower requested width is raised to it");

    const QFontMetricsF rowMetrics(kit::font(kit::TypeRole::UiSmall));
    for (const auto& [proxy, widget] : hostedControls(*card)) {
        expect(proxy->pos().x() >= kCardPadding + card->labelColumnWidth() + kCardLabelGap - 0.5,
               "at the minimum width the label column still fits beside every control");
        expect(widget->width() >= widget->sizeHint().width(),
               "and no control is narrower than the narrowest width it can be used at");
        expect(proxy->pos().x() + static_cast<qreal>(widget->width()) <=
                   card->cardWidth() - kCardPadding + 1.0,
               "nor does one run past the card's padding");
    }
    for (const auto* socket : card->sockets()) {
        expect(rowMetrics.horizontalAdvance(socket->name) + 2.0 * kCardPadding <= card->cardWidth(),
               "every socket name fits inside the card's padding at the minimum width");
        expect(socket->pos().y() - socket->rowHeight() / 2.0 >=
                   node_editor::kCardHeaderHeight - 0.001,
               "and no socket row reaches up into the header, so the header never overlaps one");
    }

    // A resize gesture clamps against the same floor rather than a spelled 128.
    const qreal edge = card->pos().x() + card->cardWidth();
    f.drag({edge, card->pos().y() + 10}, {card->pos().x() - 400, card->pos().y() + 10});
    expect(card->cardWidth() == minimum, "a resize drag cannot take the card below its content");
}
// Item 6. A socket and the link leaving it take the socket palette, and a link drag in flight says
// where it could land: compatible sockets brighten, incompatible ones fade.
void testSocketsBrightenAndDimDuringALinkDrag() {
    Fixture f;
    const auto source = f.add(document::kSolidSourceNodeType, {100, 100});
    const auto target = f.add(document::kLayerOutputNodeType, {600, 100});
    auto* output = f.socket(source, false);
    auto* input = f.socket(target, true);
    auto* targetOutput = f.socket(target, false);
    expect(output != nullptr && input != nullptr && targetOutput != nullptr,
           "drag fixture sockets");
    if (output == nullptr || input == nullptr || targetOutput == nullptr)
        return;

    const QColor resting = kit::color(kit::Color::SocketImage);
    expect(output->paintedInk() == resting,
           "at rest a socket paints exactly its transport kind's token");
    expect(output->dragAffinity() == node_editor::SocketItem::DragAffinity::Idle &&
               input->dragAffinity() == node_editor::SocketItem::DragAffinity::Idle,
           "and no drag is in flight");

    f.press(output->scenePos());
    expect(input->dragAffinity() == node_editor::SocketItem::DragAffinity::Compatible &&
               input->paintedInk() == kit::hoverFillFor(resting),
           "a socket the dragged link could land on brightens toward Foreground");
    expect(targetOutput->dragAffinity() == node_editor::SocketItem::DragAffinity::Incompatible &&
               targetOutput->paintedInk() == kit::withOpacity(resting, kit::kDisabledOpacity),
           "one it could not fades to the disabled ink");
    expect(output->dragAffinity() == node_editor::SocketItem::DragAffinity::Idle &&
               output->paintedInk() == resting,
           "and the socket in the artist's hand keeps its resting ink -- it is not a target");

    f.release(input->scenePos());
    expect(output->dragAffinity() == node_editor::SocketItem::DragAffinity::Idle &&
               input->dragAffinity() == node_editor::SocketItem::DragAffinity::Idle &&
               input->paintedInk() == resting,
           "ending the drag puts every socket back to its resting ink");
}
// Item 7, terminology. The four renamed types read as what they are, and their ids are untouched.
void testNodeTypesAreNamedForWhatTheyAre() {
    expect(node_editor::nodeTypeDisplayName(document::kSolidSourceNodeType) ==
               QStringLiteral("Solid"),
           "Solid Source is just Solid");
    expect(node_editor::nodeTypeDisplayName(document::kLayerOutputNodeType) ==
               QStringLiteral("Layer"),
           "a Layer Output is a Layer");
    expect(node_editor::nodeTypeDisplayName(document::kLayerStackNodeType) ==
               QStringLiteral("Merge"),
           "the Layer Stack is Merge");
    expect(node_editor::nodeTypeDisplayName(document::kCompositionOutputNodeType) ==
               QStringLiteral("Output"),
           "the Composition Output is Output");
    // Anything unnamed still falls back to the spelled-out identifier, and a parameter role --
    // which is what displayTypeName() is for -- is untouched by any of this.
    expect(node_editor::nodeTypeDisplayName(document::kTextSourceNodeType) ==
               node_editor::displayTypeName(document::kTextSourceNodeType),
           "an unnamed type keeps the spelled-out fallback");
    expect(document::kLayerStackNodeType == std::string_view{"bloom.layer-stack"} &&
               document::kCompositionOutputNodeType == std::string_view{"bloom.composition-output"},
           "and the type ids themselves are unchanged: this is vocabulary, not identity");

    Fixture f;
    expect(f.session.addSolidLayer(QStringLiteral("Backdrop"), core::Color4d{1, 0, 0, 1}),
           "layer fixture");
    const auto layerId = f.session.composition()->graph().layerOutputs().front().layerId;
    const auto boundary = f.session.boundaryNodeForLayer(layerId);
    const auto* record = boundary ? f.session.composition()->graph().findNode(*boundary) : nullptr;
    expect(record != nullptr, "the layer resolves its boundary node");
    if (record == nullptr)
        return;
    expect(node_editor::nodeDisplayName(*f.session.composition(), *record) ==
               QStringLiteral("Backdrop"),
           "a layer card is named after its layer, not after its node type");
    expect(node_editor::nodeEyebrow(*f.session.composition(), *record) == QStringLiteral("Layer"),
           "so the eyebrow is what still says it is a Layer");
    const auto* stack = f.session.composition()->graph().findNode(
        f.session.composition()->graph().layerStack().nodeId());
    expect(node_editor::nodeEyebrow(*f.session.composition(), *stack).isEmpty(),
           "and a node named after its own type carries no eyebrow");
}

// Item 7, Merge. One ordered multi-input for the whole stack, with the slot model untouched
// beneath.
void testMergeRendersOneOrderedMultiInput() {
    Fixture f;
    expect(f.session.addSolidLayer(QStringLiteral("Lower"), core::Color4d{1, 0, 0, 1}) &&
               f.session.addSolidLayer(QStringLiteral("Upper"), core::Color4d{0, 1, 0, 1}),
           "two layer fixtures");
    const auto stackId = f.session.composition()->graph().layerStack().nodeId();
    const auto entries = f.session.composition()->graph().layerStack().entries();
    expect(entries.size() == 2, "the stack really holds two slots");
    auto* card = f.card(stackId);
    expect(card != nullptr, "the Merge card exists");
    if (card == nullptr || entries.size() != 2)
        return;

    std::vector<node_editor::SocketItem*> inputs;
    for (auto* socket : card->sockets())
        if (socket->input.has_value())
            inputs.push_back(socket);
    expect(inputs.size() == 1, "two layers are ONE port on Merge, not two repeated content rows");
    if (inputs.size() != 1)
        return;
    auto* pill = inputs.front();
    expect(pill->multiInput() && pill->orderedInputs().size() == entries.size(),
           "and that port stands for every slot the stack holds");
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const document::InputPortRef expected = document::LayerStackInputRef{
            stackId, entries[index].slotId, std::string(document::kLayerStackContentInputRole)};
        expect(pill->orderedInputs().at(index) == expected,
               "in the stack's own order, read straight off the slot model");
        expect(pill->accepts(expected),
               "and every one of those slots' edges terminates on this one socket");
    }
    expect(pill->pillLength() ==
               std::max(node_editor::kSocketRowHeight,
                        static_cast<qreal>(entries.size()) * node_editor::kStackSlotPitch),
           "the pill's length is the stack's depth");
    expect(pill->rowHeight() >= pill->pillLength(),
           "and the card gives that port a row tall enough to hold it");
    expect(pill->toolTip().contains(QStringLiteral("Ordered multi-input")),
           "the port says it is ordered");

    // Every stack edge is still projected, and still lands on this socket.
    int wires = 0;
    for (const auto* item : f.scene()->items())
        if (dynamic_cast<const node_editor::NodeEdgeItem*>(item) != nullptr)
            ++wires;
    expect(wires == static_cast<int>(f.session.composition()->graph().edges().size()),
           "collapsing the rows loses no link: one wire per graph edge, as before");

    // A link drag over the pill names the position in the order the pointer is at, while the pill
    // itself is dimmed -- a stack slot is structural and accepts no drop.
    const auto solid = f.session.directSourceNodeForLayer(entries.front().layerId);
    expect(solid.has_value(), "the lower layer resolves its source node");
    if (!solid.has_value())
        return;
    auto* output = f.socket(*solid, false);
    expect(output != nullptr, "the drag fixture has a draggable output");
    if (output == nullptr)
        return;
    f.press(output->scenePos());
    const QPointF overSecond =
        pill->scenePos() +
        QPointF(0, pill->pillLength() / 2.0 - node_editor::kStackSlotPitch / 2.0);
    f.move(overSecond);
    expect(pill->dropIndicator().has_value() &&
               *pill->dropIndicator() == pill->orderedInputs().size() - 1,
           "the indicator names the slot under the pointer");
    expect(pill->dragAffinity() == node_editor::SocketItem::DragAffinity::Incompatible,
           "while the pill stays dimmed: the caret reports a position, never a landing");
    f.release(overSecond);
    expect(!pill->dropIndicator().has_value(),
           "and ending the drag clears the indicator with the gesture");
}
// Item 8. Two ways into a layer card's name -- Enter and a double-click -- and a card with no name
// of its own refuses both.
void testEnterAndDoubleClickRenameALayerCard() {
    Fixture f;
    expect(f.session.addSolidLayer(QStringLiteral("Before"), core::Color4d{1, 0, 0, 1}),
           "layer fixture");
    const auto layerId = f.session.composition()->graph().layerOutputs().front().layerId;
    const auto boundary = f.session.boundaryNodeForLayer(layerId);
    expect(boundary.has_value(), "the layer resolves its boundary node");
    if (!boundary.has_value())
        return;

    f.session.selectNode(*boundary);
    f.key(Qt::Key_Return);
    auto* field = qobject_cast<QLineEdit*>(
        f.scene()->nodeFieldForTest(*boundary, QStringLiteral("nodeRenameEditor")));
    expect(field != nullptr, "Enter opens the layer card's inline name field");
    if (field == nullptr)
        return;
    field->setText(QStringLiteral("Renamed by Enter"));
    QTest::keyClick(field, Qt::Key_Return);
    expect(f.session.composition()->graph().layerOutputs().front().name == "Renamed by Enter",
           "and committing it executes RenameLayer");

    // The committed field is retired with deleteLater(), so the event loop has to run before the
    // card can be asked about its rename field again -- otherwise the stale, hidden one answers.
    QCoreApplication::processEvents();

    // A double-click on the card itself is the pointer route to the same field.
    auto* card = f.card(*boundary);
    expect(card != nullptr, "the layer card exists");
    if (card == nullptr)
        return;
    const QPointF header = card->pos() + QPointF(card->cardWidth() / 2.0, 8.0);
    f.mouse(QEvent::MouseButtonDblClick, header, Qt::LeftButton, Qt::LeftButton);
    auto* reopened = qobject_cast<QLineEdit*>(
        f.scene()->nodeFieldForTest(*boundary, QStringLiteral("nodeRenameEditor")));
    expect(reopened != nullptr && reopened->graphicsProxyWidget() != nullptr &&
               reopened->graphicsProxyWidget()->isVisible() && reopened != field,
           "a double-click on a layer card opens the same field");

    // A card that is not a layer boundary has no name of its own, and neither route invents one.
    const auto stackId = f.session.composition()->graph().layerStack().nodeId();
    f.session.selectNode(stackId);
    f.key(Qt::Key_Return);
    expect(f.scene()->nodeFieldForTest(stackId, QStringLiteral("nodeRenameEditor")) == nullptr,
           "Enter on a node with no layer name of its own opens nothing");
    auto* stackCard = f.card(stackId);
    if (stackCard == nullptr)
        return;
    f.mouse(QEvent::MouseButtonDblClick,
            stackCard->pos() + QPointF(stackCard->cardWidth() / 2.0, 8.0), Qt::LeftButton,
            Qt::LeftButton);
    expect(f.scene()->nodeFieldForTest(stackId, QStringLiteral("nodeRenameEditor")) == nullptr,
           "and neither does a double-click on it");
}
} // namespace bloom::ui::test

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        bloom::ui::test::testHostedFieldsAreFullWidthAndUnscaled();
        bloom::ui::test::testCardMinimumWidthIsItsContent();
        bloom::ui::test::testSocketsBrightenAndDimDuringALinkDrag();
        bloom::ui::test::testNodeTypesAreNamedForWhatTheyAre();
        bloom::ui::test::testMergeRendersOneOrderedMultiInput();
        bloom::ui::test::testEnterAndDoubleClickRenameALayerCard();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return bloom::ui::test::failures == 0 ? 0 : 1;
}
