// #364 Cryptomatte UI on a real document: a multi-rank Cryptomatte EXR generated in-test feeds a
// Cryptomatte node. Properties lists the layer from a worker read and edits the matte list (name
// field, remove buttons, undo); the Viewer channel menu previews the matte; the Viewer eyedropper
// adds with a click or Shift-click and removes with Alt-click, reading the object under the cursor
// on a worker (manifest names, and raw IDs without a manifest).

#include "cryptomatte_fixture_support.hpp"
#include "layer_parent_transform.hpp"

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/cryptomatte_inspection.hpp>
#include <bloom/runtime/cryptomatte_matte_list.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/cryptomatte_controller.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QTest>
#include <QToolButton>

#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace bloom;
namespace fixture = bloom::cryptomatte_fixture;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Predicate> bool waitUntil(Predicate predicate, const int milliseconds = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < milliseconds)
        QTest::qWait(5);
    return predicate();
}

struct Scene final {
    std::filesystem::path folder;
    document::NewProject initial;
    document::Document document{std::move(initial.project)};
    commands::CommandStack stack{document};
    ui::CompositionSession session{document, stack, initial.initialCompositionId};
    document::NodeId source;
    document::NodeId layer;
    document::NodeId cryptomatte;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Cryptomatte UI", session.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = session.executeTransaction(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("fixture command refused");
        return result;
    }

    // `sequence` writes a two-frame sequence whose second frame names different objects.
    explicit Scene(const bool manifest, const std::uint32_t width = fixture::kWidth,
                   const std::uint32_t height = fixture::kHeight, const bool sequence = false)
        : initial(document::makeNewProject(
              "Cryptomatte UI", "Main", core::RationalTime::fromInteger(1),
              *document::CompositionFormat::create(width, height, core::PixelAspectRatio::square(),
                                                   *document::FrameRate::create(24, 1)))) {
        std::mt19937_64 random{std::random_device{}()};
        folder = std::filesystem::temp_directory_path() /
                 ("bloom-cryptomatte-ui-" + std::to_string(random()));
        std::filesystem::create_directories(folder);
        const auto first = folder / (sequence ? "crypto.0001.exr" : "crypto.exr");
        fixture::writeCryptomatteExr(first, manifest);
        if (sequence)
            fixture::writeCryptomatteExr(folder / "crypto.0002.exr", manifest, "CryptoObject", {},
                                         1);
        const auto composition = session.compositionId();
        run<commands::ImportAssets>(std::vector{first}, folder);
        run<commands::AddImageLayer>(composition, session.snapshot().project().assets().front().id);
        for (const auto& node : session.composition()->graph().nodes()) {
            if (node.typeId == document::kImageSourceNodeType)
                source = node.id;
            if (node.typeId == document::kLayerOutputNodeType)
                layer = node.id;
        }
        cryptomatte =
            *run<commands::AddNode>(composition, std::string(document::kCryptomatteNodeType),
                                    document::Vec2d{})
                 .outputId<document::NodeId>(commands::kAddNodeOutput);
        run<commands::ConnectPorts>(
            composition,
            document::OutputPortRef{source, std::string(document::kImageSourceChannelsOutputPort)},
            document::NodeInputRef{cryptomatte, std::string(document::kChannelSetPort)});
        run<commands::ConnectPorts>(
            composition,
            document::OutputPortRef{cryptomatte,
                                    std::string(document::kCryptomatteImageOutputPort)},
            document::NodeInputRef{layer, std::string(document::kLayerOutputContentInputPort)});
    }
    ~Scene() {
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    }
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
};

struct Pipeline final {
    runtime::NodeDefinitionRegistry definitions;
    std::unique_ptr<runtime::SnapshotCompiler> compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualified;
    Pipeline() {
        if (!runtime::registerBuiltInNodeDefinitions(definitions))
            throw std::runtime_error("built-in definitions");
        definitions.freeze();
        compiler = std::make_unique<runtime::SnapshotCompiler>(definitions);
    }
};

