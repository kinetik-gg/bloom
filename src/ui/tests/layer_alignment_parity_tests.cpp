// ADR 0031: the interactive and headless layer bounds agree. The Viewer aligns from the bounds of
// the frame it was delivered (CompositionPreviewController::currentLayerBounds, no UI-thread
// evaluation); scripting and MCP measure with runtime::queryLayerBounds. For the same snapshot and
// time both give the same bounds, the same alignment geometry and the same planned move.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/layer_alignment_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/host/layer_alignment.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/layer_bounds_query.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <source_location>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace bloom;

int failures = 0;

void expect(const bool condition, const std::string& message,
            const std::source_location location = std::source_location::current()) {
    if (!condition) {
        ++failures;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
}

template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 8'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (std::invoke(predicate))
            return true;
        std::this_thread::yield();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return std::invoke(predicate);
}

[[nodiscard]] bool near(const double a, const double b) { return std::abs(a - b) <= 1e-9; }

[[nodiscard]] bool same(const runtime::EvaluatedOperationBounds& a,
                        const runtime::EvaluatedOperationBounds& b) {
    bool result = a.layerId == b.layerId && near(a.output.left, b.output.left) &&
                  near(a.output.top, b.output.top) && near(a.output.right, b.output.right) &&
                  near(a.output.bottom, b.output.bottom) && near(a.local.left, b.local.left) &&
                  near(a.local.right, b.local.right) && near(a.local.top, b.local.top) &&
                  near(a.local.bottom, b.local.bottom);
    for (std::size_t corner = 0; corner < 4; ++corner)
        result = result && near(a.polygon[corner].x, b.polygon[corner].x) &&
                 near(a.polygon[corner].y, b.polygon[corner].y);
    return result;
}

struct PipelineFixture final {
    runtime::NodeDefinitionRegistry definitions;
    runtime::SnapshotCompiler compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    ui::PreviewPreparationFunction pipeline;

    PipelineFixture() : compiler(definitions) {
        if (!runtime::registerBuiltInNodeDefinitions(definitions))
            std::abort();
        definitions.freeze();
        pipeline = ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                      qualifiedProcessorProvider);
    }
};

struct Layer final {
    document::LayerId id;
    commands::CommandResult added;
};

template <typename Operation, typename... Arguments>
commands::CommandResult run(document::Document& document, commands::CommandStack& stack,
                            Arguments&&... args) {
    commands::Transaction transaction("Fixture", document.snapshot().revision());
    transaction.emplace<Operation>(std::forward<Arguments>(args)...);
    auto result = stack.execute(std::move(transaction));
    if (!result.succeeded())
        std::abort();
    return result;
}

