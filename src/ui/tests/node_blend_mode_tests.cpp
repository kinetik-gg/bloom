// Task FIX1 item E: blend modes, end to end. Two overlapping layers, the mode changed through the
// session write every surface (timeline row, Properties, node card) calls, and the frame compared
// against the blend kernel's own answer -- plus the preview controller, because "the viewer does
// not change" is a claim about the preview path and not only about the evaluator.
#include "node_production_harness.hpp"

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QElapsedTimer>
#include <QEventLoop>

#include <chrono>
#include <thread>
#include <vector>

using namespace bloom;
using namespace bloom::ui;
using namespace bloom::ui::production_test;
using namespace std::chrono_literals;

namespace {
template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 6'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (predicate())
            return true;
        std::this_thread::yield();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

// The kernel's own answer for one premultiplied source over one premultiplied backdrop.
[[nodiscard]] std::array<float, 4> kernelGolden(const core::BlendMode mode,
                                                const std::array<float, 4>& source,
                                                const std::array<float, 4>& backdrop) {
    const auto sourcePixel =
        render::Rgba32f::fromPremultiplied(source[0], source[1], source[2], source[3]);
    const auto destination =
        render::Rgba32f::fromPremultiplied(backdrop[0], backdrop[1], backdrop[2], backdrop[3]);
    if (!sourcePixel || !destination)
        return {};
    std::array<render::Rgba32f, 1> sourceRow{*sourcePixel.value()};
    std::array<render::Rgba32f, 1> destinationRow{*destination.value()};
    if (render::blendLinearRec709SceneRow(mode, sourceRow, destinationRow))
        return {};
    return {destinationRow[0].red(), destinationRow[0].green(), destinationRow[0].blue(),
            destinationRow[0].alpha()};
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        // --- Evidence first: ONE layer over the composition's transparent backdrop. --------------
        {
            App one;
            expect(one.session.addSolidLayer(QStringLiteral("Only"),
                                             core::Color4d{1.0, 0.5, 0.25, 0.5}),
                   "one straight half-alpha layer over transparent");
            const auto layerId = one.session.composition()->graph().layerOutputs().front().layerId;
            const auto normal = composite(one);
            expect(one.session.setLayerBlendMode(layerId, core::BlendMode::Multiply),
                   "its mode is set to Multiply");
            const auto multiply = composite(one);
            expect(
                normal.ok && multiply.ok && normal.centerPixel == multiply.centerPixel,
                "over a TRANSPARENT backdrop every mode equals Normal -- the W3C fold, not a bug");
        }

        // --- Two overlapping layers: the S6 fixture. ---------------------------------------------
        App a;
        expect(a.session.addSolidLayer(QStringLiteral("Bottom"), core::Color4d{0.2, 0.2, 0.2, 1.0}),
               "an opaque bottom layer");
        expect(a.session.addSolidLayer(QStringLiteral("Top"), core::Color4d{1.0, 0.5, 0.25, 0.5}),
               "a straight half-alpha top layer over it");
        const auto boundaries = a.session.composition()->graph().layerOutputs();
        expect(boundaries.size() == 2, "two layers");
        if (boundaries.size() != 2)
            return 1;
        // Entry ZERO is the topmost layer: the evaluator folds the stack from its last entry to its
        // first, and a newly added layer lands on top. So the layer whose blend mode has anything
        // beneath it to combine with is the front entry -- and it is the layer the artist just
        // added.
        const auto topLayer =
            a.session.composition()->graph().layerStack().entries().front().layerId;
        expect(a.session.composition()->graph().layerOutputs().back().layerId == topLayer ||
                   a.session.composition()->graph().layerOutputs().front().layerId == topLayer,
               "the topmost stack entry is a real layer");
        {
            const auto* boundary = &a.session.composition()->graph().layerOutputs().front();
            for (const auto& candidate : a.session.composition()->graph().layerOutputs())
                if (candidate.layerId == topLayer)
                    boundary = &candidate;
            expect(boundary->name == "Top",
                   "a newly added layer lands on TOP of the stack, not underneath everything");
        }

        const auto normal = composite(a);
        expect(normal.ok, "the two-layer composition evaluates");
        std::cerr << "DIAG normal: " << normal.centerPixel[0] << ' ' << normal.centerPixel[1] << ' '
                  << normal.centerPixel[2] << ' ' << normal.centerPixel[3] << '\n';

        // What the kernel says the same fold should produce, from the same two premultiplied
        // pixels.
        const std::array<float, 4> backdrop{0.2F, 0.2F, 0.2F, 1.0F};
        const std::array<float, 4> source{0.5F, 0.25F, 0.125F, 0.5F};
        for (const auto mode : {core::BlendMode::Multiply, core::BlendMode::Screen,
                                core::BlendMode::Add, core::BlendMode::Normal}) {
            expect(a.session.setLayerBlendMode(topLayer, mode), "the top layer's mode is written");
            const auto frame = composite(a);
            const auto golden = kernelGolden(mode, source, backdrop);
            std::cerr << "DIAG mode " << static_cast<int>(core::blendModeStoredValue(mode)) << ": "
                      << frame.centerPixel[0] << ' ' << frame.centerPixel[1] << ' '
                      << frame.centerPixel[2] << ' ' << frame.centerPixel[3] << "  golden "
                      << golden[0] << ' ' << golden[1] << ' ' << golden[2] << ' ' << golden[3]
                      << '\n';
            expect(frame.ok && closeTo(frame.centerPixel[0], static_cast<double>(golden[0])) &&
                       closeTo(frame.centerPixel[1], static_cast<double>(golden[1])) &&
                       closeTo(frame.centerPixel[2], static_cast<double>(golden[2])) &&
                       closeTo(frame.centerPixel[3], static_cast<double>(golden[3])),
                   "the composited frame matches the blend kernel's own answer for that mode");
        }
        expect(a.session.setLayerBlendMode(topLayer, core::BlendMode::Multiply),
               "back to Multiply for the preview check");
        const auto multiplied = composite(a);
        expect(multiplied.centerPixel != normal.centerPixel,
               "and a mode change actually changes the frame with two overlapping layers");

        // --- Through the PREVIEW CONTROLLER, which is what the viewer shows. --------------------
        {
            runtime::TaskScheduler scheduler({.cpuWorkerCount = 1,
                                              .blockingIoWorkerCount = 1,
                                              .cpuQueueCapacity = 16,
                                              .blockingIoQueueCapacity = 4,
                                              .terminalHistoryCapacity = 32,
                                              .diagnosticsPerTask = 8,
                                              .groupRegistryCapacity = 8});
            TaskUiBridge bridge(scheduler, nullptr, 1ms);
            runtime::NodeDefinitionRegistry definitions;
            if (!runtime::registerBuiltInNodeDefinitions(definitions))
                return 1;
            definitions.freeze();
            runtime::SnapshotCompiler compiler(definitions);
            runtime::CpuCompositionEvaluator evaluator;
            runtime::CpuReferenceDisplayPreparer preparer;
            runtime::QualifiedDisplayProcessorProvider provider;
            CompositionPreviewController controller(
                a.session, scheduler, bridge,
                makeCompositionPreviewPipeline(compiler, evaluator, preparer, provider));
            expect(waitUntil([&] { return controller.state().activity == PreviewActivity::Ready; }),
                   "the preview reaches Ready for the Multiply frame");
            const auto firstGeneration = controller.state().desiredIdentity.has_value()
                                             ? controller.state().desiredIdentity->requestGeneration
                                             : 0;
            std::vector<render::Rgba8> before;
            if (controller.state().frame) {
                if (const auto view = controller.state().frame->displayBufferView())
                    before.assign(view->pixels.begin(), view->pixels.end());
            }
            expect(!before.empty(), "and publishes a display buffer");

            expect(a.session.setLayerBlendMode(topLayer, core::BlendMode::Screen),
                   "the artist picks Screen");
            expect(waitUntil([&] {
                       const auto& state = controller.state();
                       return state.activity == PreviewActivity::Ready &&
                              state.desiredIdentity.has_value() &&
                              state.desiredIdentity->requestGeneration != firstGeneration;
                   }),
                   "a parameter-only change re-requests the preview and reaches Ready again");
            std::vector<render::Rgba8> after;
            if (controller.state().frame) {
                if (const auto view = controller.state().frame->displayBufferView())
                    after.assign(view->pixels.begin(), view->pixels.end());
            }
            expect(!after.empty() && after != before,
                   "and the published display buffer the viewer paints actually changes");
            controller.beginShutdown();
            bridge.beginShutdown();
            expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                   "the preview fixture reaches quiescence");
        }

        // --- The state is visible on the card: the mode joins the Layer eyebrow. -----------------
        {
            const auto* boundary = &a.session.composition()->graph().layerOutputs().front();
            for (const auto& candidate : a.session.composition()->graph().layerOutputs())
                if (candidate.layerId == topLayer)
                    boundary = &candidate;
            QCoreApplication::processEvents();
            const auto eyebrow = node_editor::nodeEyebrow(
                *a.session.composition(),
                *a.session.composition()->graph().findNode(boundary->nodeId));
            expect(eyebrow.contains(QStringLiteral("Screen")),
                   "a Layer card's eyebrow names the blend mode it is set to");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
