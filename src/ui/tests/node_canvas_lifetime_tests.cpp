// The node canvas's own lifetime contract against Qt's scene index.
//
// A QGraphicsScene on BspTreeIndex files every item under its EFFECTIVE scene bounding rectangle --
// the item's own boundingRect() widened by every enabled QGraphicsEffect on the item and on each of
// its ancestors (QGraphicsItemPrivate::effectiveBoundingRect()). The index only ever takes an item
// back out of the BSP leaves that its CURRENT effective rectangle reaches
// (QGraphicsSceneBspTreeIndexPrivate::removeItem -> bsp.removeItem(item,
// sceneEffectiveBoundingRect)), so an item whose effective rectangle shrinks without the scene
// being told FIRST stays behind in the leaves the old rectangle covered. Those leftovers outlive
// the item, and the next scene query or paint walks them: that is the SIGSEGV inside
// QGraphicsSceneBspTree::items() that this file exists to keep fixed.
//
// These cases are in their own executable because they are about item/scene lifetime rather than
// about the gesture and command contracts node_interaction_tests.cpp owns.

#include "node_interaction_test_support.hpp"

#include <bloom/document/graph.hpp>

#include <QCursor>
#include <QGraphicsEffect>
#include <QLineEdit>
#include <QPixmap>
#include <QRectF>

#include <algorithm>
#include <ranges>

