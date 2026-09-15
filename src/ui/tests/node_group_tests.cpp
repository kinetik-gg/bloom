// Task S2, item 3: node group frames in the editor -- what one is painted as, what dragging one
// does, what dropping a card into or out of one does, and what the keyboard and the menus offer.
// Its own executable rather than more cases inside node_interaction_tests.cpp, which owns the
// gesture/command contract this task keeps intact.

#include "node_interaction_test_support.hpp"

#include <QGraphicsItem>
#include <QLineEdit>

#include <algorithm>

namespace bloom::ui::test {
namespace {
using node_editor::kGroupTitleHeight;
using node_editor::NodeGroupItem;
using node_editor::NodeItem;

const document::NodeGroups& groupsOf(Fixture& f) { return f.session.composition()->nodeGroups(); }

// The one group the fixture has authored, or an invalid id.
document::NodeGroupId onlyGroup(Fixture& f) {
    const auto& groups = groupsOf(f);
    return groups.empty() ? document::NodeGroupId{} : groups.begin()->first;
}

NodeGroupItem* frame(Fixture& f, const document::NodeGroupId id) {
    return dynamic_cast<NodeGroupItem*>(f.scene()->findNodeGroupItem(id));
}

// The union of the cards' own rectangles in scene coordinates: what a frame is computed from.
QRectF cardBounds(Fixture& f, const std::vector<document::NodeId>& nodes) {
    QRectF bounds;
    bool first = true;
    for (const auto id : nodes) {
        auto* card = f.card(id);
        if (card == nullptr)
            continue;
        const QRectF rect = card->mapRectToScene(card->cardRect());
        bounds = first ? rect : bounds.united(rect);
        first = false;
    }
    return bounds;
}

QAction* actionNamed(const QMenu& menu, const QString& objectName) {
    const auto actions = menu.actions();
    const auto found = std::ranges::find_if(
        actions, [&objectName](const auto* action) { return action->objectName() == objectName; });
    return found == actions.end() ? nullptr : *found;
}
} // namespace

// The frame itself: behind its members, sized from their bounding rectangle plus the record's own
// padding with room for the title strip, and following them as they move.
void testFrameIsPaintedBehindItsMembers() {
    Fixture f;
    const auto a = f.add(document::kSolidSourceNodeType, {100, 100});
    const auto b = f.add(document::kSolidSourceNodeType, {400, 260});
    f.session.selectNodes({a, b}, a);
    f.key(Qt::Key_G, Qt::ControlModifier);
    expect(groupsOf(f).size() == 1, "Ctrl+G groups the selection");
    const auto id = onlyGroup(f);
    auto* item = frame(f, id);
    expect(item != nullptr, "the group is projected as a frame item");
    if (item == nullptr)
        return;
    expect(item->members() == std::set{a, b}, "the frame knows what it frames");
    expect(item->zValue() < f.card(a)->zValue(),
           "the frame sits behind the cards it frames, not over them");
    const auto padding = groupsOf(f).at(id).padding;
    const auto expected = cardBounds(f, {a, b}).adjusted(-padding.x, -padding.y - kGroupTitleHeight,
                                                         padding.x, padding.y);
    expect(item->frameRect() == expected,
           "the frame is the member bounding rectangle plus its padding and title strip");
    expect(item->titleRect().height() == kGroupTitleHeight &&
               item->title() == QStringLiteral("Group"),
           "the frame carries its own title strip and name");

    // While ONE member is being dragged, the frame is computed from the members that are holding
    // still -- so it does not chase the card being taken out of it, and the artist can see the
    // rectangle the drop will be judged against.
    f.session.selectNodes({b}, b);
    const auto settled = cardBounds(f, {a}).adjusted(-padding.x, -padding.y - kGroupTitleHeight,
                                                     padding.x, padding.y);
    f.press(f.card(b)->pos() + QPointF(20, 15));
    f.move(f.card(b)->pos() + QPointF(60, 15));
    expect(frame(f, id)->frameRect() == settled,
           "a frame holds still around its settled members while one is dragged out of it");
    f.key(Qt::Key_Escape);
    expect(frame(f, id)->frameRect() == expected, "cancelling the drag restores the frame");
    f.release(f.card(b)->pos() + QPointF(60, 15));
}

// Dragging the frame moves every member, as one MoveNodes and therefore one undo, and changes no
// membership: the whole frame travelled, so nothing left it.
void testDraggingTheFrameMovesEveryMember() {
    Fixture f;
    const auto a = f.add(document::kSolidSourceNodeType, {100, 100});
    const auto b = f.add(document::kSolidSourceNodeType, {380, 100});
    f.session.selectNodes({a, b}, a);
    f.key(Qt::Key_G, Qt::ControlModifier);
    const auto id = onlyGroup(f);
    auto* item = frame(f, id);
    expect(item != nullptr, "the frame exists");
    if (item == nullptr)
        return;

    // A point inside the frame but on no card: its own title strip.
    const QPointF grip = item->frameRect().topLeft() + QPointF(12, kGroupTitleHeight / 2);
    f.session.clearSelection();
    f.click(grip);
    expect(f.session.selectedNodes() == std::set{a, b}, "clicking the frame selects its members");

    const auto before = f.stack.size();
    const auto positionA = f.card(a)->pos();
    const auto positionB = f.card(b)->pos();
    const auto framedBefore = item->frameRect();
    f.press(grip);
    f.move(grip + QPointF(40, 30));
    expect(f.stack.size() == before && f.card(a)->pos() == positionA + QPointF(40, 30) &&
               f.card(b)->pos() == positionB + QPointF(40, 30),
           "the frame drag previews every member without per-pixel commands");
    f.release(grip + QPointF(40, 30));
    expect(f.stack.size() == before + 1 &&
               f.session.composition()->nodeLayout().at(a).position ==
                   document::Vec2d{positionA.x() + 40, positionA.y() + 30} &&
               f.session.composition()->nodeLayout().at(b).position ==
                   document::Vec2d{positionB.x() + 40, positionB.y() + 30},
           "one MoveNodes publishes every member's new position");
    expect(groupsOf(f).at(id).members == std::set{a, b},
           "moving the whole frame changes no membership");
    expect(frame(f, id)->frameRect() == framedBefore.translated(40, 30),
           "the frame travels with the members it carried");
    expect(f.session.undo() && f.card(a)->pos() == positionA && f.card(b)->pos() == positionB,
           "one undo restores the whole frame's move");
}

// Membership by drop: a card whose center lands inside a frame joins it, and one dragged out of it
// leaves -- each in the same transaction as the move, so each is one undo.
void testMembershipFollowsWhereACardIsDropped() {
    Fixture f;
    const auto a = f.add(document::kSolidSourceNodeType, {100, 100});
    const auto b = f.add(document::kSolidSourceNodeType, {100, 520});
    const auto outside = f.add(document::kSolidSourceNodeType, {700, 100});
    f.session.selectNodes({a, b}, a);
    f.key(Qt::Key_G, Qt::ControlModifier);
    const auto id = onlyGroup(f);
    expect(frame(f, id) != nullptr, "the frame exists");
    if (frame(f, id) == nullptr)
        return;

    // Into the frame: aim the outside card's center at the middle of the frame.
    const auto target = frame(f, id)->frameRect().center();
    auto* card = f.card(outside);
    const QPointF grab = card->pos() + QPointF(20, 15);
    const QPointF centerOffset = card->mapToScene(card->cardRect().center()) - grab;
    const auto before = f.stack.size();
    f.drag(grab, target - centerOffset);
    expect(f.stack.size() == before + 1 && groupsOf(f).at(id).members == std::set{a, b, outside},
           "a card dropped with its center inside a frame joins the group");
    expect(f.session.undo() && groupsOf(f).at(id).members == std::set{a, b},
           "one undo takes back both the move and the membership");
    expect(f.session.redo() && groupsOf(f).at(id).members == std::set{a, b, outside},
           "redo restores both together");

    // Out of the frame: the frame holds still while a member is dragged out of it, so the card can
    // actually land outside the rectangle the artist is looking at.
    auto* leaving = f.card(outside);
    const QPointF leave = leaving->pos() + QPointF(20, 15);
    const auto moved = f.stack.size();
    f.drag(leave, leave + QPointF(620, 420));
    expect(f.stack.size() == moved + 1 && groupsOf(f).at(id).members == std::set{a, b},
           "a card dropped outside every frame leaves the group it was in");
    expect(f.session.undo() && groupsOf(f).at(id).members == std::set{a, b, outside},
           "one undo puts it back in the group it left");
}

// The title is renamed in place, through RenameGroup, from the frame's own inline editor.
void testTheTitleIsRenamedInline() {
    Fixture f;
    const auto a = f.add(document::kSolidSourceNodeType, {100, 100});
    f.session.selectNodes({a}, a);
    f.key(Qt::Key_G, Qt::ControlModifier);
    const auto id = onlyGroup(f);
    auto* item = frame(f, id);
    expect(item != nullptr, "the frame exists");
    if (item == nullptr)
        return;
    const QPointF title = item->frameRect().topLeft() + QPointF(12, kGroupTitleHeight / 2);
    f.mouse(QEvent::MouseButtonDblClick, title, Qt::LeftButton, Qt::LeftButton);
    auto* editor = item->findChild<QLineEdit*>(QStringLiteral("nodeGroupRenameEditor"));
    if (editor == nullptr)
        for (auto* child : item->childItems())
            if (auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(child))
                editor = qobject_cast<QLineEdit*>(proxy->widget());
    expect(editor != nullptr, "a double-click on the frame opens its inline title editor");
    if (editor == nullptr)
        return;
    expect(editor->text() == QStringLiteral("Group"), "the editor starts from the current name");
    // A press inside the open editor belongs to the editor, not to the canvas: the frame's body is
    // a drag grip and a box selection would otherwise start under the caret.
    f.session.selectNodes({a}, a);
    f.press(title);
    const auto liveEditor = [&] {
        for (auto* child : item->childItems())
            if (auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(child))
                if (proxy->isVisible() && qobject_cast<QLineEdit*>(proxy->widget()) != nullptr)
                    return true;
        return false;
    };
    expect(liveEditor() && !f.scene()->gestureActive(),
           "a press inside the open title editor starts no canvas gesture and keeps the editor");
    f.release(title);
    const auto before = f.stack.size();
    editor->setText(QStringLiteral("Key Light"));
    Q_EMIT editor->editingFinished();
    QCoreApplication::processEvents();
    expect(f.stack.size() == before + 1 && groupsOf(f).at(id).name == "Key Light",
           "committing the editor publishes one RenameGroup");
    expect(frame(f, id) != nullptr && frame(f, id)->title() == QStringLiteral("Key Light"),
           "the frame shows the name it was given");
}

// Ctrl+Shift+G, and what each menu offers.
void testUngroupingAndMenus() {
    Fixture f;
    const auto a = f.add(document::kSolidSourceNodeType, {100, 100});
    const auto b = f.add(document::kSolidSourceNodeType, {400, 100});
    f.session.selectNodes({a, b}, a);

    {
        const auto menu = std::unique_ptr<QMenu>(f.editor.contextMenuForTest(true));
        expect(actionNamed(*menu, QStringLiteral("nodeGroupAction")) != nullptr,
               "the selection menu offers Group");
        expect(actionNamed(*menu, QStringLiteral("nodeUngroupAction")) == nullptr,
               "an ungrouped selection is not offered Ungroup");
    }
    f.key(Qt::Key_G, Qt::ControlModifier);
    const auto id = onlyGroup(f);
    expect(id.isValid(), "the group exists");
    {
        const auto menu = std::unique_ptr<QMenu>(f.editor.contextMenuForTest(true));
        expect(actionNamed(*menu, QStringLiteral("nodeUngroupAction")) != nullptr,
               "a grouped selection is offered Ungroup");
        const auto frameMenu = std::unique_ptr<QMenu>(f.editor.contextMenuForTest(false, id));
        expect(actionNamed(*frameMenu, QStringLiteral("nodeUngroupAction")) != nullptr &&
                   actionNamed(*frameMenu, QStringLiteral("nodeGroupRenameAction")) != nullptr &&
                   frameMenu->actions().size() == 2,
               "a frame's own menu offers exactly Ungroup and Rename");
    }
    const auto layout = f.session.composition()->nodeLayout();
    const auto before = f.stack.size();
    f.key(Qt::Key_G, Qt::ControlModifier | Qt::ShiftModifier);
    expect(f.stack.size() == before + 1 && groupsOf(f).empty(), "Ctrl+Shift+G ungroups");
    expect(f.session.composition()->nodeLayout() == layout &&
               f.session.composition()->graph().findNode(a) != nullptr &&
               f.scene()->findNodeGroupItem(id) == nullptr,
           "ungrouping removes only the frame");
    expect(f.session.undo() && groupsOf(f).size() == 1, "one undo restores the group");
}
} // namespace bloom::ui::test

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        bloom::ui::test::testFrameIsPaintedBehindItsMembers();
        bloom::ui::test::testDraggingTheFrameMovesEveryMember();
        bloom::ui::test::testMembershipFollowsWhereACardIsDropped();
        bloom::ui::test::testTheTitleIsRenamedInline();
        bloom::ui::test::testUngroupingAndMenus();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return bloom::ui::test::failures == 0 ? 0 : 1;
}
