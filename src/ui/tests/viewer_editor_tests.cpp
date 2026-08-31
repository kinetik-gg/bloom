#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QRectF>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "Failure: " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] bool near(const qreal lhs, const qreal rhs) noexcept {
    return std::abs(lhs - rhs) <= 0.0001;
}

bloom::render::ImageExtent extent(const std::uint64_t width, const std::uint64_t height) {
    auto result = bloom::render::ImageExtent::create(width, height);
    if (!result) {
        std::abort();
    }
    return *result.value();
}

void testSquarePixelFitting(Expectations& expectations) {
    const QRectF available(0.0, 0.0, 1000.0, 1000.0);
    const QRectF wide = bloom::ui::fitDisplayRect(available, extent(1920, 1080),
                                                  bloom::core::PixelAspectRatio::square());
    expectations.expect(near(wide.width(), 1000.0) && near(wide.height(), 562.5) &&
                            near(wide.left(), 0.0) && near(wide.top(), 218.75),
                        "wide square-pixel content is centered and width-limited");

    const QRectF tall = bloom::ui::fitDisplayRect(available, extent(100, 200),
                                                  bloom::core::PixelAspectRatio::square());
    expectations.expect(near(tall.width(), 500.0) && near(tall.height(), 1000.0) &&
                            near(tall.left(), 250.0) && near(tall.top(), 0.0),
                        "tall square-pixel content is centered and height-limited");
}

void testPixelAspectFitting(Expectations& expectations) {
    const auto widePixels = bloom::core::PixelAspectRatio::create(2, 1);
    expectations.expect(widePixels.has_value(), "non-square pixel aspect is valid");
    if (!widePixels.has_value()) {
        return;
    }

    const QRectF fitted =
        bloom::ui::fitDisplayRect(QRectF(10.0, 20.0, 300.0, 300.0), extent(100, 100), *widePixels);
    expectations.expect(near(fitted.width(), 300.0) && near(fitted.height(), 150.0) &&
                            near(fitted.left(), 10.0) && near(fitted.top(), 95.0),
                        "pixel width-to-height ratio changes display aspect before fitting");
}

void testDegenerateAvailableRect(Expectations& expectations) {
    const QRectF fitted =
        bloom::ui::fitDisplayRect(QRectF(), extent(1, 1), bloom::core::PixelAspectRatio::square());
    expectations.expect(fitted.isEmpty(), "empty available geometry produces no display rectangle");
}

// --- Issue #97 (task C3): "Viewer: renders the qualified frame's buffer (offscreen smoke), status
// surface shows the color state in the failure case." The rest of this file (below) sets up a real
// CompositionPreviewController/ViewerEditor pair to exercise that, mirroring the fixture shape in
// composition_preview_controller_tests.cpp.

bloom::document::CompositionFormat smallFormat() {
    const auto format = bloom::document::CompositionFormat::create(4, 3);
    if (!format.has_value()) {
        std::abort();
    }
    return *format;
}

bloom::document::NewProject makeTestProject(std::string projectName) {
    return bloom::document::makeNewProject(
        std::move(projectName), "Main", bloom::core::RationalTime::fromInteger(10), smallFormat());
}

bloom::runtime::TaskSchedulerConfig testSchedulerConfig() {
    return {.cpuWorkerCount = 1,
            .blockingIoWorkerCount = 1,
            .cpuQueueCapacity = 16,
            .blockingIoQueueCapacity = 4,
            .terminalHistoryCapacity = 32,
            .diagnosticsPerTask = 8,
            .groupRegistryCapacity = 8};
}

template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 4'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (std::invoke(predicate)) {
            return true;
        }
        std::this_thread::yield();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return std::invoke(predicate);
}

bool isReady(const bloom::ui::CompositionPreviewController& controller) {
    return controller.state().activity == bloom::ui::PreviewActivity::Ready;
}

void reachQuiescence(bloom::ui::CompositionPreviewController& controller,
                     bloom::ui::TaskUiBridge& bridge, bloom::runtime::TaskScheduler& scheduler,
                     Expectations& expectations) {
    bool quiescentSignal = false;
    QObject::connect(&bridge, &bloom::ui::TaskUiBridge::shutdownQuiescent, &bridge,
                     [&quiescentSignal] { quiescentSignal = true; });
    controller.beginShutdown();
    bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return quiescentSignal && scheduler.isQuiescent(); }),
                        "viewer fixture reaches asynchronous scheduler quiescence");
}

