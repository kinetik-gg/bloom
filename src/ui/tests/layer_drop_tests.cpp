// #380: dropping Assets items or files on the Timeline or the Viewer creates layers, as in After
// Effects. Over the layer stack the drop inserts at the row; over the lanes it starts at the frame
// under the pointer; elsewhere it goes on top at the playhead. Several items stack in selection
// order (Alt sequences them end to end), compositions become nested layers, and each drop is one
// undo step. Files are imported first and become layers in a second step once the import commits.

#include "asset_drop.hpp"
#include "assets_editor_internal.hpp"
#include "layer_drop.hpp"

#include <QApplication>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QImage>
#include <QItemSelectionModel>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/timeline_ruler.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace bloom;
using namespace std::chrono_literals;

namespace {
int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 20000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}
core::RationalTime frame(const std::int64_t index) {
    return *core::RationalTime::create(index, 24);
}

struct Pipeline final {
    runtime::NodeDefinitionRegistry definitions;
    runtime::SnapshotCompiler compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    ui::PreviewPreparationFunction pipeline;
    Pipeline() : compiler(definitions) {
        if (!runtime::registerBuiltInNodeDefinitions(definitions))
            std::abort();
        definitions.freeze();
        pipeline = ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                      qualifiedProcessorProvider);
    }
};

void writeFrames(const QString& folder, const QString& stem, const int count, const QColor colour) {
    QImage image(32, 18, QImage::Format_RGB32);
    image.fill(colour);
    for (int index = 1; index <= count; ++index)
        image.save(
            QStringLiteral("%1/%2_%3.png").arg(folder, stem).arg(index, 4, 10, QLatin1Char('0')));
}

struct Fixture final {
    QTemporaryDir directory;
    runtime::TaskScheduler scheduler{runtime::TaskSchedulerConfig{.cpuWorkerCount = 1,
                                                                  .blockingIoWorkerCount = 1,
                                                                  .cpuQueueCapacity = 32,
                                                                  .blockingIoQueueCapacity = 8,
                                                                  .terminalHistoryCapacity = 64,
                                                                  .diagnosticsPerTask = 8,
                                                                  .groupRegistryCapacity = 8}};
    ui::ProjectHost host{scheduler};
    ui::CompositionSession session{*host.liveDocumentAndStack().first,
                                   *host.liveDocumentAndStack().second, host.lowestCompositionId()};
    ui::TaskUiBridge bridge{scheduler, nullptr, 1ms};
    std::unique_ptr<ui::AssetController> controller;
    Pipeline pipeline;
    std::unique_ptr<ui::CompositionPreviewController> preview;
    document::CompositionId main, nested;
    document::AssetId still, shotA, shotB;

