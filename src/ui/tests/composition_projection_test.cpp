#include <bloom/commands/command_stack.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_editors.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGraphicsItem>
#include <QLabel>
#include <QMenu>
#include <QToolButton>
#include <QTreeWidget>

#include <algorithm>
#include <chrono>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace {

[[nodiscard]] bool require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "Failure: " << message << '\n';
    }
    return condition;
}

template <typename Predicate> [[nodiscard]] bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 2'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::yield();
    }
    return predicate();
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

[[nodiscard]] bool runSolidPaletteTest() {
    using namespace bloom;
    auto newProject =
        document::makeNewProject("Palette Test", "Main", core::RationalTime::fromInteger(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge taskUiBridge(scheduler, nullptr, std::chrono::milliseconds{1});
    ui::CompositionPreviewController previewController(
        session, scheduler, taskUiBridge,
        [](const document::Snapshot&, const runtime::PreviewRequestIdentity&, std::size_t,
           const std::optional<runtime::SnapshotParameterOverride>&, runtime::TaskContext&) {
            return runtime::TaskResult<ui::PreviewPreparationResultHandle>::cancelled();
        });
    ui::TimelineEditor timeline(session, previewController);
    auto* addSolidAction = timeline.findChild<QAction*>("addSolidLayerAction");
    if (!require(addSolidAction != nullptr &&
                     addSolidAction->toolTip().contains(QStringLiteral("reference-linear-sRGB")),
                 "Solid action exposes the proof-palette encoding")) {
        return false;
    }

    addSolidAction->trigger();
    const auto* firstLayer = std::get_if<document::LayerId>(&session.selection().primary);
    const auto firstSource =
        firstLayer == nullptr ? std::nullopt : session.directSourceNodeForLayer(*firstLayer);
    const auto* firstParameter = firstSource.has_value()
                                     ? parameterForRole(*session.composition(), *firstSource,
                                                        document::kSolidColorParameterRole)
                                     : nullptr;
    const auto firstColor =
        firstParameter == nullptr ? std::nullopt : session.constantColorValue(firstParameter->id);

    addSolidAction->trigger();
    const auto* secondLayer = std::get_if<document::LayerId>(&session.selection().primary);
    const auto secondSource =
        secondLayer == nullptr ? std::nullopt : session.directSourceNodeForLayer(*secondLayer);
    const auto* secondParameter = secondSource.has_value()
                                      ? parameterForRole(*session.composition(), *secondSource,
                                                         document::kSolidColorParameterRole)
                                      : nullptr;
    const auto secondColor =
        secondParameter == nullptr ? std::nullopt : session.constantColorValue(secondParameter->id);

    const bool paletteOk =
        require(firstColor == core::Color4d{0.62, 0.08, 0.04, 1.0},
                "first built-in solid uses the warm proof color") &&
        require(secondColor == core::Color4d{0.04, 0.20, 0.72, 1.0},
                "second built-in solid uses a clearly distinct cool proof color") &&
        require(firstColor != secondColor,
                "consecutive built-in solids make layer ordering visually distinguishable");

    previewController.beginShutdown();
    taskUiBridge.beginShutdown();
    return paletteOk && require(waitUntil([&scheduler] { return scheduler.isQuiescent(); }),
                                "solid palette fixture reaches scheduler quiescence");
}

[[nodiscard]] bool runProjectionTest() {
    using namespace bloom;
    const auto format = document::CompositionFormat::create(64, 36);
    if (!format.has_value()) {
        (void)require(false, "small projection format is valid");
        return false;
    }
    auto newProject = document::makeNewProject("Projection Test", "Main",
                                               core::RationalTime::fromInteger(10), *format);
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::NodeDefinitionRegistry nodeDefinitions;
    if (!require(runtime::registerBuiltInNodeDefinitions(nodeDefinitions),
                 "built-in node definitions register")) {
        return false;
    }
    nodeDefinitions.freeze();
    runtime::SnapshotCompiler snapshotCompiler(nodeDefinitions);
    runtime::CpuCompositionEvaluator cpuEvaluator;
    runtime::CpuReferenceDisplayPreparer referenceDisplayPreparer;
    // Never published to Ready in this test (issue #97, task C3): composition projection is
    // unrelated to qualified-display readiness/failure, so this pipeline stays on the unchanged
    // reference path throughout.
    runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge taskUiBridge(scheduler, nullptr, std::chrono::milliseconds{1});
    ui::CompositionPreviewController previewController(
        session, scheduler, taskUiBridge,
        ui::makeCompositionPreviewPipeline(snapshotCompiler, cpuEvaluator, referenceDisplayPreparer,
                                           qualifiedProcessorProvider));
    ui::EditorRegistry registry;
    if (!require(ui::registerFoundationEditors(registry, session, previewController),
                 "foundation editor registration succeeds") ||
        !require(registry.editors().size() == 5,
                 "foundation registration exposes five replaceable editor types")) {
        return false;
    }
    ui::TimelineEditor timeline(session, previewController);
    ui::NodeGraphEditor nodes(session);
    ui::PropertiesEditor properties(session);
    [[maybe_unused]] ui::MediaEditor media(session);
    ui::ViewerEditor viewer(session, previewController);

    if (!require(waitUntil([&] {
                     return previewController.state().activity == ui::PreviewActivity::Ready;
                 }),
                 "built-in pipeline renders the initial composition") ||
        !require(previewController.state().desiredIdentity.has_value() &&
                     previewController.state().desiredIdentity->sourceRevision ==
                         session.snapshot().revision(),
                 "prepared frame identifies the exact active revision")) {
        return false;
    }

    auto* addButton = timeline.findChild<QToolButton*>("addLayerButton");
    auto* addMenu = timeline.findChild<QMenu*>("addLayerMenu");
    auto* addSolidAction = timeline.findChild<QAction*>("addSolidLayerAction");
    auto* addTextAction = timeline.findChild<QAction*>("addTextLayerAction");

    const auto layerStackNodeId = session.composition()->graph().layerStack().nodeId();
    if (!require(nodes.graphScene()->findNodeItem(layerStackNodeId) != nullptr,
                 "node projection includes the layer stack") ||
        !require(timeline.findChild<QTreeWidget*>("layerStackView")->topLevelItemCount() == 0,
                 "empty document starts with no layer rows") ||
        !require(addButton != nullptr && addButton->menu() == addMenu && addMenu != nullptr &&
                     !addButton->accessibleName().isEmpty() && !addMenu->accessibleName().isEmpty(),
                 "timeline exposes one accessible Add menu") ||
        !require(addSolidAction != nullptr && addSolidAction->text() == QStringLiteral("Solid") &&
                     addTextAction != nullptr && addTextAction->text() == QStringLiteral("Text"),
                 "Add menu exposes Solid and Text actions") ||
        !require(addSolidAction->toolTip().contains(QStringLiteral("reference-linear-sRGB")),
                 "Solid action names the built-in proof palette encoding")) {
        return false;
    }
    addTextAction->trigger();

    if (!require(waitUntil([&] {
                     return previewController.state().activity == ui::PreviewActivity::Unsupported;
                 }),
                 "reachable text produces an explicit unsupported preview state") ||
        !require(!previewController.state().diagnostics.empty() &&
                     previewController.state().diagnostics.front().code ==
                         runtime::compileDiagnosticCodeId(
                             runtime::CompileDiagnosticCode::UnsupportedNode),
                 "unsupported preview retains the structured compiler diagnostic")) {
        return false;
    }

    const auto* composition = session.composition();
    if (!require(composition != nullptr, "active composition remains available") ||
        !require(composition->graph().layerStack().entries().size() == 1,
                 "text command inserts one layer stack entry")) {
        return false;
    }

    const auto* layerIdPtr = std::get_if<document::LayerId>(&session.selection().primary);
    if (!require(layerIdPtr != nullptr, "new layer becomes the stable shared selection")) {
        return false;
    }
    const auto layerId = *layerIdPtr;
    const auto boundaryNode = session.boundaryNodeForLayer(layerId);
    const auto directTextSource = session.directSourceNodeForLayer(layerId);
    auto* layerRow = timeline.findChild<QTreeWidget*>("layerStackView")->topLevelItem(0);
    if (!boundaryNode.has_value()) {
        (void)require(false, "layer resolves to its graph boundary");
        return false;
    }
    if (!require(directTextSource.has_value() && *directTextSource != *boundaryNode,
                 "layer resolves only its direct content source") ||
        !require(layerRow != nullptr && layerRow->text(0) == QStringLiteral("Text 1") &&
                     layerRow->text(1) == QStringLiteral("Text"),
                 "timeline reads the durable name and derives Text from the direct source") ||
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
                 "text layer exposes its source node") ||
        !require(directTextSource == textNode->id,
                 "direct-source query identifies the exact text source node")) {
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

    auto* positionX = properties.findChild<ui::kit::KValueField*>("positionXEditor");
    auto* positionY = properties.findChild<ui::kit::KValueField*>("positionYEditor");
    auto* opacity = properties.findChild<ui::kit::KValueField*>("opacityEditor");
    if (!require(positionX != nullptr && positionY != nullptr && opacity != nullptr,
                 "properties exposes transform editors") ||
        !require(positionX->isEnabled() && positionY->isEnabled() && opacity->isEnabled(),
                 "constant layer parameters are editable") ||
        !require(session.setSelectedPosition(24.0, 48.0), "position command succeeds") ||
        !require(session.setSelectedOpacity(0.5), "opacity command succeeds")) {
        return false;
    }

    composition = session.composition();
    if (!boundaryNode.has_value()) {
        (void)require(false, "boundary node ID remains available for parameter lookup");
        return false;
    }
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

    addSolidAction->trigger();
    composition = session.composition();
    if (!require(composition != nullptr && composition->graph().layerStack().entries().size() == 2,
                 "Solid menu action adds one structured layer")) {
        return false;
    }

    const auto* solidLayerIdPtr = std::get_if<document::LayerId>(&session.selection().primary);
    if (!require(solidLayerIdPtr != nullptr, "new solid becomes the primary layer selection")) {
        return false;
    }
    const auto solidLayerId = *solidLayerIdPtr;
    const auto solidSourceNodeId = session.directSourceNodeForLayer(solidLayerId);
    auto* solidRow = timeline.findChild<QTreeWidget*>("layerStackView")->topLevelItem(1);
    if (!solidSourceNodeId.has_value()) {
        (void)require(false, "solid layer has one exact direct source node");
        return false;
    }
    if (!require(solidRow != nullptr && solidRow->text(0) == QStringLiteral("Solid 1") &&
                     solidRow->text(1) == QStringLiteral("Solid"),
                 "timeline derives Solid kind and default numbered name from project truth")) {
        return false;
    }

    const auto* solidSourceNode = composition->graph().findNode(*solidSourceNodeId);
    const auto* solidColorParameter =
        parameterForRole(*composition, *solidSourceNodeId, document::kSolidColorParameterRole);
    auto* solidColorPanel = properties.findChild<QWidget*>("solidColorProperties");
    auto* solidColorValue = properties.findChild<QLabel*>("solidColorValue");
    auto* solidAlphaAssociation = properties.findChild<QLabel*>("solidAlphaAssociation");
    auto* solidColorEncoding = properties.findChild<QLabel*>("solidColorEncoding");
    if (!require(solidSourceNode != nullptr &&
                     solidSourceNode->typeId == document::kSolidSourceNodeType,
                 "direct source is the durable solid-source node") ||
        !require(solidColorParameter != nullptr &&
                     session.constantColorValue(solidColorParameter->id) ==
                         core::Color4d{0.62, 0.08, 0.04, 1.0},
                 "first default solid stores the warm proof-palette color") ||
        !require(solidColorPanel != nullptr && !solidColorPanel->isHidden() &&
                     solidColorValue != nullptr &&
                     solidColorValue->text() == QStringLiteral("R 0.62  G 0.08  B 0.04  A 1"),
                 "Properties exposes the exact default RGBA as read-only text") ||
        !require(solidAlphaAssociation != nullptr &&
                     solidAlphaAssociation->text() == QStringLiteral("Straight (unassociated)") &&
                     solidColorEncoding != nullptr &&
                     solidColorEncoding->text() == QStringLiteral("bloom.reference.linear-srgb"),
                 "Properties names alpha association and reference encoding explicitly")) {
        return false;
    }

    auto* solidNodeItem = nodes.graphScene()->findNodeItem(*solidSourceNodeId);
    if (!require(solidNodeItem != nullptr, "node scene projects the solid source")) {
        return false;
    }
    nodes.graphScene()->clearSelection();
    solidNodeItem->setSelected(true);
    solidRow = timeline.findChild<QTreeWidget*>("layerStackView")->topLevelItem(1);
    if (!require(session.selection().primary == ui::SelectionTarget{*solidSourceNodeId},
                 "clicking a layer-owned node preserves NodeId as primary selection") ||
        !require(session.selection().contextualLayer == solidLayerId,
                 "node selection retains its contextual layer") ||
        !require(solidRow != nullptr && solidRow->isSelected(),
                 "timeline highlights node context without replacing primary selection")) {
        return false;
    }

    constexpr core::Color4d hdrColor{-0.25, 1.5, 0.125, 0.8};
    if (!require(session.addSolidLayer(QStringLiteral("HDR Solid"), hdrColor),
                 "session can add an explicit HDR solid")) {
        return false;
    }
    const auto* hdrLayerIdPtr = std::get_if<document::LayerId>(&session.selection().primary);
    if (!require(hdrLayerIdPtr != nullptr,
                 "explicit HDR solid becomes the primary layer selection")) {
        return false;
    }
    const auto hdrLayerId = *hdrLayerIdPtr;
    const auto hdrSourceNodeId = session.directSourceNodeForLayer(hdrLayerId);
    if (!hdrSourceNodeId.has_value()) {
        (void)require(false, "HDR solid layer has one exact direct source node");
        return false;
    }
    if (!require(solidColorValue->text() == QStringLiteral("R -0.25  G 1.5  B 0.125  A 0.8"),
                 "Properties preserves negative and HDR RGB without clipping") ||
        !require(session.undo(), "adding the HDR solid is undoable") ||
        !require(session.composition()->graph().layerStack().entries().size() == 2 &&
                     std::holds_alternative<std::monostate>(session.selection().primary),
                 "undo removes the solid and clears its unavailable selection") ||
        !require(session.redo(), "adding the HDR solid is redoable")) {
        return false;
    }

    composition = session.composition();
    const auto* restoredColor =
        hdrSourceNodeId.has_value()
            ? parameterForRole(*composition, *hdrSourceNodeId, document::kSolidColorParameterRole)
            : nullptr;
    if (!require(composition->graph().layerStack().entries().size() == 3 &&
                     session.directSourceNodeForLayer(hdrLayerId) == hdrSourceNodeId &&
                     nodes.graphScene()->findNodeItem(*hdrSourceNodeId) != nullptr,
                 "redo restores exact layer/source IDs and all synchronized projections") ||
        !require(restoredColor != nullptr &&
                     session.constantColorValue(restoredColor->id) == hdrColor,
                 "redo restores exact unclipped solid color truth") ||
        !require(waitUntil([&] {
                     return previewController.state().activity == ui::PreviewActivity::Unsupported;
                 }),
                 "unsupported Text remains explicit after later edits") ||
        !require(viewer.accessibleDescription().contains(
                     QStringLiteral("Previous composition pixels"), Qt::CaseInsensitive),
                 "Viewer accessibility reports retained pixels as previous")) {
        return false;
    }

    previewController.beginShutdown();
    taskUiBridge.beginShutdown();
    return require(waitUntil([&scheduler] { return scheduler.isQuiescent(); }),
                   "preview fixture reaches scheduler quiescence");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    try {
        return runSolidPaletteTest() && runProjectionTest() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "composition projection test failed with an exception: " << error.what()
                  << '\n';
        return 1;
    }
}