namespace bloom::ui::test {
namespace {
// Every item the scene still owns, read through the index's own item list rather than through a
// rectangle query: this is the honest set, because QGraphicsSceneBspTreeIndex::items(order) answers
// from its indexed/unindexed item lists and never from the BSP leaves.
bool sceneStillOwns(const QGraphicsScene& scene, const QGraphicsItem* item) {
    const auto owned = scene.items();
    return std::ranges::find(owned, item) != owned.end();
}

// Every item a rectangle query reports must still be an item of the scene. A rectangle query is the
// one door that answers from the BSP leaves, so this is exactly the invariant a stranded leaf entry
// breaks -- and the invariant whose violation is a dangling pointer once the item is freed.
void expectRectangleQueryAgreesWithTheScene(Fixture& f, const QRectF& rect, const char* message) {
    const auto reported = f.scene()->items(rect);
    const auto stranded = std::ranges::find_if(reported, [&](const QGraphicsItem* item) {
        return item == nullptr || item->scene() != f.scene() || !sceneStillOwns(*f.scene(), item);
    });
    expect(stranded == reported.end(), message);
}
} // namespace

// The drag elevation is the one thing on a node card that changes its effective bounding rectangle
// without changing its geometry: kit's Elevation::Drag shadow is installed once and only ENABLED
// while a drag is in flight. Flipping that flag has to be announced to the scene BEFORE the
// rectangle shrinks, because Qt's own notification (QGraphicsEffect::setEnabled ->
// effectBoundingRectChanged -> prepareGeometryChange) arrives only AFTER the flag has already
// flipped, which is too late for the index to find the leaves it has to clean.
void testDragElevationDoesNotStrandSceneIndexEntries() {
    Fixture f;
    const auto text = f.add(document::kTextSourceNodeType, {320.0, 220.0});
    auto* card = f.card(text);
    if (card == nullptr) {
        expect(false, "the added text node has a card");
        return;
    }
    const auto* elevation = card->graphicsEffect();
    if (elevation == nullptr) {
        expect(false, "a node card carries the drag elevation effect");
        return;
    }

    // A scene rectangle the test owns, so that the BSP tree's own leaf boundaries are known: Qt
    // initializes the tree over the scene rectangle and splits it horizontally first, at its
    // vertical centre (QGraphicsSceneBspTree::initialize).
    const QRectF sceneRect(0.0, 0.0, 2048.0, 1024.0);
    f.scene()->setSceneRect(sceneRect);
    const qreal split = sceneRect.center().y();

    // Park the card so the split line falls halfway into the shadow's reach below it: the card's
    // own rectangle stays clear of the boundary while the elevated rectangle crosses it. The leaves
    // under the split are then reachable ONLY through the elevated rectangle, which makes them
    // exactly the leaves an unannounced shrink strands.
    const QRectF local = card->boundingRect();
    const QRectF elevated = elevation->boundingRectFor(local);
    const qreal reach = elevated.bottom() - local.bottom();
    expect(reach > 0.0, "the drag elevation hangs below the card it lifts");
    card->setPos(sceneRect.center().x() - local.width() / 2.0,
                 split - local.bottom() - reach / 2.0);

    // A rectangle query is what builds the BSP tree and files every item into its leaves.
    const auto flushIndex = [&] { (void)f.scene()->items(sceneRect); };
    flushIndex();

    // The drag begins: card and children are re-filed under the widened rectangle, which reaches
    // across the split.
    card->setDragging(true);
    flushIndex();

    // The drag ends. Without the announcement this is where the leaves below the split are
    // stranded.
    card->setDragging(false);

    // The scene is asked to give the card up while it is unindexed -- exactly what a projection
    // does to a card the graph no longer has, and what buildSockets() does to the sockets it
    // replaces. An item the index has at index -1 is only taken off its unindexed list, so anything
    // still sitting in a leaf stays there, pointing at an item the scene no longer owns (and, once
    // it is freed, at freed memory).
    f.scene()->removeItem(card);
    expect(card->scene() == nullptr, "a removed card is out of the scene");
    const auto reported = f.scene()->items(sceneRect);
    expect(std::ranges::find(reported, static_cast<QGraphicsItem*>(card)) == reported.end(),
           "a card removed from the scene is gone from the scene's spatial index too");
    expectRectangleQueryAgreesWithTheScene(
        f, sceneRect, "no item the scene gave up is left behind in its spatial index");

    // The card is the test's to delete now that the scene has given it up: its children go with it,
    // and nothing may be left pointing at any of them.
    delete card;
    expectRectangleQueryAgreesWithTheScene(
        f, sceneRect, "a deleted card leaves nothing behind in the scene's spatial index");
}

// The product owner's own gesture, end to end: a text layer added through the canvas's add search,
// then dragged and dropped, which submits a transaction and rebuilds the whole projection --
// freeing every socket on every card and every edge in the scene -- under a pointer the canvas
// believes it has, and then queried and painted.
//
// This is coverage of the reported path, not the regression detector: which BSP leaf a stranded
// entry lands in depends on where the cards happen to sit in the scene rectangle, and the
// projection resets that rectangle on every rebuild (which makes Qt regenerate the tree and quietly
// drop the stranded entries). The case above is what actually pins the contract; this one walks the
// whole reported gesture over it, and is the case a sanitizer run has something to chew on.
void testAddingAndDraggingATextLayerSurvivesTheRebuild() {
    Fixture f;
    const QPointF cursor(320.0, 220.0);
    const auto global = f.view()->viewport()->mapToGlobal(f.view()->mapFromScene(cursor));
    f.editor.openAddSearch(cursor, global);
    auto* popup = f.editor.findChild<kit::KSearchPopup*>();
    if (popup == nullptr) {
        expect(false, "the canvas opens its add search");
        return;
    }
    auto* field = popup->findChild<QLineEdit*>(QStringLiteral("kSearchFilter"));
    if (field == nullptr) {
        expect(false, "the add search carries its filter field");
        return;
    }
    field->setText(QStringLiteral("text"));
    QTest::keyClick(field, Qt::Key_Return);
    const auto* const selected = f.session.selectedNode();
    if (selected == nullptr || selected->typeId != document::kTextSourceNodeType) {
        expect(false, "the add search adds a text layer");
        return;
    }
    auto* card = f.card(selected->id);
    if (card == nullptr) {
        expect(false, "the new text layer has a card");
        return;
    }

    // Qt asks every view UNDER THE POINTER what lies beneath it whenever an item's cursor is set
    // (QGraphicsItem::setCursor -> QGraphicsView::items), and NodeItem::setAuthoringEnabled() sets
    // a cursor on every card of every rebuild. That is the exact door the product crashed through,
    // from inside the projection itself and before it had finished. Offscreen there is no real
    // pointer, so the canvas is told where one is and that it has it.
    QCursor::setPos(global);
    f.view()->viewport()->setAttribute(Qt::WA_UnderMouse, true);

    const auto sceneRect = f.scene()->sceneRect();
    (void)f.scene()->items(sceneRect);

    // Drag the card by its header and drop it. The release elevates nothing any more, cancels the
    // gesture and submits the move, which rebuilds the projection underneath.
    const QPointF grab = card->pos() + QPointF(card->cardWidth() / 2.0, 8.0);
    f.press(grab);
    f.move(grab + QPointF(140.0, 90.0));
    (void)f.scene()->items(sceneRect);
    f.move(grab + QPointF(180.0, 120.0));
    f.release(grab + QPointF(180.0, 120.0));

    expectRectangleQueryAgreesWithTheScene(
        f, f.scene()->sceneRect(),
        "the rebuild a dropped text layer triggers leaves no stale entry in the scene's index");

    // The paint path reads the same leaves, through QGraphicsView::paintEvent.
    QPixmap surface(f.view()->viewport()->size());
    surface.fill(Qt::black);
    f.view()->viewport()->render(&surface);
    QCoreApplication::processEvents();
    expectRectangleQueryAgreesWithTheScene(
        f, f.scene()->sceneRect(), "and the canvas is still consistent after it has been painted");
}
} // namespace bloom::ui::test

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        // The end-to-end gesture first: the focused case below deliberately frees a card, so it is
        // the one that would take the process down if the contract it pins were ever broken again.
        bloom::ui::test::testAddingAndDraggingATextLayerSurvivesTheRebuild();
        bloom::ui::test::testDragElevationDoesNotStrandSceneIndexEntries();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return bloom::ui::test::failures == 0 ? 0 : 1;
}