    Fixture() {
        const auto add = [&](const char* name, const std::int64_t seconds) {
            commands::Transaction transaction("Add composition", session.snapshot().revision());
            transaction.emplace<commands::AddComposition>(
                name, *document::CompositionFormat::create(320, 180),
                core::RationalTime::fromInteger(seconds));
            const auto result = session.executeTransaction(std::move(transaction));
            return result.outputId<document::CompositionId>(commands::kAddCompositionOutput)
                .value_or(document::CompositionId{});
        };
        nested = add("Nested", 2);
        main = add("Main", 4);
        if (!session.setComposition(main))
            std::abort();
        controller = std::make_unique<ui::AssetController>(session, host, scheduler, bridge);
        preview = std::make_unique<ui::CompositionPreviewController>(session, scheduler, bridge,
                                                                     pipeline.pipeline);
        // A still and two image sequences (12 and 6 frames), imported the ordinary way.
        QImage plate(32, 18, QImage::Format_RGB32);
        plate.fill(Qt::white);
        plate.save(directory.filePath("plate.png"));
        writeFrames(directory.path(), QStringLiteral("shotA"), 12, Qt::red);
        writeFrames(directory.path(), QStringLiteral("shotB"), 6, Qt::blue);
        commands::Transaction import("Import", session.snapshot().revision());
        import.emplace<commands::ImportAssets>(
            std::vector<std::filesystem::path>{directory.filePath("plate.png").toStdString(),
                                               directory.filePath("shotA_0001.png").toStdString(),
                                               directory.filePath("shotB_0001.png").toStdString()},
            directory.path().toStdString());
        if (!session.executeTransaction(std::move(import)).succeeded())
            std::abort();
        for (const auto& asset : session.snapshot().project().assets()) {
            if (asset.kind == document::AssetKind::Image)
                still = asset.id;
            else if (asset.locator.path.find("shotA") != std::string::npos)
                shotA = asset.id;
            else if (asset.locator.path.find("shotB") != std::string::npos)
                shotB = asset.id;
        }
    }
    ~Fixture() {
        controller->cancel();
        preview->beginShutdown();
        bridge.beginShutdown();
        for (int i = 0; i < 400 && !scheduler.isQuiescent(); ++i)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    [[nodiscard]] const document::Composition& composition() const {
        return *session.snapshot().project().findComposition(main);
    }
    [[nodiscard]] std::vector<document::LayerId> stack() const {
        std::vector<document::LayerId> layers;
        for (const auto& entry : composition().graph().layerStack().entries())
            layers.push_back(entry.layerId);
        return layers;
    }
    [[nodiscard]] const document::LayerOutputBoundary& layer(const document::LayerId id) const {
        return *composition().graph().findLayer(id);
    }
    bool drop(std::vector<ui::LayerDropItem> items, ui::LayerDropPlacement placement) {
        commands::Transaction transaction(ui::layerDropLabel(items), session.snapshot().revision());
        transaction.emplace<ui::AddDroppedLayers>(main, std::move(items), placement);
        return session.executeTransaction(std::move(transaction)).succeeded();
    }
};

// The drop selected exactly `layers`, the first one primary (a session change, not an edit).
bool selected(const Fixture& fixture, const std::vector<document::LayerId>& layers) {
    const auto* primary = std::get_if<document::LayerId>(&fixture.session.selection().primary);
    std::set<document::NodeId> nodes;
    for (const auto layer : layers)
        nodes.insert(fixture.layer(layer).nodeId);
    return !layers.empty() && primary != nullptr && *primary == layers.front() &&
           fixture.session.selectedNodes() == nodes;
}

// The nested composition a layer reads, through its image input.
document::CompositionId nestedSourceOf(const document::Composition& composition,
                                       const document::LayerOutputBoundary& layer) {
    for (const auto& edge : composition.graph().edges())
        if (const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
            input && input->nodeId == layer.nodeId)
            if (const auto* node = composition.graph().findNode(edge.source.nodeId))
                if (const auto id = ui::compositionSourceId(composition, *node); id.isValid())
                    return id;
    return {};
}

void operationPlacesAndTimesLayers(Fixture& fixture) {
    auto& session = fixture.session;
    expect(session.addSolidLayer("Lower", core::Color4d{0.2, 0.2, 0.2, 1.0}) &&
               session.addSolidLayer("Upper", core::Color4d{0.4, 0.4, 0.4, 1.0}),
           "two solids");
    const auto solids = fixture.stack();
    expect(solids.size() == 2, "the stack holds the two solids, Upper first");
    const auto lowerSlot = fixture.composition().graph().layerStack().entries()[1].slotId;

    // Row insertion: directly above Lower, starting at the given frame.
    expect(fixture.drop({fixture.shotA}, {.stack = ui::LayerDropPlacement::Stack::Before,
                                          .before = lowerSlot,
                                          .start = frame(10)}),
           "a sequence drops above Lower");
    auto layers = fixture.stack();
    expect(layers.size() == 3 && layers[0] == solids[0] && layers[2] == solids[1],
           "the new layer is inserted between Upper and Lower");
    const auto& shot = fixture.layer(layers[1]);
    expect(shot.inPoint == frame(10) &&
               shot.endPoint(fixture.composition().duration()) == frame(22),
           "the 12-frame sequence keeps its own length from frame 10");
    expect(session.undo() && fixture.stack() == solids, "one undo removes the dropped layer");

    // A start between frames lands on the nearest frame start.
    expect(fixture.drop({fixture.shotB}, {.start = *core::RationalTime::create(101, 240)}),
           "a drop between frames");
    expect(fixture.layer(fixture.stack().front()).inPoint == frame(10),
           "the start snaps to the composition frame grid");
    expect(session.undo(), "undo");

    // Several items stack in order, the first on top; a composition becomes a nested layer.
    const auto before = session.snapshot().revision();
    expect(fixture.drop({fixture.shotA, fixture.shotB, fixture.nested}, {.start = frame(0)}),
           "three items drop on top");
    layers = fixture.stack();
    expect(layers.size() == 5 && layers[3] == solids[0] && layers[4] == solids[1],
           "the three new layers sit above the solids");
    const auto& composition = fixture.composition();
    expect(fixture.layer(layers[0]).endPoint(composition.duration()) == frame(12) &&
               fixture.layer(layers[1]).endPoint(composition.duration()) == frame(6),
           "first item on top, then the second, each its own length");
    expect(nestedSourceOf(composition, fixture.layer(layers[2])) == fixture.nested &&
               fixture.layer(layers[2]).endPoint(composition.duration()) == frame(48),
           "the composition is a nested layer as long as the nested composition");
    expect(session.undo() && fixture.stack() == solids && session.snapshot().revision() != before,
           "the whole drop is one undo step");

    // Sequenced (Alt): end to end from the start.
    expect(
        fixture.drop(
            {fixture.shotA, fixture.shotB},
            {.stack = ui::LayerDropPlacement::Stack::Bottom, .start = frame(5), .sequence = true}),
        "a sequenced drop");
    layers = fixture.stack();
    const auto& a = fixture.layer(layers[2]);
    const auto& b = fixture.layer(layers[3]);
    expect(layers.size() == 4 && a.inPoint == frame(5) && a.outPoint == frame(17) &&
               b.inPoint == frame(17) && b.outPoint == frame(23),
           "sequenced layers run end to end below the solids, first above second");
    expect(session.undo(), "undo the sequence");

    // The active composition cannot be dropped into itself.
    expect(!ui::droppableAsLayer(session.snapshot().project(), fixture.main, fixture.main) &&
               !fixture.drop({fixture.main}, {}),
           "a composition is never a layer of itself");
}

// Drag events in the Timeline's own coordinates.
bool sendDrag(QWidget& target, const QEvent::Type type, const QPoint position,
              const QMimeData& mime, const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    if (type == QEvent::Drop) {
        QDropEvent event(QPointF(position), Qt::CopyAction, &mime, Qt::LeftButton, modifiers);
        QApplication::sendEvent(&target, &event);
        return event.isAccepted();
    }
    if (type == QEvent::DragEnter) {
        QDragEnterEvent event(position, Qt::CopyAction, &mime, Qt::LeftButton, modifiers);
        QApplication::sendEvent(&target, &event);
        return event.isAccepted();
    }
    QDragMoveEvent event(position, Qt::CopyAction, &mime, Qt::LeftButton, modifiers);
    QApplication::sendEvent(&target, &event);
    return event.isAccepted();
}

// The Assets tree's own drag payload for `names`, selected in that order.
std::unique_ptr<QMimeData> assetsDrag(ui::AssetsEditor& assets, const QStringList& names) {
    auto* tree = assets.findChild<QTreeWidget*>("assetsTree");
    tree->clearSelection();
    for (const auto& name : names)
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->text(0) == name)
                tree->selectionModel()->select(tree->indexFromItem(*it),
                                               QItemSelectionModel::Select |
                                                   QItemSelectionModel::Rows);
    // The model's own mimeData() reorders items; a drag uses the selection order.
    return std::unique_ptr<QMimeData>(
        static_cast<ui::assets::AssetTree*>(tree)->selectionMimeDataForTest());
}

