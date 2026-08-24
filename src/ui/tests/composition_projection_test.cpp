#include <bloom/commands/command_stack.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_editors.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/node_editor.hpp>

#include <QApplication>
#include <QDoubleSpinBox>
#include <QGraphicsItem>
#include <QTreeWidget>

#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {

[[nodiscard]] bool require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "Failure: " << message << '\n';
    }
    return condition;
}

const bloom::document::ParameterRecord*
parameterForRole(const bloom::document::Composition& composition,
                 const bloom::document::NodeId nodeId, const std::string_view role) {
    const auto* node = composition.graph().findNode(nodeId);
    if (node == nullptr) {
        return nullptr;
    }
    for (const auto& binding : node->parameters) {
        if (binding.role == role) {
            return composition.parameters().find(binding.parameterId);
        }
    }
    return nullptr;
}

[[nodiscard]] bool runProjectionTest() {
    using namespace bloom;
    auto newProject =
        document::makeNewProject("Projection Test", "Main", core::RationalTime::fromInteger(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    ui::EditorRegistry registry;
    if (!require(ui::registerFoundationEditors(registry, session),
                 "foundation editor registration succeeds") ||
        !require(registry.editors().size() == 5,
                 "foundation registration exposes five replaceable editor types")) {
        return false;
    }
    ui::TimelineEditor timeline(session);
    ui::NodeGraphEditor nodes(session);
    ui::PropertiesEditor properties(session);
    [[maybe_unused]] ui::MediaEditor media(session);
    [[maybe_unused]] ui::ViewerEditor viewer(session);

    const auto layerStackNodeId = session.composition()->graph().layerStack().nodeId();
    if (!require(nodes.graphScene()->findNodeItem(layerStackNodeId) != nullptr,
                 "node projection includes the layer stack") ||
        !require(timeline.findChild<QTreeWidget*>("layerStackView")->topLevelItemCount() == 0,
                 "empty document starts with no layer rows") ||
        !require(session.addTextLayer(QStringLiteral("Title"), QStringLiteral("Hello, Bloom")),
                 "text layer command succeeds")) {
        return false;
    }

    const auto* composition = session.composition();
    if (!require(composition != nullptr, "active composition remains available") ||
        !require(composition->graph().layerStack().entries().size() == 1,
                 "text command inserts one layer stack entry") ||
        !require(std::holds_alternative<document::LayerId>(session.selection().primary),
                 "new layer becomes the stable shared selection")) {
        return false;
    }

    const auto layerId = std::get<document::LayerId>(session.selection().primary);
    const auto boundaryNode = session.boundaryNodeForLayer(layerId);
    auto* layerRow = timeline.findChild<QTreeWidget*>("layerStackView")->topLevelItem(0);
    if (!require(boundaryNode.has_value(), "layer resolves to its graph boundary") ||
        !require(layerRow != nullptr && layerRow->text(0) == QStringLiteral("Title"),
                 "timeline reads the durable boundary name") ||
        !require(nodes.graphScene()->findNodeItem(*boundaryNode) != nullptr,
                 "node scene refreshes with the same boundary") ||
        !require(nodes.graphScene()
                         ->findNodeItem(*boundaryNode)
                         ->data(ui::kNodeStableIdRole)
                         .toULongLong() == boundaryNode->value(),
                 "node graphics item stores the stable document ID")) {
        return false;
    }

    const auto textNode = std::ranges::find_if(composition->graph().nodes(), [](const auto& node) {
        return node.typeId == document::kTextSourceNodeType;
    });
    if (!require(textNode != composition->graph().nodes().end(),
                 "text layer exposes its source node")) {
        return false;
    }
    session.selectNode(textNode->id);
    if (!require(session.selection().primary == ui::SelectionTarget{textNode->id},
                 "internal node remains the primary selection") ||
        !require(session.selection().contextualLayer == layerId,
                 "internal node resolves its unique owning layer context") ||
        !require(layerRow->isSelected(),
                 "timeline reflects contextual layer selection without replacing the node")) {
        return false;
    }
    session.selectLayer(layerId);

    auto* positionX = properties.findChild<QDoubleSpinBox*>("positionXEditor");
    auto* positionY = properties.findChild<QDoubleSpinBox*>("positionYEditor");
    auto* opacity = properties.findChild<QDoubleSpinBox*>("opacityEditor");
    if (!require(positionX != nullptr && positionY != nullptr && opacity != nullptr,
                 "properties exposes transform editors") ||
        !require(positionX->isEnabled() && positionY->isEnabled() && opacity->isEnabled(),
                 "constant layer parameters are editable") ||
        !require(session.setSelectedPosition(24.0, 48.0), "position command succeeds") ||
        !require(session.setSelectedOpacity(0.5), "opacity command succeeds")) {
        return false;
    }

    composition = session.composition();
    const auto* positionParameter =
        parameterForRole(*composition, *boundaryNode, document::kPositionParameterRole);
    const auto* opacityParameter =
        parameterForRole(*composition, *boundaryNode, document::kOpacityParameterRole);
    const auto positionValue = positionParameter == nullptr
                                   ? std::nullopt
                                   : session.constantVec2Value(positionParameter->id);
    const auto opacityValue =
        opacityParameter == nullptr ? std::nullopt : session.constantValue(opacityParameter->id);
    const auto opacityParameterId =
        opacityParameter == nullptr ? document::ParameterId{} : opacityParameter->id;
    if (!require(positionValue == document::Vec2d{24.0, 48.0},
                 "properties command updates the canonical Vec2 parameter") ||
        !require(opacityValue == 0.5,
                 "properties command updates the canonical opacity parameter") ||
        !require(session.undo(), "opacity edit undoes") ||
        !require(session.constantValue(opacityParameterId) == 1.0, "undo restores exact opacity") ||
        !require(session.redo(), "opacity edit redoes") ||
        !require(session.constantValue(opacityParameterId) == 0.5,
                 "redo restores exact opacity edit")) {
        return false;
    }

    bool rejected = false;
    QObject::connect(&session, &ui::CompositionSession::commandRejected, &session,
                     [&rejected] { rejected = true; });
    const auto revision = session.snapshot().revision();
    if (!require(!session.setSelectedOpacity(2.0), "out-of-range opacity is rejected") ||
        !require(rejected, "rejected UI edit emits a diagnostic") ||
        !require(session.snapshot().revision() == revision,
                 "rejected UI edit cannot mutate document truth")) {
        return false;
    }

    return true;
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    return runProjectionTest() ? 0 : 1;
}