void testDeliveredFrameMatchesHeadlessQuery() {
    auto project = document::makeNewProject("Parity", "Main", core::RationalTime::fromInteger(10),
                                            document::CompositionFormat::create(1000, 500).value());
    const auto compositionId = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    // The layers exist before the session so its constructor-time snapshot holds them.
    const auto add = [&](const char* name, const document::Vec2d position) {
        auto result = run<commands::AddSolidLayer>(document, stack, compositionId, name,
                                                   core::Color4d{0.5, 0.5, 0.5, 1}, position);
        return Layer{
            result.outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput).value(),
            result};
    };
    const auto parameter = [](const Layer& layer, const std::string_view output) {
        return layer.added.outputId<document::ParameterId>(output).value();
    };
    const auto parent = add("Parent", {300, 250});
    const auto child = add("Child", {40, -30});
    const auto free = add("Free", {800, 120});
    run<commands::SetParameterSource>(
        document, stack, compositionId,
        parameter(parent, commands::kAddSolidLayerRotationParameterOutput),
        document::ConstantValueSource{30.0});
    run<commands::SetParameterSource>(
        document, stack, compositionId,
        parameter(parent, commands::kAddSolidLayerScaleParameterOutput),
        document::ConstantValueSource{document::Vec2d{0.4, 0.2}});
    run<commands::SetParameterSource>(
        document, stack, compositionId,
        parameter(child, commands::kAddSolidLayerRotationParameterOutput),
        document::ConstantValueSource{15.0});
    run<commands::SetParameterSource>(document, stack, compositionId,
                                      parameter(free, commands::kAddSolidLayerScaleParameterOutput),
                                      document::ConstantValueSource{document::Vec2d{0.1, 0.3}});
    run<commands::SetLayerParent>(document, stack, compositionId, child.id, parent.id);

    ui::CompositionSession session(document, stack, compositionId);
    runtime::TaskScheduler scheduler({.cpuWorkerCount = 1,
                                      .blockingIoWorkerCount = 1,
                                      .cpuQueueCapacity = 16,
                                      .blockingIoQueueCapacity = 4,
                                      .terminalHistoryCapacity = 32,
                                      .diagnosticsPerTask = 8,
                                      .groupRegistryCapacity = 8});
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    ui::CompositionPreviewController controller(session, scheduler, bridge, fixture.pipeline);
    expect(waitUntil([&] {
               return controller.state().activity == ui::PreviewActivity::Ready &&
                      controller.state().frame != nullptr;
           }),
           "the preview delivers a frame");
    const auto frame = controller.state().frame;
    if (frame != nullptr) {
        const auto delivered = controller.currentLayerBounds(frame);
        const auto time = frame->desiredIdentity().time;
        const auto snapshot = session.snapshot();
        const auto headless = runtime::queryLayerBounds(
            fixture.compiler, fixture.evaluator,
            {.snapshot = snapshot, .composition = compositionId, .time = time, .layers = {}});
        expect(headless.succeeded(), "the headless query evaluates: " + headless.diagnostic);
        expect(delivered.size() == 3 && headless.bounds.size() == delivered.size(),
               "both paths report every visible layer");
        for (std::size_t index = 0; index < std::min(delivered.size(), headless.bounds.size());
             ++index)
            expect(same(delivered[index], headless.bounds[index]),
                   "the delivered frame's bounds equal the headless bounds for layer " +
                       std::to_string(index));
        const auto* composition = snapshot.project().findComposition(compositionId);
        const auto fromViewer = host::layerAlignmentGeometry(*composition, time, delivered);
        const auto fromScript = host::layerAlignmentGeometry(*composition, time, headless.bounds);
        commands::LayerAlignmentRequest request{
            .composition = compositionId,
            .time = time,
            .layers = {child.id, free.id},
            .arrangement = commands::LayerArrangement::AlignHorizontalCentre,
            .target = commands::AlignmentTarget::KeyObject,
            .keyLayer = parent.id,
            .geometry = fromViewer};
        const auto viewerPlan = commands::planLayerAlignment(*composition, request);
        request.geometry = fromScript;
        const auto scriptPlan = commands::planLayerAlignment(*composition, request);
        bool samePlan = !viewerPlan.rejected() && viewerPlan.moves.size() == 2 &&
                        viewerPlan.moves.size() == scriptPlan.moves.size();
        for (std::size_t index = 0; samePlan && index < viewerPlan.moves.size(); ++index)
            samePlan = viewerPlan.moves[index].layer == scriptPlan.moves[index].layer &&
                       near(viewerPlan.moves[index].value.x, scriptPlan.moves[index].value.x) &&
                       near(viewerPlan.moves[index].value.y, scriptPlan.moves[index].value.y);
        expect(samePlan, "the Viewer and the headless query plan the same alignment");
    }

    bool quiescent = false;
    QObject::connect(&bridge, &ui::TaskUiBridge::shutdownQuiescent, &bridge,
                     [&quiescent] { quiescent = true; });
    controller.beginShutdown();
    bridge.beginShutdown();
    expect(waitUntil([&] { return quiescent && scheduler.isQuiescent(); }),
           "the parity fixture reaches scheduler quiescence");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QCoreApplication application(argc, argv);
    testDeliveredFrameMatchesHeadlessQuery();
    if (failures != 0) {
        std::cerr << failures << " layer alignment parity checks failed\n";
        return 1;
    }
    return 0;
}