void timelineDrops(Fixture& fixture) {
    auto& session = fixture.session;
    ui::AssetsEditor assets(session);
    ui::TimelineEditor timeline(session, *fixture.preview);
    timeline.resize(1400, 420);
    timeline.show();
    QApplication::processEvents();
    auto* stack = timeline.layerStackForTest();
    auto* lanes = timeline.laneRegionForTest();
    const auto solids = fixture.stack();
    const auto name = [&](const document::AssetId id) {
        return QString::fromStdString(session.snapshot().project().findAsset(id)->name);
    };

    // Over the layer stack: the row insertion line, then a layer inserted at that row.
    expect(session.setCurrentTime(frame(12)), "playhead at frame 12");
    auto mime = assetsDrag(assets, {name(fixture.shotB)});
    expect(mime->hasFormat(ui::kLayerItemsMimeType), "an Assets drag carries its layer items");
    const auto rowPoint = stack->mapTo(&timeline, QPoint(20, stack->rowTop(1) + 3));
    expect(sendDrag(timeline, QEvent::DragEnter, rowPoint, *mime) &&
               sendDrag(timeline, QEvent::DragMove, rowPoint, *mime),
           "the Timeline accepts an Assets drag over the layer stack");
    auto* indicator = stack->findChild<QWidget*>("timelineLayerInsertionIndicator");
    expect(indicator != nullptr && indicator->isVisible() &&
               indicator->geometry().top() == stack->rowTop(1),
           "the insertion line shows above the row under the pointer");
    expect(lanes->dropMarkerRowForTest() == 1 && !lanes->dropMarkerTimeForTest(),
           "over the stack the lanes continue the row line and show no time marker");
    expect(sendDrag(timeline, QEvent::Drop, rowPoint, *mime), "drop on the row");
    auto layers = fixture.stack();
    expect(layers.size() == 3 && layers[0] == solids[0] && layers[2] == solids[1] &&
               fixture.layer(layers[1]).inPoint == frame(12),
           "the layer is inserted at that row and starts at the playhead");
    expect(!indicator->isVisible() && lanes->dropMarkerRowForTest() == -1,
           "the indicator goes away after the drop");
    expect(session.undo(), "undo the row drop");

    // Over the lanes: the time marker at the frame under the pointer; Alt sequences two items.
    const auto axis = timeline.rulerForTest()->axisForWidth(lanes->width());
    expect(axis.has_value(), "timeline axis");
    if (!axis)
        return;
    const auto x = static_cast<int>(std::lround(axis->pixelForTime(frame(30)) + 1.0));
    const auto lanePoint = lanes->mapTo(&timeline, QPoint(x, lanes->rowTop(0) + 3));
    mime = assetsDrag(assets, {name(fixture.shotB), name(fixture.shotA)});
    expect(sendDrag(timeline, QEvent::DragEnter, lanePoint, *mime) &&
               sendDrag(timeline, QEvent::DragMove, lanePoint, *mime),
           "the Timeline accepts the drag over the lanes");
    expect(lanes->dropMarkerTimeForTest() == frame(30) && lanes->dropMarkerRowForTest() == 0,
           "the time marker snaps to frame 30 at the top row");
    auto* marker = lanes->findChild<QWidget*>("timelineDropTimeMarker");
    auto* rowLine = lanes->findChild<QWidget*>("timelineDropRowLine");
    expect(marker != nullptr && marker->isVisible() && rowLine != nullptr && rowLine->isVisible() &&
               std::abs(marker->x() - static_cast<int>(axis->pixelForTime(frame(30)))) <= 1,
           "the lanes show the time marker at frame 30 and the row line");
    expect(sendDrag(timeline, QEvent::Drop, lanePoint, *mime, Qt::AltModifier),
           "Alt-drop two items over the lanes");
    layers = fixture.stack();
    expect(layers.size() == 4, "two layers added");
    if (layers.size() == 4) {
        const auto& first = fixture.layer(layers[0]);
        const auto& second = fixture.layer(layers[1]);
        expect(first.inPoint == frame(30) && first.outPoint == frame(36) &&
                   second.inPoint == frame(36) && second.outPoint == frame(48),
               "in selection order (shotB then shotA), end to end from frame 30");
        expect(selected(fixture, {layers[0], layers[1]}),
               "both new layers are selected after the drop, the uppermost primary");
    }
    expect(session.undo() && fixture.stack() == solids, "the Alt drop is one undo step");

    // Elsewhere in the panel: on top at the playhead.
    QPoint elsewhere;
    for (int y = 0; y < timeline.height() && elsewhere.isNull(); ++y) {
        const QPoint candidate(timeline.width() / 2, y);
        if (!stack->rect().contains(stack->mapFrom(&timeline, candidate)) &&
            !lanes->rect().contains(lanes->mapFrom(&timeline, candidate)))
            elsewhere = candidate;
    }
    mime = assetsDrag(assets, {name(fixture.still), "Nested"});
    expect(sendDrag(timeline, QEvent::DragEnter, elsewhere, *mime) &&
               sendDrag(timeline, QEvent::Drop, elsewhere, *mime),
           "a drop elsewhere in the Timeline");
    layers = fixture.stack();
    expect(layers.size() == 4 && fixture.layer(layers[0]).inPoint == frame(12) &&
               fixture.layer(layers[1]).inPoint == frame(12) &&
               nestedSourceOf(fixture.composition(), fixture.layer(layers[1])) == fixture.nested,
           "elsewhere the layers go on top at the playhead; the composition is nested");
    expect(session.undo(), "undo");

    // A drag that leaves hides every indicator.
    mime = assetsDrag(assets, {name(fixture.shotA)});
    expect(sendDrag(timeline, QEvent::DragEnter, rowPoint, *mime), "drag enters again");
    QDragLeaveEvent leave;
    QApplication::sendEvent(&timeline, &leave);
    expect(!indicator->isVisible() && lanes->dropMarkerRowForTest() == -1 &&
               !lanes->dropMarkerTimeForTest() && !marker->isVisible() && !rowLine->isVisible(),
           "leaving hides the insertion line and the time marker");
}

