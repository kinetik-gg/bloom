// Task FIX1 item I: a reroute is a point on a LINK. One type, whose kind comes from the wire it
// sits on; made by right-clicking the link or dragging across it; drawn as a dot; and gone again
// once both of its ends are.
#include "node_production_harness.hpp"

#include <QMenu>

using namespace bloom;
using namespace bloom::ui;
using namespace bloom::ui::production_test;

namespace {
[[nodiscard]] node_editor::NodeEdgeItem* linkTo(App& a, const document::InputPortRef& destination) {
    for (auto* item : a.editor.graphScene()->items())
        if (auto* link = dynamic_cast<node_editor::NodeEdgeItem*>(item);
            link != nullptr && link->edge.destination == destination)
            return link;
    return nullptr;
}
[[nodiscard]] std::optional<document::NodeId> rerouteNode(const App& a) {
    return a.nodeOfType(document::kRerouteNodeType);
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        App a;
        // A hand-wired image link to bend.
        const auto solid = a.addByType(document::kSolidSourceNodeType, {-600, 0});
        const auto layer = a.addByType(document::kLayerOutputNodeType, {0, 0});
        const document::InputPortRef imageInput =
            document::NodeInputRef{layer, std::string(document::kLayerOutputContentInputPort)};
        a.drag(a.outputSocket(solid)->scenePos(),
               a.namedSocket(layer, QString::fromUtf8(document::kLayerOutputContentInputPort), true)
                   ->scenePos());
        QCoreApplication::processEvents();
        expect(linkTo(a, imageInput) != nullptr, "the image link exists to bend");
        expect(!rerouteNode(a).has_value(), "and no reroute exists yet");

        // --- (a) the link's own context menu. ----------------------------------------------------
        {
            auto* link = linkTo(a, imageInput);
            const auto midpoint = link->path().pointAtPercent(0.5);
            auto* menu =
                a.editor.linkContextMenuForTest(a.editor.graphView()->mapFromScene(midpoint));
            expect(menu != nullptr, "a link offers a menu");
            if (menu == nullptr)
                return 1;
            auto* action = menu->findChild<QAction*>(QStringLiteral("nodeLinkAddRerouteAction"));
            expect(action != nullptr, "whose entries include Add Reroute");
            if (action == nullptr)
                return 1;
            action->trigger();
            QCoreApplication::processEvents();
            delete menu;
        }
        const auto reroute = rerouteNode(a);
        expect(reroute.has_value(), "and triggering it inserts a reroute");
        if (!reroute.has_value())
            return 1;
        expect(a.session.composition()->graph().edges().size() >= 2,
               "the link is now two links, through the reroute");
        expect(linkTo(a, imageInput) != nullptr &&
                   linkTo(a, imageInput)->edge.source.nodeId == *reroute,
               "the Layer now reads from the reroute");

        // --- kind inference: the dot takes the kind of the wire it joined. -----------------------
        expect(a.session.composition()->graph().rerouteKind(*reroute) ==
                   document::SocketValueKind::Image,
               "the reroute carries the kind of the link it was inserted into");
        auto* card = a.card(*reroute);
        expect(card != nullptr && card->isReroute() && card->cardWidth() == 10.0,
               "and is drawn as a 10px dot rather than a card");
        expect(card->toolTip().contains(QStringLiteral("Image")),
               "whose tooltip names the kind it carries");

        // The composition still evaluates through the bend.
        expect(composite(a).ok, "a composition with a reroute in its image chain still compiles");

        // --- auto-delete: take both ends away and the dot goes with them. ------------------------
        {
            const auto before = a.session.composition()->graph().nodes().size();
            // The downstream end first; the upstream link then leaves it stranded.
            a.drag(a.namedSocket(layer, QString::fromUtf8(document::kLayerOutputContentInputPort),
                                 true)
                       ->scenePos(),
                   QPointF(-1400, 900));
            QCoreApplication::processEvents();
            expect(rerouteNode(a).has_value(), "one end gone leaves the reroute in place");
            a.drag(a.namedSocket(*reroute, QString::fromUtf8(document::kValuePortName), true)
                       ->scenePos(),
                   QPointF(-1400, -900));
            QCoreApplication::processEvents();
            expect(!rerouteNode(a).has_value(),
                   "a reroute with nothing on either side removes itself");
            expect(a.session.composition()->graph().nodes().size() == before - 1,
                   "and takes exactly itself with it");
            expect(a.session.undo() && rerouteNode(a).has_value(),
                   "one undo brings the reroute and its link back together");
            expect(a.session.undo(), "and the other end undoes too");
            QCoreApplication::processEvents();
        }

        // --- (b) Shift + right drag across a link. -----------------------------------------------
        {
            App b;
            const auto source = b.addByType(document::kSolidSourceNodeType, {-600, 0});
            const auto boundary = b.addByType(document::kLayerOutputNodeType, {0, 0});
            b.drag(b.outputSocket(source)->scenePos(),
                   b.namedSocket(boundary,
                                 QString::fromUtf8(document::kLayerOutputContentInputPort), true)
                       ->scenePos());
            QCoreApplication::processEvents();
            const document::InputPortRef destination = document::NodeInputRef{
                boundary, std::string(document::kLayerOutputContentInputPort)};
            auto* link = linkTo(b, destination);
            expect(link != nullptr, "the drag fixture has a link");
            if (link == nullptr)
                return 1;
            const auto midpoint = link->path().pointAtPercent(0.5);
            b.mouse(QEvent::MouseButtonPress, midpoint - QPointF(0, 40), Qt::RightButton,
                    Qt::RightButton, Qt::ShiftModifier);
            b.mouse(QEvent::MouseMove, midpoint + QPointF(0, 40), Qt::NoButton, Qt::RightButton,
                    Qt::ShiftModifier);
            b.mouse(QEvent::MouseButtonRelease, midpoint + QPointF(0, 40), Qt::RightButton,
                    Qt::NoButton, Qt::ShiftModifier);
            QCoreApplication::processEvents();
            expect(rerouteNode(b).has_value(),
                   "Shift + right drag across a link inserts a reroute at the crossing");
            expect(b.session.undo() && !rerouteNode(b).has_value(),
                   "and one undo takes it away again");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