[[nodiscard]] ui::CryptomatteController::LayerProvider layerProvider(Pipeline& pipeline,
                                                                     std::filesystem::path base) {
    return [&pipeline, base](const document::Snapshot& snapshot,
                             const document::CompositionId composition, const document::NodeId node,
                             const core::RationalTime time,
                             const runtime::EvaluationColorIntent& colorIntent,
                             const runtime::CancellationToken& cancellation) {
        return runtime::listCryptomatteLayers(*pipeline.compiler, snapshot, composition, node, base,
                                              time, colorIntent, cancellation);
    };
}

[[nodiscard]] ui::CryptomatteController::PickProvider pickProvider(Pipeline& pipeline,
                                                                   std::filesystem::path base) {
    return [&pipeline,
            base](const document::Snapshot& snapshot, const document::CompositionId composition,
                  const document::NodeId node, const core::RationalTime time,
                  const runtime::EvaluationColorIntent& colorIntent,
                  const std::optional<runtime::ChannelInspection>& inspection, const std::int64_t x,
                  const std::int64_t y, const runtime::CancellationToken& cancellation) {
        return runtime::pickCryptomatte(*pipeline.compiler, snapshot, composition, node, base, time,
                                        colorIntent, inspection, x, y, cancellation);
    };
}

void shutdown(runtime::TaskScheduler& scheduler, ui::TaskUiBridge& bridge,
              ui::CompositionPreviewController* preview) {
    if (preview != nullptr)
        preview->beginShutdown();
    bridge.beginShutdown();
    scheduler.beginShutdown();
    QElapsedTimer timer;
    timer.start();
    while (!scheduler.isQuiescent() && timer.elapsed() < 4000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

void testPropertiesAndViewer() {
    Scene scene(true);
    Pipeline pipeline;
    pipeline.evaluator.setAssetBaseDirectory(scene.folder);
    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge(scheduler, nullptr, std::chrono::milliseconds{1});
    ui::CompositionPreviewController preview(
        scene.session, scheduler, bridge,
        ui::makeCompositionPreviewPipeline(*pipeline.compiler, pipeline.evaluator,
                                           pipeline.displayPreparer, pipeline.qualified));
    ui::CryptomatteController controller(scene.session, scheduler, bridge,
                                         layerProvider(pipeline, scene.folder),
                                         pickProvider(pipeline, scene.folder));
    expect(scene.session.cryptomatteController() == &controller,
           "the controller registers with the session");
    QWidget window;
    auto* properties = new ui::PropertiesEditor(scene.session, &window);
    auto* viewer = new ui::ViewerEditor(scene.session, preview, nullptr, &window);
    properties->setGeometry(0, 0, 400, 700);
    viewer->setGeometry(400, 0, 800, 700);
    window.resize(1200, 700);
    window.show();
    scene.session.selectNode(scene.cryptomatte);
    QApplication::processEvents();

    // Properties: the layer picker reads the channel set's Cryptomatte types on a worker.
    ui::kit::KDropdown* picker = nullptr;
    expect(waitUntil([&] {
               picker = properties->findChild<ui::kit::KDropdown*>("propertiesCryptomatteLayer");
               return picker != nullptr && picker->toolTip().isEmpty() && picker->count() == 1;
           }),
           "the layer picker fills once the worker listing arrives");
    expect(picker != nullptr && picker->count() == 1 && picker->itemText(0) == "CryptoObject",
           "the layer picker lists exactly CryptoObject");

    // The name field adds; a remove button removes; undo restores.
    auto* field = properties->findChild<ui::kit::KLineEdit*>("propertiesCryptomatteAdd");
    expect(field != nullptr, "the add-matte field exists");
    if (field != nullptr) {
        field->setText("cube, floor");
        emit field->returnPressed();
        QApplication::processEvents();
    }
    expect(controller.mattes(scene.cryptomatte) == std::vector<std::string>{"cube", "floor"},
           "typed names join the matte list");
    QApplication::processEvents();
    std::vector<ui::kit::KButton*> removes;
    expect(waitUntil([&] {
               removes.clear();
               for (auto* button :
                    properties->findChildren<ui::kit::KButton*>("propertiesCryptomatteRemove"))
                   if (button->isVisible())
                       removes.push_back(button);
               return removes.size() == 2;
           }),
           "each selected matte has a remove button");
    for (auto* button : removes)
        if (button->property("matteName").toString() == "cube")
            button->click();
    QApplication::processEvents();
    expect(controller.mattes(scene.cryptomatte) == std::vector<std::string>{"floor"},
           "a remove button drops its matte");
    expect(scene.session.undo() &&
               controller.mattes(scene.cryptomatte) == std::vector<std::string>{"cube", "floor"},
           "removing a matte is one undoable edit");
    expect(controller.setMattes(scene.cryptomatte, {}, "Clear"), "the list clears");

    expect(waitUntil([&] { return preview.state().activity == ui::PreviewActivity::Ready; }),
           "the composition preview renders");
    // Viewer: the channel menu previews the matte (a viewer-only plan of the Mask output).
    auto* dropdown = viewer->findChild<ui::kit::KDropdown*>("viewerChannelDropdown");
    int matteIndex = -1;
    expect(waitUntil([&] {
               dropdown = viewer->findChild<ui::kit::KDropdown*>("viewerChannelDropdown");
               if (dropdown == nullptr)
                   return false;
               for (int index = 0; index < dropdown->count(); ++index)
                   if (dropdown->itemText(index).contains("Cryptomatte matte"))
                       matteIndex = index;
               return matteIndex >= 0;
           }),
           "the Viewer channel menu offers the Cryptomatte matte");
    if (dropdown != nullptr && matteIndex >= 0) {
        dropdown->setCurrentIndex(matteIndex);
        QApplication::processEvents();
        const auto& inspection = preview.viewerChannelInspection();
        expect(inspection.has_value() && inspection->node == scene.cryptomatte &&
                   inspection->port == document::kCryptomatteMaskOutputPort &&
                   inspection->channels.empty(),
               "choosing it inspects the node's Mask output");
        expect(waitUntil([&] {
                   return preview.state().activity == ui::PreviewActivity::Ready &&
                          preview.state().frame != nullptr &&
                          preview.state().frame->desiredIdentity().channelInspection == inspection;
               }),
               "the matte preview renders for the inspection it was asked for");
    }

    // The eyedropper: click adds the dominant object, Shift-click adds, Alt-click removes.
    auto* tool = viewer->findChild<QToolButton*>("viewerCryptomatteTool");
    expect(tool != nullptr, "the Viewer has a Cryptomatte eyedropper");
    if (tool != nullptr)
        tool->click();
    std::optional<ui::ViewerMapping> mapping;
    expect(waitUntil([&] {
               mapping = viewer->mappingForTest();
               return mapping.has_value();
           }),
           "the Viewer has a display mapping");
    if (!mapping) {
        std::cerr << "preview activity " << static_cast<int>(preview.state().activity) << "\n";
        for (const auto& diagnostic : preview.state().diagnostics)
            std::cerr << "  " << diagnostic.summary << "\n";
    }
    const auto click = [&](const double x, const double y, const Qt::KeyboardModifiers modifiers) {
        if (!mapping)
            return;
        const auto point = mapping->toScreen(document::Vec2d{x, y}).toPoint();
        QTest::mouseClick(viewer, Qt::LeftButton, modifiers, point);
        waitUntil([&] { return !controller.pickPending(); });
        QApplication::processEvents();
    };
    // Pixel (1, 0): sphere 0.6, cube 0.4 -- the dominant object is the sphere.
    click(1.5, 0.5, Qt::NoModifier);
    expect(controller.mattes(scene.cryptomatte) == std::vector<std::string>{"sphere"},
           "a click adds the object with the largest coverage under the cursor");
    // Pixel (0, 1): sky.
    click(0.5, 1.5, Qt::ShiftModifier);
    expect(controller.mattes(scene.cryptomatte) == std::vector<std::string>{"sphere", "sky"},
           "Shift-click adds another object");
    click(0.5, 0.5, Qt::AltModifier);
    expect(controller.mattes(scene.cryptomatte) == std::vector<std::string>{"sky"},
           "Alt-click removes the object under the cursor");
    click(2.5, 1.5, Qt::ControlModifier);
    expect(controller.mattes(scene.cryptomatte) == std::vector<std::string>{"sky"},
           "Ctrl-click on an unselected object leaves the list unchanged");
    expect(controller.lastPickMessage().contains("cube"), "the pick reports the object it read");
    // Pixel (1, 1) is empty: nothing is added.
    click(1.5, 1.5, Qt::NoModifier);
    expect(controller.mattes(scene.cryptomatte) == std::vector<std::string>{"sky"} &&
               controller.lastPickMessage() == "No object under the cursor",
           "an empty pixel adds nothing");
    expect(scene.session.undo() &&
               controller.mattes(scene.cryptomatte) == std::vector<std::string>{"sphere", "sky"},
           "each pick is one undoable edit");
    // The matte preview shows exactly the selected objects' coverage.
    expect(waitUntil([&] {
               const auto& state = preview.state();
               return state.activity == ui::PreviewActivity::Ready && state.frame != nullptr &&
                      state.frame->desiredIdentity().sourceRevision ==
                          scene.session.snapshot().revision() &&
                      state.frame->hasProcessFrame();
           }),
           "the matte preview re-renders for the edited selection");
    if (const auto frame = preview.state().frame; frame != nullptr && frame->hasProcessFrame()) {
        bool exact = true;
        for (std::size_t pixel = 0; pixel < fixture::kPixels.size(); ++pixel) {
            const auto value =
                frame->processImage().read(static_cast<std::int64_t>(pixel % fixture::kWidth),
                                           static_cast<std::int64_t>(pixel / fixture::kWidth));
            exact = exact && value &&
                    value.value()->alpha() == fixture::expectedCoverage(pixel, {0, 3});
        }
        expect(exact, "the matte preview equals the sphere + sky coverage");
    }

    shutdown(scheduler, bridge, &preview);
}

// The eyedropper through a moved, scaled and rotated layer, a proxy, a parented layer and a
// time-offset layer: each click aims at the centre of one source pixel, placed in composition space
// by the evaluator's own layer matrix, and must pick that pixel's object.
void testLayerMapping() {
    Scene scene(true, 32, 32, true);
    Pipeline pipeline;
    pipeline.evaluator.setAssetBaseDirectory(scene.folder);
    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge(scheduler, nullptr, std::chrono::milliseconds{1});
    ui::CompositionPreviewController preview(
        scene.session, scheduler, bridge,
        ui::makeCompositionPreviewPipeline(*pipeline.compiler, pipeline.evaluator,
                                           pipeline.displayPreparer, pipeline.qualified));
    ui::CryptomatteController controller(scene.session, scheduler, bridge,
                                         layerProvider(pipeline, scene.folder),
                                         pickProvider(pipeline, scene.folder));
    QWidget window;
    auto* viewer = new ui::ViewerEditor(scene.session, preview, nullptr, &window);
    viewer->setGeometry(0, 0, 900, 900);
    window.resize(900, 900);
    window.show();
    const auto composition = scene.session.compositionId();
    document::LayerId layer;
    for (const auto& boundary : scene.session.composition()->graph().layerOutputs())
        if (boundary.nodeId == scene.layer)
            layer = boundary.layerId;
    const auto setTransform = [&](const document::NodeId node, const document::Vec2d position,
                                  const document::Vec2d anchor, const document::Vec2d scale,
                                  const double rotation) {
        scene.run<commands::SetNodeParameterValue>(composition, node,
                                                   std::string(document::kPositionParameterRole),
                                                   document::ParameterValue{position});
        scene.run<commands::SetNodeParameterValue>(composition, node,
                                                   std::string(document::kAnchorParameterRole),
                                                   document::ParameterValue{anchor});
        scene.run<commands::SetNodeParameterValue>(composition, node,
                                                   std::string(document::kScaleParameterRole),
                                                   document::ParameterValue{scale});
        scene.run<commands::SetNodeParameterValue>(composition, node,
                                                   std::string(document::kRotationParameterRole),
                                                   document::ParameterValue{rotation});
    };
    // The last current mapping: a pick re-renders the frame, and the next click aims through the
    // geometry the artist still sees.
    std::optional<ui::ViewerMapping> lastMapping;
    const auto settle = [&](const std::string& label) {
        expect(waitUntil([&] {
                   const auto& state = preview.state();
                   return state.activity == ui::PreviewActivity::Ready && state.frame != nullptr &&
                          state.frame->desiredIdentity().sourceRevision ==
                              scene.session.snapshot().revision() &&
                          viewer->mappingForTest().has_value();
               }),
               label + ": the Viewer shows the edited composition");
        lastMapping = viewer->mappingForTest();
    };
    // Picking through the composition (the ID-colour preview, which shows the node in its own
    // frame, is off here).
    controller.setPreview(scene.cryptomatte, false);
    scene.session.selectNode(scene.cryptomatte);
    if (auto* tool = viewer->findChild<QToolButton*>("viewerCryptomatteTool"))
        tool->click();
    const auto pickAt = [&](const runtime::detail::LayerMatrix& world, const int pixel,
                            const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        const auto& mapping = lastMapping;
        if (!mapping)
            return;
        const auto point = world.map({(pixel % fixture::kWidth) + 0.5, (pixel / fixture::kWidth) + 0.5});
        QTest::mouseClick(viewer, Qt::LeftButton, modifiers, mapping->toScreen(point).toPoint());
        waitUntil([&] { return !controller.pickPending(); });
        QApplication::processEvents();
    };
    using runtime::detail::LayerMatrix;

    // A layer pivots about its content centre plus its anchor (the 4x2 source's centre is (2, 1);
    // a parent solid's is the composition centre).
    const auto authored = [](const document::Vec2d position, const document::Vec2d centre,
                             const document::Vec2d scale, const double rotation) {
        return LayerMatrix::authored(position, centre, scale, rotation);
    };
    const document::Vec2d centre{2, 1};

    // Moved, 4x scaled and rotated a quarter turn about the content centre.
    setTransform(scene.layer, {16, 16}, {0, 0}, {4, 4}, 90);
    settle("rotated");
    const auto rotated = authored({16, 16}, centre, {4, 4}, 90);
    pickAt(rotated, 6);
    expect(controller.mattes(scene.cryptomatte) == std::vector<std::string>{"cube"},
           "a scaled, rotated layer picks the object under the cursor");
    pickAt(rotated, 4, Qt::ShiftModifier);
    expect(controller.mattes(scene.cryptomatte) == std::vector<std::string>{"cube", "sky"},
           "a second pick through the rotated layer adds its object");
    pickAt(LayerMatrix{}, 0); // composition pixel (0, 0) lies outside the rotated content
    expect(controller.lastPickMessage() == "No object under the cursor" &&
               controller.mattes(scene.cryptomatte) == std::vector<std::string>{"cube", "sky"},
           "a point outside the source reports no object");

    // The same layer under a half-resolution proxy.
    preview.setResolutionPolicy(runtime::PreviewResolutionPolicy::Half);
    expect(controller.setMattes(scene.cryptomatte, {}, "Clear"), "clear before the proxy pick");
    settle("proxy");
    pickAt(rotated, 0);
    expect(controller.mattes(scene.cryptomatte) == std::vector<std::string>{"sphere"},
           "a proxy preview picks the full-resolution object");
    preview.setResolutionPolicy(runtime::PreviewResolutionPolicy::Full);

    // A parented layer: the parent's rotation and offset compose with the child's own transform.
    const auto before = scene.session.composition()->graph().layerOutputs();
    scene.run<commands::AddSolidLayer>(composition, "Parent", core::Color4d{0, 0, 0, 0});
    document::LayerId parent;
    document::NodeId parentNode;
    for (const auto& boundary : scene.session.composition()->graph().layerOutputs())
        if (std::ranges::none_of(before, [&](const auto& old) { return old.layerId == boundary.layerId; })) {
            parent = boundary.layerId;
            parentNode = boundary.nodeId;
        }
    setTransform(parentNode, {16, 16}, {0, 0}, {1, 1}, 30);
    setTransform(scene.layer, {18, 15}, {0, 0}, {3, 3}, 0);
    scene.run<commands::SetLayerParent>(composition, layer, parent);
    expect(controller.setMattes(scene.cryptomatte, {}, "Clear"), "clear before the parent pick");
    settle("parented");
    const auto parented = authored({16, 16}, {16, 16}, {1, 1}, 30)
                              .times(authored({18, 15}, centre, {3, 3}, 0));
    pickAt(parented, 6);
    expect(controller.mattes(scene.cryptomatte) == std::vector<std::string>{"cube"},
           "a parented layer picks through its parent's transform");
    scene.run<commands::SetLayerParent>(composition, layer, std::nullopt);

    // A time-offset layer: at composition time 0 the layer shows the sequence's second frame, whose
    // pixel 0 is the cube rather than the sphere.
    setTransform(scene.layer, {16, 16}, {0, 0}, {4, 4}, 0);
    scene.run<commands::SetLayerTiming>(composition, layer, *core::RationalTime::create(-1, 24),
                                        core::RationalTime{}, core::RationalTime::fromInteger(1));
    expect(controller.setMattes(scene.cryptomatte, {}, "Clear"), "clear before the time pick");
    settle("time offset");
    pickAt(authored({16, 16}, centre, {4, 4}, 0), 0);
    expect(controller.mattes(scene.cryptomatte) == std::vector<std::string>{"cube"},
           "a time-offset layer picks from the source frame it shows");

    shutdown(scheduler, bridge, &preview);
}

void testRawIdsWithoutManifest() {
    Scene scene(false);
    Pipeline pipeline;
    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge(scheduler, nullptr, std::chrono::milliseconds{1});
    ui::CryptomatteController controller(scene.session, scheduler, bridge,
                                         layerProvider(pipeline, scene.folder),
                                         pickProvider(pipeline, scene.folder));
    controller.pick(scene.cryptomatte, 0, 0, ui::CryptomatteController::PickAction::Add);
    expect(waitUntil([&] { return !controller.pickPending(); }), "the pick completes");
    QApplication::processEvents();
    const auto expected = runtime::formatCryptomatteRawId(runtime::cryptomatteNameId("sphere"));
    expect(controller.mattes(scene.cryptomatte) == std::vector<std::string>{expected},
           "without a manifest the pick stores the raw ID");
    controller.requestLayers(scene.cryptomatte);
    expect(waitUntil([&] { return controller.layers(scene.cryptomatte) != nullptr; }),
           "the layer listing completes");
    const auto* layers = controller.layers(scene.cryptomatte);
    expect(layers != nullptr && *layers == std::vector<std::string>{"CryptoObject"},
           "the layer listing does not need a manifest");
    shutdown(scheduler, bridge, nullptr);
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        testPropertiesAndViewer();
        testRawIdsWithoutManifest();
        testLayerMapping();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "PASS: Cryptomatte UI\n";
    return 0;
}