// Files from a file manager: imported through the import flow, then layers in a second step.
void fileDropImportsThenAddsLayers(Fixture& fixture) {
    auto& session = fixture.session;
    ui::TimelineEditor timeline(session, *fixture.preview);
    timeline.resize(1400, 420);
    timeline.show();
    QApplication::processEvents();
    auto* lanes = timeline.laneRegionForTest();
    const auto solids = fixture.stack();
    QImage image(32, 18, QImage::Format_RGB32);
    image.fill(Qt::green);
    const auto path = fixture.directory.filePath("fresh.png");
    expect(image.save(path), "write the dropped file");
    QString followUp;
    QObject::connect(
        fixture.controller.get(), &ui::AssetController::importFinished,
        [&](const ui::AssetController::ImportReport& report) { followUp = report.followUp; });
    const auto axis = timeline.rulerForTest()->axisForWidth(lanes->width());
    const auto x = static_cast<int>(std::lround(axis->pixelForTime(frame(40)) + 1.0));
    const auto point = lanes->mapTo(&timeline, QPoint(x, lanes->rowTop(0) + 3));
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(path)});
    const auto assetsBefore = session.snapshot().project().assets().size();
    expect(sendDrag(timeline, QEvent::DragEnter, point, mime) &&
               sendDrag(timeline, QEvent::DragMove, point, mime),
           "the Timeline accepts files");
    expect(lanes->dropMarkerTimeForTest() == frame(40), "files show the time marker too");
    expect(sendDrag(timeline, QEvent::Drop, point, mime), "drop the file");
    expect(waitUntil([&] { return !fixture.controller->busy(); }), "the import finishes");
    const auto layers = fixture.stack();
    expect(session.snapshot().project().assets().size() == assetsBefore + 1,
           "the file is imported");
    expect(layers.size() == solids.size() + 1 && fixture.layer(layers[0]).inPoint == frame(40),
           "a layer for it starts at the dropped frame, on top");
    expect(followUp == "Added 1 layer", "the import report says what the drop added");
    expect(selected(fixture, {layers[0]}), "the layer the file drop added is selected");
    expect(session.undo() && fixture.stack() == solids &&
               session.snapshot().project().assets().size() == assetsBefore + 1,
           "the first undo removes the layer and keeps the import");
    expect(session.undo() && session.snapshot().project().assets().size() == assetsBefore,
           "the second undo removes the import");
}

