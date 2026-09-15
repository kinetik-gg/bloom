
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

#include <algorithm>
#include <chrono>
#include <cmath>
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
    [[maybe_unused]] ui::AssetsEditor assets(session);
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
        !require(timeline.layerStackForTest()->rowCount() == 0,
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
    // ADAPTED (task S3): the Add > Text action used to refuse with "portable CPU text rendering",
    // and the layer it could not create had to be installed by a test-only fixture so the rest of
    // this projection could be exercised. The command now succeeds, so the action itself creates
    // the layer, no fixture is involved, and the preview state to require is a real frame rather
    // than an UnsupportedNode diagnostic.
    const auto beforeText = session.snapshot().revision();
    QString textRefusal;
    QObject::connect(&session, &ui::CompositionSession::commandRejected, &session,
                     [&textRefusal](const QString& message) { textRefusal = message; });
    addTextAction->trigger();
    if (!require(session.snapshot().revision() != beforeText && textRefusal.isEmpty(),
                 "the text action creates a layer and reports no refusal"))
        return false;

    if (!require(waitUntil([&] {
                     return previewController.state().activity == ui::PreviewActivity::Ready &&
                            previewController.state().frame != nullptr;
                 }),
                 "reachable text produces a real preview frame") ||
        !require(previewController.state().diagnostics.empty(),
                 "a text layer produces no preview diagnostics at all")) {
        return false;
    }

    // Visible in the viewer, asserted on the exact packed RGBA8 buffer the viewer paints rather
    // than on the activity state alone: a text layer whose frame existed but held no ink would
    // satisfy every check above and still show nothing.
    {
        const auto frame = previewController.state().frame;
        const auto buffer = frame == nullptr ? std::nullopt : frame->displayBufferView();
        std::size_t inkPixels = 0;
        if (buffer.has_value()) {
            for (const auto& pixel : buffer->pixels) {
                inkPixels += pixel.alpha == 0 ? 0U : 1U;
            }
        }
        if (!require(buffer.has_value() && inkPixels > 0 && inkPixels < buffer->pixels.size(),
                     "the viewer's own display buffer really carries glyph ink, covering part of "
                     "the frame rather than none or all of it")) {
            return false;
        }
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
    auto* layerStack = timeline.layerStackForTest();
    if (!boundaryNode.has_value()) {
        (void)require(false, "layer resolves to its graph boundary");
        return false;
    }
    if (!require(directTextSource.has_value() && *directTextSource != *boundaryNode,
                 "layer resolves only its direct content source") ||
        !require(layerStack != nullptr && layerStack->rowCount() == 1 &&
                     layerStack->entries()[0].name == QStringLiteral("Text 1") &&
                     layerStack->entries()[0].kind == QStringLiteral("Text"),
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
        !require(timeline.layerStackForTest()->currentRow() == 0,
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
    layerStack = timeline.layerStackForTest();
    if (!solidSourceNodeId.has_value()) {
        (void)require(false, "solid layer has one exact direct source node");
        return false;
    }
    // ADAPTED (task FIX1, item E): a newly added layer lands on TOP of the stack, so the Solid the
    // menu action just added is row 0 rather than the last row.
    if (!require(layerStack != nullptr && layerStack->rowCount() == 2 &&
                     layerStack->entries()[0].name == QStringLiteral("Solid 1") &&
                     layerStack->entries()[0].kind == QStringLiteral("Solid"),
                 "timeline derives Solid kind and default numbered name from project truth")) {
        return false;
    }

    const auto* solidSourceNode = composition->graph().findNode(*solidSourceNodeId);
    const auto* solidColorParameter =
        parameterForRole(*composition, *solidSourceNodeId, document::kSolidColorParameterRole);
    auto* solidColorPanel = properties.findChild<QWidget*>("solidColorProperties");
    // Task P3 (owner review 2026-09-12) replaced the read-only RGBA label with four editable
    // kit::KValueField cells; this fixture now reads the same default color back through them
    // instead of a QLabel's text.
    auto* solidColorRed = properties.findChild<ui::kit::KValueField*>("solidColorRedEditor");
    auto* solidColorGreen = properties.findChild<ui::kit::KValueField*>("solidColorGreenEditor");
    auto* solidColorBlue = properties.findChild<ui::kit::KValueField*>("solidColorBlueEditor");
    auto* solidColorAlpha = properties.findChild<ui::kit::KValueField*>("solidColorAlphaEditor");
    if (!require(waitUntil([&] { return solidColorRed && solidColorRed->unit().isEmpty(); }),
                 "qualified colour presentation becomes ready"))
        return false;
    if (!require(solidSourceNode != nullptr &&
                     solidSourceNode->typeId == document::kSolidSourceNodeType,
                 "direct source is the durable solid-source node") ||
        !require(solidColorParameter != nullptr &&
                     session.constantColorValue(solidColorParameter->id) ==
                         core::Color4d{0.62, 0.08, 0.04, 1.0},
                 "first default solid stores the warm proof-palette color") ||
        !require(
            solidColorPanel != nullptr && !solidColorPanel->isHidden() &&
                solidColorRed != nullptr && solidColorGreen != nullptr &&
                solidColorBlue != nullptr && solidColorAlpha != nullptr &&
                std::abs(solidColorRed->value() - 0.809468) < 3e-5 &&
                std::abs(solidColorGreen->value() - 0.313304) < 3e-5 &&
                std::abs(solidColorBlue->value() - 0.220916) < 3e-5 &&
                solidColorAlpha->value() == 1.0,
            "Properties exposes the converted default display RGBA through editable value cells") ||
        !require(properties.findChild<QLabel*>("solidAlphaAssociation") == nullptr &&
                     properties.findChild<QLabel*>("solidColorEncoding") == nullptr,
                 "Properties keeps technical alpha and encoding metadata out of source rows")) {
        return false;
    }

    auto* solidNodeItem = nodes.graphScene()->findNodeItem(*solidSourceNodeId);
    if (!require(solidNodeItem != nullptr, "node scene projects the solid source")) {
        return false;
    }
    nodes.graphScene()->clearSelection();
    solidNodeItem->setSelected(true);
    if (!require(session.selection().primary == ui::SelectionTarget{*solidSourceNodeId},
                 "clicking a layer-owned node preserves NodeId as primary selection") ||
        !require(session.selection().contextualLayer == solidLayerId,
                 "node selection retains its contextual layer") ||
        // ADAPTED (task FIX1, item E): the Solid landed on top, so its row is 0.
        !require(timeline.layerStackForTest()->currentRow() == 0,
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
    // FORMAL AMENDMENT 1 (2026-09-12): the RGBA cells are unbounded, exactly like the read-only
    // label they replaced -- restores the original "preserves negative and HDR RGB without
    // clipping" pin, now read through the editable cells instead of a QLabel's text.
    if (!require(solidColorRed->value() == -0.25 && solidColorGreen->value() == 1.5 &&
                     solidColorBlue->value() == 0.125 && solidColorAlpha->value() == 0.8,
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
        // ADAPTED (task S3): this required the preview to stay Unsupported after later edits,
        // because the composition still contained a text layer. Text renders now, so the
        // composition
        // -- one text layer and two solids -- must reach a real frame, and the viewer must be
        // showing current pixels rather than retained previous ones.
        !require(waitUntil([&] {
                     return previewController.state().activity == ui::PreviewActivity::Ready &&
                            previewController.state().frame != nullptr;
                 }),
                 "a composition mixing text and solid layers keeps producing real frames") ||
        !require(previewController.state().diagnostics.empty(),
                 "and reports no diagnostics once text is renderable") ||
        !require(!viewer.accessibleDescription().contains(
                     QStringLiteral("Previous composition pixels"), Qt::CaseInsensitive),
                 "Viewer accessibility no longer reports retained previous pixels")) {
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