// Renders the qualified frame's buffer (offscreen smoke) and confirms the corner/accessibility
// status surface reads "Qualified display" once the qualified processor is ready -- never a silent
// relabel of the earlier reference-labeled frame.
void testViewerRendersQualifiedFrameAndReportsColorState(Expectations& expectations) {
    using namespace bloom;

    auto newProject = makeTestProject("Viewer Qualified Smoke Test");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);

    runtime::NodeDefinitionRegistry definitions;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(definitions),
                        "viewer smoke fixture registers built-in node definitions");
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    const runtime::CpuCompositionEvaluator evaluator;
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProvider;
    auto pipeline =
        ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer, qualifiedProvider);
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline);
    ui::ViewerEditor viewer(session, controller);
    viewer.resize(320, 240);

    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "an initial reference-labeled frame becomes ready");
    const QImage beforeReadiness = viewer.grab().toImage();
    expectations.expect(!beforeReadiness.isNull(),
                        "the viewer renders offscreen before the qualified processor is ready");
    expectations.expect(
        viewer.accessibleDescription().contains(QStringLiteral("Reference display")),
        "the status surface reports the reference (unqualified) color state before "
        "readiness");

    auto resolution = color::resolveBloomNeutralV1BuiltIn(
        color::OcioConfigLocatorKind::BloomBuiltIn, color::kBloomNeutralV1ConfigUri,
        color::kBloomNeutralV1ConfigDigest);
    expectations.expect(resolution.ready(), "the embedded Bloom Neutral built-in resolves");
    auto resolved = std::move(resolution).takeResolved();
    if (resolved.has_value()) {
        auto built = color::buildBloomNeutralCpuDisplayProcessor(*resolved);
        expectations.expect(static_cast<bool>(built), "the qualified processor builds");
        if (built) {
            auto handleValue = std::move(built).takeHandle();
            expectations.expect(handleValue.has_value(), "the built result carries a handle");
            if (handleValue.has_value()) {
                auto shared = std::make_shared<const color::PreparedCpuDisplayProcessorHandle>(
                    std::move(*handleValue));
                qualifiedProvider.publish(
                    runtime::QualifiedDisplayProcessorBuildResult::ready(shared));
            }
        }
    }
    controller.requestRefresh();
    expectations.expect(waitUntil([&] {
                            return isReady(controller) && controller.state().frame != nullptr &&
                                   controller.state().frame->isOcioQualified();
                        }),
                        "a qualified frame becomes ready");

    const QImage afterReadiness = viewer.grab().toImage();
    expectations.expect(!afterReadiness.isNull(),
                        "the viewer renders the qualified frame's buffer offscreen (smoke)");
    expectations.expect(
        viewer.accessibleDescription().contains(QStringLiteral("Qualified display")),
        "the status surface reports the qualified color state once the frame is qualified");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// Fail-closed: the status surface shows the color state (a diagnostic message, plus a corner label
// that never claims "Qualified") when qualification has failed, while the viewer keeps rendering
// its last-good pixels.
void testViewerStatusSurfaceReflectsFailClosedColorState(Expectations& expectations) {
    using namespace bloom;

    auto newProject = makeTestProject("Viewer Fail-Closed Smoke Test");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);

    runtime::NodeDefinitionRegistry definitions;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(definitions),
                        "fail-closed viewer fixture registers built-in node definitions");
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    const runtime::CpuCompositionEvaluator evaluator;
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProvider;
    auto pipeline =
        ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer, qualifiedProvider);
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline);
    ui::ViewerEditor viewer(session, controller);
    viewer.resize(320, 240);

    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the honest startup window still renders a reference-labeled frame");
    const QImage lastGoodImage = viewer.grab().toImage();
    expectations.expect(!lastGoodImage.isNull(), "the last-good frame renders offscreen");

    qualifiedProvider.publish(runtime::QualifiedDisplayProcessorBuildResult::failed(
        {.code = "bloom.test.viewer-qualified-display.forced-failure",
         .severity = runtime::DiagnosticSeverity::Error,
         .summary = "The Bloom Neutral display configuration could not be resolved",
         .detail = {},
         .suggestedAction = {}}));
    controller.requestRefresh();
    expectations.expect(
        waitUntil([&] { return controller.state().activity == ui::PreviewActivity::Failed; }),
        "a request after a forced qualification failure reaches Failed");

    const QImage failedImage = viewer.grab().toImage();
    expectations.expect(!failedImage.isNull(),
                        "the viewer still renders its retained last-good pixels while Failed");
    expectations.expect(
        !viewer.accessibleDescription().contains(QStringLiteral("Qualified display")),
        "the status surface never claims a qualified color state once qualification has failed");
    expectations.expect(
        viewer.accessibleDescription().contains(controller.state().message),
        "the status surface's message reflects the controller's own fail-closed diagnostic");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testSquarePixelFitting(expectations);
    testPixelAspectFitting(expectations);
    testDegenerateAvailableRect(expectations);
    testViewerRendersQualifiedFrameAndReportsColorState(expectations);
    testViewerStatusSurfaceReflectsFailClosedColorState(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
