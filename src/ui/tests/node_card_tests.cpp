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
    expect(hosted.size() == 3, "the layer output card hosts its X, Y and opacity fields");
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
} // namespace bloom::ui::test

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        bloom::ui::test::testHostedFieldsAreFullWidthAndUnscaled();
        bloom::ui::test::testCardMinimumWidthIsItsContent();
        bloom::ui::test::testSocketsBrightenAndDimDuringALinkDrag();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return bloom::ui::test::failures == 0 ? 0 : 1;
}
