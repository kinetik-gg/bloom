#include "node_interaction_test_support.hpp"
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <bloom/document/color_settings.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <string>

namespace bloom::ui::test {
namespace {
project::ProjectIoOperationMemory memory() {
    auto coordinator = project::ProjectIoMemoryCoordinator::create(64U << 20U);
    if (!coordinator)
        throw std::runtime_error("archive memory coordinator");
    auto operation = coordinator->createOperation(64U << 20U, 64U << 20U);
    if (!operation)
        throw std::runtime_error("archive memory operation");
    return std::move(*operation);
}
} // namespace
void testLayoutSelectionAndSockets() {
    Fixture f;
    const auto a = f.add(document::kSolidSourceNodeType, {0, 100});
    const auto b = f.add(document::kLayerOutputNodeType, {280, 100});
    f.session.selectNodes({a, b}, b);
    const auto outlinePixel = [&](document::NodeId id) {
        QImage image(200, 300, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        QStyleOptionGraphicsItem option;
        option.state |= QStyle::State_Selected;
        f.card(id)->paint(&painter, &option, nullptr);
        painter.end();
        return image.pixelColor(64, 1);
    };
    expect(outlinePixel(a) == kit::color(kit::Color::Accent) &&
               outlinePixel(b) == kit::color(kit::Color::Accent),
           "selected and primary outlines paint the exact Accent token above the "
           "header");
    f.session.clearSelection();
    const auto before = f.stack.size();
    f.click({120, 115});
    f.click({370, 115}, Qt::ShiftModifier);
    expect(f.session.selectedNodes() == std::set{a, b},
           "Shift click toggles through the session set");
    f.press({120, 115});
    f.move({155, 145});
    f.move({180, 165});
    expect(f.stack.size() == before && f.card(a)->pos() == QPointF(60, 150) &&
               f.card(b)->pos() == QPointF(340, 150),
           "drag previews the entire selection without per-pixel commands");
    f.release({180, 165});
    expect(f.stack.size() == before + 1 &&
               f.session.composition()->nodeLayout().at(a).position == document::Vec2d{60, 150},
           "one MoveNodes publishes the final positions");
    expect(f.session.undo() && f.card(a)->pos() == QPointF(0, 100) &&
               f.card(b)->pos() == QPointF(280, 100),
           "one undo restores every selected node");
    expect(f.session.redo(), "move redoes");
    const auto resizedBefore = f.stack.size();
    const auto resizePoint = f.card(a)->pos() + QPointF(f.card(a)->cardWidth() - 1, 10);
    const auto resizedWidth = f.card(a)->cardWidth() + 72;
    f.drag(resizePoint, resizePoint + QPointF(72, 0));
    expect(f.session.composition()->nodeLayout().at(a).width == resizedWidth &&
               f.stack.size() == resizedBefore + 1,
           "right edge resize publishes one width edit");
    const auto savedPosition = f.card(a)->pos();
    f.press(savedPosition + QPointF(20, 15));
    f.move(savedPosition + QPointF(90, 45));
    f.key(Qt::Key_Escape);
    expect(f.card(a)->pos() == savedPosition && f.stack.size() == resizedBefore + 1,
           "Escape cancels a move preview");
    f.release(savedPosition + QPointF(90, 45));
    f.drag({5, 150}, {250, 290});
    expect(f.session.selectedNodes() == std::set{a},
           "box selection routes its complete set to session");
    // The box's right edge stays clear of the unplaced Merge card's own right edge: that card now
    // carries the audio stack pill (task FOLLOW-1) one PropertyRow taller than before, which brings
    // its bottom edge low enough to clip this box at its old width. Touching `a` and `b` only needs
    // their LEFT edges, so narrowing the box keeps the gesture this test actually exercises.
    f.drag({300, 149}, {500, 340}, Qt::ShiftModifier);
    expect(f.session.selectedNodes() == std::set{a, b}, "Shift box extends the selection");
    f.click({25, 680});
    expect(f.session.selectedNodes().empty(), "empty click clears all selection");
    auto* socket = f.socket(a, false);
    expect(socket->shape().contains({15, 0}) && !socket->shape().contains({17, 0}) &&
               socket->toolTip() == QStringLiteral("image · Image"),
           "socket has 12px hit slop and name/kind tooltip");
    QGraphicsSceneHoverEvent hover(QEvent::GraphicsSceneHoverEnter);
    f.scene()->sendEvent(socket, &hover);
    expect(socket->data(kNodeHoveredRole).toBool(), "socket hover grows its painted state");
    // Task S1, item 6: the socket palette is its own, not the Data* palette's.
    // ADAPTED (task S7): three more kinds, plus Audio. Both vector widths share SocketVector
    // deliberately -- they read as one family, and a cross-width link is refused by the kind check
    // regardless.
    const std::array mapping{
        kit::Color::SocketImage,   kit::Color::SocketAudio,   kit::Color::SocketColor,
        kit::Color::SocketScalar,  kit::Color::SocketVector,  kit::Color::SocketString,
        kit::Color::SocketInteger, kit::Color::SocketBoolean, kit::Color::SocketVector};
    for (std::size_t i = 0; i < mapping.size(); ++i)
        expect(socketColorToken(static_cast<document::SocketValueKind>(i)) == mapping[i],
               "all socket palette mappings use the socket roles");
    expect(f.edit<commands::SetNodeMuted>(b, true).changed(), "mute fixture");
    auto* field = f.scene()->nodeFieldForTest(b, QStringLiteral("nodePositionXEditor"));
    expect(field && field->isVisible() && field->window()->graphicsProxyWidget()->opacity() == 0.5,
           "unlinked fields remain in-node, and muted body widgets have 50 percent opacity");
    expect(f.edit<commands::ConnectPorts>(document::OutputPortRef{a, "image"},
                                          document::NodeInputRef{b, "image"})
               .changed(),
           "linked Image input fixture");
    expect(field->isVisible(),
           "linked Image input cannot hide or drive unrelated parameter controls");
    expect(f.edit<commands::SetNodeCollapsed>(b, true).changed() &&
               f.card(b)->cardRect().height() == node_editor::kCardHeaderHeight &&
               !field->isVisible() && f.socket(b, true)->pos().y() < node_editor::kCardHeaderHeight,
           "collapsed node is header-only with visible header-edge sockets and hidden fields");
    // ADAPTED (task S7, item 3): the render rule is real now, so it is pinned against the
    // PRODUCTION Solid card rather than a detached fixture schema -- and what hides the control is
    // the parameter's own driver binding, not an edge, because an operand socket and its parameter
    // are one authored value with one durable source.
    document::NodeDefinitionRegistry registry;
    expect(document::registerBuiltInNodeDefinitions(registry), "render-rule registry builtins");
    registry.freeze();
    auto projectionComposition = *f.session.composition();
    const auto* sourceRecord = projectionComposition.graph().findNode(a);
    expect(sourceRecord != nullptr, "render-rule fixture source node");
    const document::NodeRecord rowNode = *sourceRecord;
    node_editor::NodeItem rowCard(rowNode.id, &f.session);
    rowCard.refresh(rowNode, projectionComposition, {{0, 0}, 200, false, false}, registry);
    auto* rowWidget = rowCard.fieldWidget(QStringLiteral("nodeColorChip"));
    expect(rowWidget && rowWidget->isVisible(),
           "unlinked parameter-role socket keeps its kit control");
    const auto colorBinding = std::ranges::find(
        rowNode.parameters, document::kSolidColorParameterRole, &document::ParameterBinding::role);
    expect(colorBinding != rowNode.parameters.end(), "render-rule fixture colour binding");
    const auto valueNodeId = document::NodeId::fromRaw(99999);
    const auto valueParameterId = document::ParameterId::fromRaw(99999);
    expect(projectionComposition.parameters().insert(
               {valueParameterId, std::string(document::kColorValueParameterSchemaKey),
                document::ConstantValueSource{document::kDefaultValueColor}}),
           "render-rule fixture value parameter");
    expect(projectionComposition.graph().addNode(
               {valueNodeId,
                std::string(document::kColorValueNodeType),
                {{std::string(document::kValueParameterRole), valueParameterId}},
                document::kValueNodeSchemaVersion}),
           "render-rule fixture Colour value node");
    expect(projectionComposition.parameters().setSource(
               colorBinding->parameterId,
               document::DriverBindingSource{valueNodeId, std::string(document::kValuePortName)}),
           "render-rule fixture driver binding");
    rowCard.refresh(rowNode, projectionComposition, {{0, 0}, 200, false, false}, registry);
    expect(rowWidget && !rowWidget->isVisible() && rowCard.hasInputSocket(),
           "a driven parameter role hides its kit control while retaining the real socket");
    const auto snapshot = f.session.snapshot();
    const auto color = document::makeBloomNeutralColorSettingsV1(
        core::Sha256Digest::fromBytes(std::array<std::uint8_t, 32>{}));
    const project::CanonicalDocumentV1 input{.snapshot = &snapshot, .colorSettings = &color};
    auto saved = project::buildVerifiedSaveArchive({}, input, {}, memory());
    expect(static_cast<bool>(saved),
           "dragged layout saves through verified production archive writer");
    if (saved) {
        auto reopened = project::openProjectArchive(saved.archive()->bytes(), {}, memory());
        expect(reopened.outcome() == project::OpenArchiveOutcome::Opened,
               "dragged layout reopens through production archive reader");
        if (reopened.outcome() == project::OpenArchiveOutcome::Opened) {
            auto opened = std::move(reopened).takeOpened();
            NodeGraphicsScene projection;
            projection.setProjection(opened.document->snapshot(), f.session.compositionId());
            auto* item = dynamic_cast<node_editor::NodeItem*>(projection.findNodeItem(a));
            expect(item && item->pos() == savedPosition && item->cardWidth() == resizedWidth,
                   "reopened card reads persisted dragged position and resized width");
        }
    }
}
// Task NODES-1: the header menus (deliverable 1), grid snapping (deliverable 3), link style
// (deliverable 2), and the footer (deliverable 4), all exercised on one fixture -- a Solid source
// linked into a Layer Output, which is enough for a "select linked upstream/downstream" walk and
// for a real edge to repaint under each link style.
void testHeaderMenusGridSnappingLinkStyleAndFooter() {
    Fixture f;
    const auto a = f.add(document::kSolidSourceNodeType, {20, 100});
    const auto b = f.add(document::kLayerOutputNodeType, {280, 100});
    expect(f.edit<commands::ConnectPorts>(document::OutputPortRef{a, "image"},
                                          document::NodeInputRef{b, "image"})
               .changed(),
           "fixture link for the grid snap/link style/select-linked walk");

    // --- Deliverable 3: grid snapping, off by default, snaps on when enabled, Alt bypasses. ---
    expect(!f.scene()->gridSnapEnabled(), "grid snapping is disabled by default");
    expect(f.scene()->gridSize() == 16.0, "the default grid size is 16 design px");
    f.scene()->setGridSnapEnabled(true);
    f.click({120, 115});
    f.press({120, 115});
    f.move({131, 122}); // unsnapped target would be (111, 107) -- not on the 16px lattice
    expect(f.card(a)->pos() == QPointF(32, 112),
           "an in-flight drag snaps to the nearest 16px lattice point while enabled");
    f.release({131, 122});
    expect(f.session.composition()->nodeLayout().at(a).position == document::Vec2d{32, 112},
           "the committed MoveNodes position is the snapped one, not the raw drag delta");
    expect(f.session.undo(), "the snapped move undoes in one step");

    f.click({120, 115});
    f.press({120, 115});
    f.move({131, 122}, Qt::LeftButton, Qt::AltModifier);
    expect(f.card(a)->pos() == QPointF(31, 107), "Alt bypasses snapping for this drag only");
    f.release({131, 122}, Qt::AltModifier);
    expect(f.session.composition()->nodeLayout().at(a).position == document::Vec2d{31, 107},
           "and the committed position is the exact, unsnapped one");
    expect(f.session.undo(), "the Alt-bypassed move undoes in one step");
    f.scene()->setGridSnapEnabled(false);

    // --- Deliverable 2: link style. Hit-testing follows the path since NodeEdgeItem::shape()
    // strokes path() itself, so this only has to pin the geometry each style produces. ---
    QGraphicsPathItem* edge = nullptr;
    for (auto* item : f.scene()->items())
        if (item->data(kNodeItemKindRole).toString() == QStringLiteral("edge"))
            if (auto* path = dynamic_cast<QGraphicsPathItem*>(item)) {
                edge = path;
                break;
            }
    expect(edge != nullptr, "the fixture link projects an edge item");
    if (edge != nullptr) {
        expect(edge->path().elementAt(1).type == QPainterPath::CurveToElement,
               "Spline, the default, is the original cubic bezier");
        f.scene()->setLinkStyle(LinkStyle::Straight);
        expect(edge->path().elementCount() == 2 &&
                   edge->path().elementAt(1).type == QPainterPath::LineToElement,
               "Straight repaints the SAME edge item as a direct line, in place");
        f.scene()->setLinkStyle(LinkStyle::Angled);
        expect(edge->path().elementCount() == 4 &&
                   edge->path().elementAt(1).type == QPainterPath::LineToElement &&
                   edge->path().elementAt(1).x == edge->path().elementAt(2).x,
               "Angled repaints it as a horizontal-vertical-horizontal path");
        f.scene()->setLinkStyle(LinkStyle::Spline);
    }

    // --- Deliverable 1: the header menus, built once and reachable even without an EditorArea. ---
    for (const char* which : {"add", "view", "select", "node"})
        expect(f.editor.headerMenuForTest(which) != nullptr,
               (std::string("the header exposes the ") + which + " menu").c_str());
    auto* viewMenu = f.editor.headerMenuForTest("view");
    for (const char* name : {"nodeFitAction", "nodeFrameSelectedAction", "nodeActualSizeAction",
                             "nodeZoomInAction", "nodeZoomOutAction", "nodeGridSnapAction"})
        expect(viewMenu->findChild<QAction*>(QString::fromLatin1(name)) != nullptr,
               (std::string("View offers ") + name).c_str());
    auto* linkStyleMenu = viewMenu->findChild<QMenu*>(QStringLiteral("nodeLinkStyleMenu"));
    expect(linkStyleMenu != nullptr, "View offers a Link Style submenu");
    auto* organizeMenu = viewMenu->findChild<QMenu*>(QStringLiteral("nodeOrganizeMenu"));
    expect(organizeMenu != nullptr, "View offers an Organize submenu");
    if (organizeMenu != nullptr)
        for (const char* name : {"nodeArrangeAllAction", "nodeArrangeSelectionAction"})
            expect(organizeMenu->findChild<QAction*>(QString::fromLatin1(name)) != nullptr,
                   (std::string("Organize offers ") + name).c_str());
    const auto canvasMenu = std::unique_ptr<QMenu>(f.editor.contextMenuForTest(false));
    auto* canvasOrganize = canvasMenu->findChild<QMenu*>(QStringLiteral("nodeOrganizeMenu"));
    expect(canvasOrganize != nullptr, "the canvas context menu offers Organize");
    if (canvasOrganize != nullptr)
        expect(canvasOrganize->findChild<QAction*>(QStringLiteral("nodeArrangeAllAction")) !=
                       nullptr &&
                   canvasOrganize->findChild<QAction*>(
                       QStringLiteral("nodeArrangeSelectionAction")) != nullptr,
               "the canvas Organize menu offers both arrange actions for a selection");
    if (linkStyleMenu != nullptr)
        for (const char* name : {"nodeLinkStyleSplineAction", "nodeLinkStyleStraightAction",
                                 "nodeLinkStyleAngledAction"})
            expect(linkStyleMenu->findChild<QAction*>(QString::fromLatin1(name)) != nullptr,
                   (std::string("Link Style offers ") + name).c_str());
    auto* selectMenu = f.editor.headerMenuForTest("select");
    for (const char* name :
         {"nodeSelectAllAction", "nodeSelectNoneAction", "nodeSelectInvertAction",
          "nodeSelectLinkedUpstreamAction", "nodeSelectLinkedDownstreamAction"})
        expect(selectMenu->findChild<QAction*>(QString::fromLatin1(name)) != nullptr,
               (std::string("Select offers ") + name).c_str());
    auto* nodeMenu = f.editor.headerMenuForTest("node");
    for (const char* name :
         {"nodeGroupAction", "nodeUngroupAction", "nodeMuteAction", "nodeCollapseAction",
          "nodeRenameAction", "nodeDissolveAction", "nodeDeleteAction"})
        expect(nodeMenu->findChild<QAction*>(QString::fromLatin1(name)) != nullptr,
               (std::string("Node offers ") + name).c_str());
    auto* addMenu = f.editor.headerMenuForTest("add");
    expect(addMenu->findChild<QAction*>(QStringLiteral("nodeAddSolidLayerAction")) != nullptr,
           "the header's Add menu is the same categorized submenu the canvas offers");

    // Select None/Invert/Linked Upstream actually do what they say.
    f.session.clearSelection();
    f.click({370, 115}); // card b, the Layer Output
    selectMenu->findChild<QAction*>(QStringLiteral("nodeSelectLinkedUpstreamAction"))->trigger();
    expect(f.session.selectedNodes().contains(a) && f.session.selectedNodes().contains(b),
           "Linked Upstream extends the selection to the node feeding it");
    selectMenu->findChild<QAction*>(QStringLiteral("nodeSelectNoneAction"))->trigger();
    expect(f.session.selectedNodes().empty(), "Select None clears the selection");
    selectMenu->findChild<QAction*>(QStringLiteral("nodeSelectInvertAction"))->trigger();
    expect(f.session.selectedNodes().size() == f.session.composition()->graph().nodes().size(),
           "Invert from nothing selects everything");

    // --- Deliverable 4: the footer. ---
    auto* footer = f.editor.footerWidgetForTest();
    expect(footer != nullptr, "the footer widget exists");
    if (footer != nullptr) {
        expect(
            footer->findChild<QWidget*>(QStringLiteral("nodeZoomDropdown")) != nullptr &&
                f.editor.findChild<QWidget*>(QStringLiteral("nodeSnapSwitch"))->isHidden() &&
                f.editor.findChild<QWidget*>(QStringLiteral("nodeLinkStyleDropdown"))->isHidden(),
            "the footer shows zoom and hides the legacy snap and link style controls");
        auto* readout = footer->findChild<QLabel*>(QStringLiteral("nodeSelectionReadout"));
        expect(readout != nullptr, "the footer carries the selection readout");
        if (readout != nullptr) {
            f.session.selectNodes({a, b}, a);
            expect(readout->text() == QStringLiteral("2 nodes"),
                   "the readout reflects the live selection count");
        }
    }
}
} // namespace bloom::ui::test

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        bloom::ui::test::testLayoutSelectionAndSockets();
        bloom::ui::test::testConnectionsCutAndInsertion();
        bloom::ui::test::testMergeAudioPill();
        bloom::ui::test::testSearchKeyboardAndMenus();
        bloom::ui::test::testHeaderMenusGridSnappingLinkStyleAndFooter();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return bloom::ui::test::failures == 0 ? 0 : 1;
}
