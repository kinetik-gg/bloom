#include "node_interaction_test_support.hpp"
#include <QImage>
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <bloom/document/color_settings.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>

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
    const auto a = f.add(document::kSolidSourceNodeType, {100, 100});
    const auto b = f.add(document::kLayerOutputNodeType, {350, 100});
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
               outlinePixel(b) == kit::color(kit::Color::Foreground),
           "selected and primary outlines paint the exact Accent and Foreground tokens above the "
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
    expect(f.stack.size() == before && f.card(a)->pos() == QPointF(160, 150) &&
               f.card(b)->pos() == QPointF(410, 150),
           "drag previews the entire selection without per-pixel commands");
    f.release({180, 165});
    expect(f.stack.size() == before + 1 &&
               f.session.composition()->nodeLayout().at(a).position == document::Vec2d{160, 150},
           "one MoveNodes publishes the final positions");
    expect(f.session.undo() && f.card(a)->pos() == QPointF(100, 100) &&
               f.card(b)->pos() == QPointF(350, 100),
           "one undo restores every selected node");
    expect(f.session.redo(), "move redoes");
    const auto resizedBefore = f.stack.size();
    f.drag({287, 160}, {359, 160});
    expect(f.session.composition()->nodeLayout().at(a).width == 200 &&
               f.stack.size() == resizedBefore + 1,
           "right edge resize publishes one width edit");
    const auto savedPosition = f.card(a)->pos();
    f.press(savedPosition + QPointF(20, 15));
    f.move(savedPosition + QPointF(90, 45));
    f.key(Qt::Key_Escape);
    expect(f.card(a)->pos() == savedPosition && f.stack.size() == resizedBefore + 1,
           "Escape cancels a move preview");
    f.release(savedPosition + QPointF(90, 45));
    f.drag({145, 130}, {370, 290});
    expect(f.session.selectedNodes() == std::set{a},
           "box selection routes its complete set to session");
    f.drag({400, 130}, {560, 340}, Qt::ShiftModifier);
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
    const std::array mapping{kit::Color::DataImage, kit::Color::DataSequence, kit::Color::Muted,
                             kit::Color::DataComposition, kit::Color::DataAudio};
    for (std::size_t i = 0; i < mapping.size(); ++i)
        expect(socketColorToken(static_cast<document::SocketValueKind>(i)) == mapping[i],
               "all socket palette mappings match N1");
    expect(f.edit<commands::SetNodeMuted>(b, true).changed(), "mute fixture");
    auto* field = f.scene()->nodeFieldForTest(b, QStringLiteral("nodePositionXEditor"));
    expect(field && field->graphicsProxyWidget()->isVisible() &&
               field->graphicsProxyWidget()->opacity() == 0.5,
           "unlinked fields remain in-node, and muted body widgets have 50 percent opacity");
    expect(f.edit<commands::ConnectPorts>(document::OutputPortRef{a, "image"},
                                          document::NodeInputRef{b, "image"})
               .changed(),
           "linked Image input fixture");
    expect(field->graphicsProxyWidget()->isVisible(),
           "linked Image input cannot hide or drive unrelated parameter controls");
    expect(f.edit<commands::SetNodeCollapsed>(b, true).changed() &&
               f.card(b)->cardRect().height() == node_editor::kCardHeaderHeight &&
               !field->graphicsProxyWidget()->isVisible() &&
               f.socket(b, true)->pos().y() < node_editor::kCardHeaderHeight,
           "collapsed node is header-only with visible header-edge sockets and hidden fields");
    // Detached, fixture-only schema: pin the linked-widget render rule without introducing any
    // production node kind, parameter transport or evaluator behavior.
    document::NodeDefinitionRegistry registry;
    expect(document::registerBuiltInNodeDefinitions(registry), "render-rule registry builtins");
    document::NodeDefinition definition;
    definition.key = {"test.image-parameter-row", 1};
    definition.inputs = {{"color", document::SocketValueKind::Image, false}};
    expect(registry.registerDefinition(definition) == document::NodeRegistrationStatus::Registered,
           "render-rule fixture schema");
    registry.freeze();
    auto projectionComposition = *f.session.composition();
    const auto* sourceRecord = projectionComposition.graph().findNode(a);
    const document::NodeRecord rowNode{document::NodeId::fromRaw(99999), definition.key.typeId,
                                       sourceRecord->parameters, 1};
    expect(projectionComposition.graph().addNode(rowNode), "render-rule fixture node");
    node_editor::NodeItem rowCard(rowNode.id, &f.session);
    rowCard.refresh(rowNode, projectionComposition, {{0, 0}, 200, false, false}, registry);
    auto* rowWidget = rowCard.fieldWidget(QStringLiteral("nodeColorChip"));
    expect(rowWidget && rowWidget->graphicsProxyWidget()->isVisible(),
           "unlinked parameter-role socket keeps its kit control");
    expect(projectionComposition.graph().addEdge({document::EdgeId::fromRaw(99999),
                                                  {a, "image"},
                                                  document::NodeInputRef{rowNode.id, "color"}},
                                                 registry),
           "render-rule fixture Image edge");
    rowCard.refresh(rowNode, projectionComposition, {{0, 0}, 200, false, false}, registry);
    expect(rowWidget && !rowWidget->graphicsProxyWidget()->isVisible() && rowCard.hasInputSocket(),
           "linked parameter-role input hides its kit control while retaining the real socket");
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
            expect(item && item->pos() == savedPosition && item->cardWidth() == 200,
                   "reopened card reads persisted dragged position and resized width");
        }
    }
}
} // namespace bloom::ui::test

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        bloom::ui::test::testLayoutSelectionAndSockets();
        bloom::ui::test::testConnectionsCutAndInsertion();
        bloom::ui::test::testSearchKeyboardAndMenus();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return bloom::ui::test::failures == 0 ? 0 : 1;
}