// The Viewer: layers at the playhead, centred in the composition.
void viewerDrop(Fixture& fixture) {
    auto& session = fixture.session;
    ui::AssetsEditor assets(session);
    ui::ViewerEditor viewer(session, *fixture.preview, nullptr);
    viewer.resize(640, 400);
    viewer.show();
    QApplication::processEvents();
    expect(session.setCurrentTime(frame(20)), "playhead at frame 20");
    const auto before = fixture.stack();
    auto mime = assetsDrag(
        assets,
        {QString::fromStdString(session.snapshot().project().findAsset(fixture.still)->name),
         QStringLiteral("Nested")});
    const QPoint centre(viewer.width() / 2, viewer.height() / 2);
    expect(sendDrag(viewer, QEvent::DragEnter, centre, *mime) &&
               sendDrag(viewer, QEvent::Drop, centre, *mime),
           "the Viewer accepts an Assets drop");
    const auto layers = fixture.stack();
    expect(layers.size() == before.size() + 2 && fixture.layer(layers[0]).inPoint == frame(20) &&
               fixture.layer(layers[1]).inPoint == frame(20),
           "the layers go on top at the playhead");
    expect(selected(fixture, {layers[0], layers[1]}), "the Viewer drop selects its new layers");
    const auto& composition = fixture.composition();
    for (std::size_t index = 0; index < 2 && index < layers.size(); ++index) {
        const auto* node = composition.graph().findNode(fixture.layer(layers[index]).nodeId);
        std::optional<document::Vec2d> position;
        for (const auto& binding : node ? node->parameters : decltype(node->parameters){})
            if (binding.role == document::kPositionParameterRole)
                if (const auto* parameter = composition.parameters().find(binding.parameterId))
                    if (const auto* constant =
                            std::get_if<document::ConstantValueSource>(&parameter->source))
                        if (const auto* value = std::get_if<document::Vec2d>(&constant->value))
                            position = *value;
        expect(position && position->x == 160.0 && position->y == 90.0,
               "each layer (footage and nested composition) is centered in the composition");
    }
    expect(session.undo() && fixture.stack() == before, "the Viewer drop is one undo step");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("BloomLayerDropTests"));
    ui::kit::installKinetikTheme(app);
    {
        Fixture fixture;
        expect(fixture.still.isValid() && fixture.shotA.isValid() && fixture.shotB.isValid(),
               "fixture assets");
        operationPlacesAndTimesLayers(fixture);
        timelineDrops(fixture);
        fileDropImportsThenAddsLayers(fixture);
        viewerDrop(fixture);
    }
    if (failures != 0)
        return 1;
    std::cout << "PASS: layer drops\n";
    return 0;
}
