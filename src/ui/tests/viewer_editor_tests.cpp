#include "editor_chrome_test_support.hpp"
#include <array>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/commands/asset_operations.hpp>
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
#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <bloom/ui/window_status_bar.hpp>

#include <QApplication>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPoint>
#include <QRectF>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWheelEvent>

#include <algorithm>
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

// --- Task U3 (issue #119), decision 2: ViewTransform math -------------------------------------

void testViewTransformFitMatchesFitDisplayRect(Expectations& expectations) {
    using namespace bloom;
    const QRectF available(0.0, 0.0, 640.0, 360.0);
    const auto square = core::PixelAspectRatio::square();
    const ui::ViewTransform fit{}; // default: fitToWindow == true
    const QRectF viaTransform =
        ui::viewTransformedDisplayRect(available, extent(1920, 1080), square, fit);
    const QRectF viaFit = ui::fitDisplayRect(available, extent(1920, 1080), square);
    expectations.expect(near(viaTransform.left(), viaFit.left()) &&
                            near(viaTransform.top(), viaFit.top()) &&
                            near(viaTransform.width(), viaFit.width()) &&
                            near(viaTransform.height(), viaFit.height()),
                        "a default (Fit) ViewTransform reproduces fitDisplayRect() exactly");
}

void testViewTransformActualSizeMatchesActualPixelRect(Expectations& expectations) {
    using namespace bloom;
    const QRectF available(0.0, 0.0, 640.0, 360.0);
    const auto square = core::PixelAspectRatio::square();
    const ui::ViewTransform actualSize{.fitToWindow = false, .zoom = 1.0, .pan = {0.0, 0.0}};
    const QRectF viaTransform =
        ui::viewTransformedDisplayRect(available, extent(200, 100), square, actualSize);
    const QRectF viaActual = ui::actualPixelRect(available, extent(200, 100), square);
    expectations.expect(near(viaTransform.width(), 200.0) && near(viaTransform.height(), 100.0),
                        "100% (zoom == 1.0, not Fit) shows content at its own pixel size");
    expectations.expect(
        near(viaTransform.left(), viaActual.left()) && near(viaTransform.top(), viaActual.top()),
        "100% reproduces actualPixelRect() exactly, centered like fitDisplayRect()");
}

void testViewTransformZoomAndPanCompose(Expectations& expectations) {
    using namespace bloom;
    const QRectF available(0.0, 0.0, 640.0, 360.0);
    const auto square = core::PixelAspectRatio::square();
    const ui::ViewTransform doubled{.fitToWindow = false, .zoom = 2.0, .pan = {30.0, -15.0}};
    const QRectF rect =
        ui::viewTransformedDisplayRect(available, extent(200, 100), square, doubled);
    expectations.expect(near(rect.width(), 400.0) && near(rect.height(), 200.0),
                        "zoom scales actualPixelRect() by the transform's own factor");
    const QRectF actual = ui::actualPixelRect(available, extent(200, 100), square);
    const QPointF expectedTopLeft(available.center().x() - rect.width() / 2.0 + 30.0,
                                  available.center().y() - rect.height() / 2.0 - 15.0);
    Q_UNUSED(actual)
    expectations.expect(near(rect.left(), expectedTopLeft.x()) &&
                            near(rect.top(), expectedTopLeft.y()),
                        "pan translates the zoomed rectangle in screen pixels, on top of zoom");
}

void testZoomAboutCursorInvariantHoldsAtNonIdentityZoom(Expectations& expectations) {
    using namespace bloom;
    const QRectF available(0.0, 0.0, 640.0, 360.0);
    const auto square = core::PixelAspectRatio::square();
    // Start from a non-trivial, already-panned/zoomed transform -- not just the identity -- so this
    // pins the invariant at exactly the zoom != 1, pan != 0 condition the task calls out.
    const ui::ViewTransform start{.fitToWindow = false, .zoom = 2.0, .pan = {12.0, -8.0}};
    const QRectF before =
        ui::viewTransformedDisplayRect(available, extent(200, 100), square, start);
    const QPointF cursor(410.0, 190.0); // an arbitrary point, not the rect's own center
    const double fractionXBefore = (cursor.x() - before.left()) / before.width();
    const double fractionYBefore = (cursor.y() - before.top()) / before.height();

    const ui::ViewTransform stepped =
        ui::zoomAboutPoint(start, available, extent(200, 100), square, cursor, 1.25);
    expectations.expect(!stepped.fitToWindow, "a zoom step always lands in Custom mode");
    const QRectF after =
        ui::viewTransformedDisplayRect(available, extent(200, 100), square, stepped);
    expectations.expect(near(after.width(), before.width() * 1.25) &&
                            near(after.height(), before.height() * 1.25),
                        "the zoom step scales by exactly the requested factor");
    const double fractionXAfter = (cursor.x() - after.left()) / after.width();
    const double fractionYAfter = (cursor.y() - after.top()) / after.height();
    expectations.expect(std::abs(fractionXAfter - fractionXBefore) <= 0.0005 &&
                            std::abs(fractionYAfter - fractionYBefore) <= 0.0005,
                        "the document point under the cursor is fixed across the zoom step "
                        "(zoom-about-cursor invariant) at non-identity zoom and non-zero pan");

    // Zooming back out by the inverse factor about the SAME cursor returns to (very nearly) the
    // original rectangle -- a second, stronger pin on the same invariant.
    const ui::ViewTransform back =
        ui::zoomAboutPoint(stepped, available, extent(200, 100), square, cursor, 1.0 / 1.25);
    const QRectF restored =
        ui::viewTransformedDisplayRect(available, extent(200, 100), square, back);
    expectations.expect(std::abs(restored.left() - before.left()) <= 0.01 &&
                            std::abs(restored.top() - before.top()) <= 0.01 &&
                            std::abs(restored.width() - before.width()) <= 0.01,
                        "zooming in then back out by the inverse factor about the same cursor "
                        "restores the original rectangle");
}

void testZoomClampsToTheDocumentedRange(Expectations& expectations) {
    using namespace bloom;
    const QRectF available(0.0, 0.0, 640.0, 360.0);
    const auto square = core::PixelAspectRatio::square();
    const ui::ViewTransform start{.fitToWindow = false, .zoom = 1.0, .pan = {0.0, 0.0}};
    const ui::ViewTransform zoomedOut =
        ui::zoomAboutPoint(start, available, extent(200, 100), square, available.center(), 0.0001);
    expectations.expect(near(zoomedOut.zoom, ui::ViewTransform::kMinZoom),
                        "zooming out clamps at 1/16, never below");
    const ui::ViewTransform zoomedIn =
        ui::zoomAboutPoint(start, available, extent(200, 100), square, available.center(), 10000.0);
    expectations.expect(near(zoomedIn.zoom, ui::ViewTransform::kMaxZoom),
                        "zooming in clamps at 16, never above");
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

// Renders the qualified frame's buffer (offscreen smoke) and confirms the status bar's color-state
// chip (task U3, decision 3 -- the relocated, contract-preserved home of the old top-row label)
// reads "Qualified · Bloom Neutral" once the qualified processor is ready -- never a silent
// relabel of the earlier reference-labeled frame. ADAPTED from the pre-U3 top-row-label test: the
// wording and the surface (status bar chip, not a painted canvas corner) both changed; the
// underlying color-state contract (accessibleDescription still carries it too -- see
// updatePreviewAccessibility()) did not.
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
    controller.setResolutionPolicy(runtime::PreviewResolutionPolicy::Half);

    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "an initial reference-labeled frame becomes ready");
    const QImage beforeReadiness = viewer.grab().toImage();
    expectations.expect(!beforeReadiness.isNull(),
                        "the viewer renders offscreen before the qualified processor is ready");
    expectations.expect(
        viewer.accessibleDescription().contains(QStringLiteral("Reference (unqualified)")),
        "the status surface reports the reference (unqualified) color state before "
        "readiness");
    expectations.expect(
        ui::previewColorState(controller.state()).text == QStringLiteral("Reference (unqualified)"),
        "the window status bar's own color chip text agrees with the accessible description");

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
        viewer.accessibleDescription().contains(QStringLiteral("Qualified · Bloom Neutral")),
        "the status surface reports the qualified color state once the frame is qualified");
    expectations.expect(
        ui::previewColorState(controller.state()).text ==
            QStringLiteral("Qualified · Bloom Neutral"),
        "the window status bar's own color chip text agrees with the accessible description");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// Fail-closed: the status surface shows the color state (a diagnostic message, plus a chip that
// never claims "Qualified") when qualification has failed, while the viewer keeps rendering its
// last-good pixels. ADAPTED from the pre-U3 top-row-label test: same underlying assertions, now
// checked against the relocated status bar chip as well as accessibleDescription.
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
        !viewer.accessibleDescription().contains(QStringLiteral("Qualified · Bloom Neutral")),
        "the status surface never claims a qualified color state once qualification has failed");
    expectations.expect(
        viewer.accessibleDescription().contains(controller.state().message),
        "the status surface's message reflects the controller's own fail-closed diagnostic");
    expectations.expect(ui::previewColorState(controller.state()).text ==
                            controller.state().message,
                        "the status bar's Error chip shows the fail-closed diagnostic verbatim "
                        "(\"Error text on fail-closed\")");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// --- Task U3 (issue #119): status bar + empty state + pan --------------------------------------

// Mirrors direct_manipulation_tests.cpp's PipelineFixture/GestureFixture split (same idiom, this
// file's own preexisting fixtures never needed the full pipeline before U3 added zoom/pan/status
// bar state that DOES need a running preview to exercise).
struct PipelineFixture final {
    bloom::runtime::NodeDefinitionRegistry definitions;
    bloom::runtime::SnapshotCompiler compiler;
    bloom::runtime::CpuCompositionEvaluator evaluator;
    bloom::runtime::CpuReferenceDisplayPreparer displayPreparer;
    bloom::runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    bloom::ui::PreviewPreparationFunction pipeline;

    PipelineFixture() : compiler(definitions) {
        if (!bloom::runtime::registerBuiltInNodeDefinitions(definitions)) {
            std::abort();
        }
        definitions.freeze();
        pipeline = bloom::ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                             qualifiedProcessorProvider);
    }
};

// A full offscreen ViewerEditor fixture with no layer/selection involved -- these tests exercise
// zoom/pan/status-bar state, never CompositionSession's position-interaction gesture (that is
// direct_manipulation_tests.cpp's GestureFixture).
struct ViewerFixture final {
    bloom::document::Document document;
    bloom::commands::CommandStack commands;
    bloom::ui::CompositionSession session;
    bloom::runtime::TaskScheduler scheduler;
    bloom::ui::TaskUiBridge bridge;
    PipelineFixture pipeline;
    bloom::ui::CompositionPreviewController controller;
    bloom::ui::ViewerEditor viewer;

    explicit ViewerFixture(bloom::document::NewProject newProject)
        : document(std::move(newProject.project)), commands(document),
          session(document, commands, newProject.initialCompositionId),
          scheduler(testSchedulerConfig()), bridge(scheduler, nullptr, 1ms),
          controller(session, scheduler, bridge, pipeline.pipeline), viewer(session, controller) {
        viewer.resize(400, 300);
    }
};

void testToolColumnUsesViewGestures(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(makeTestProject("Tool gestures"));
    fixture.viewer.show();
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "tool fixture ready");
    auto* zoom = fixture.viewer.findChild<QToolButton*>("viewerZoomTool");
    auto* hand = fixture.viewer.findChild<QToolButton*>("viewerHandTool");
    auto* select = fixture.viewer.findChild<QToolButton*>("viewerSelectTool");
    const auto before = fixture.commands.size();
    zoom->click();
    const auto center = fixture.viewer.canvasRectForTest().center();
    QMouseEvent zoomPress(QEvent::MouseButtonPress, center, center, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
    QCoreApplication::sendEvent(&fixture.viewer, &zoomPress);
    expectations.expect(!fixture.viewer.viewTransformForTest().fitToWindow && zoom->isChecked() &&
                            !select->isChecked(),
                        "zoom tool uses pointer zoom and exclusive selection");
    hand->click();
    const auto panBefore = fixture.viewer.viewTransformForTest().pan;
    QMouseEvent press(QEvent::MouseButtonPress, center, center, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&fixture.viewer, &press);
    const QPointF delta(14, 9);
    QMouseEvent move(QEvent::MouseMove, center + delta, center + delta, Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&fixture.viewer, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, center + delta, center + delta, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&fixture.viewer, &release);
    expectations.expect(fixture.viewer.viewTransformForTest().pan == panBefore + delta,
                        "hand tool maps left-button dragging to pan");
    expectations.expect(fixture.commands.size() == before,
                        "view tools do not mutate project truth");
    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "tool fixture shuts down");
}

// The zoom dropdown (decision 3) drives ViewerEditor's ViewTransform in both directions: choosing
// a preset sets the transform, and the transform's own state is reflected back (Fit's default,
// then a preset, then a wheel-derived custom value that lands on the dropdown's trailing item).
void testStatusBarZoomDropdownDrivesViewTransform(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(makeTestProject("Zoom Dropdown Test"));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the fixture's initial frame becomes ready");

    expectations.expect(fixture.viewer.viewTransformForTest().fitToWindow,
                        "the viewer starts in Fit mode");
    expectations.expect(fixture.viewer.zoomDropdownForTest()->currentText() ==
                            QStringLiteral("Fit"),
                        "the zoom dropdown starts on \"Fit\"");

    fixture.viewer.zoomDropdownForTest()->setCurrentIndex(3); // "100%"
    expectations.expect(!fixture.viewer.viewTransformForTest().fitToWindow &&
                            near(fixture.viewer.viewTransformForTest().zoom, 1.0),
                        "choosing \"100%\" in the dropdown sets a Custom transform at zoom 1.0");

    fixture.viewer.zoomDropdownForTest()->setCurrentIndex(4); // "200%"
    expectations.expect(near(fixture.viewer.viewTransformForTest().zoom, 2.0),
                        "choosing \"200%\" sets zoom to exactly 2.0");

    // A wheel step lands off the fixed ladder; the dropdown grows exactly one trailing item that
    // reflects it and stays selected there.
    QWheelEvent wheelEvent(QPointF(50.0, 50.0), QPointF(50.0, 50.0), QPoint(), QPoint(0, 120),
                           Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&fixture.viewer, &wheelEvent);
    expectations.expect(!near(fixture.viewer.viewTransformForTest().zoom, 2.0) &&
                            !near(fixture.viewer.viewTransformForTest().zoom, 1.0),
                        "a wheel step over the canvas changes the zoom off the fixed ladder");
    expectations.expect(fixture.viewer.zoomDropdownForTest()->count() >= 7,
                        "an off-ladder zoom grows exactly one trailing dropdown item");
    const int customIndex = fixture.viewer.zoomDropdownForTest()->count() - 1;
    expectations.expect(fixture.viewer.zoomDropdownForTest()->currentIndex() == customIndex,
                        "the dropdown selects the trailing custom-value item");

    fixture.viewer.zoomDropdownForTest()->setCurrentIndex(0); // "Fit"
    expectations.expect(fixture.viewer.viewTransformForTest().fitToWindow,
                        "choosing \"Fit\" restores fitToWindow");

    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "the zoom-dropdown fixture reaches asynchronous scheduler quiescence");
}

// FORMAL AMENDMENT 1 (task C1, after the first report): takeFooterWidget() hands back a real
// widget hosting the SAME status bar -- zoom dropdown included -- and, per the amendment's own
// contract, the color-state chip keeps rendering inside it. Pinned by rendering the returned
// widget offscreen and confirming it is not just a flat Surface-colored strip -- real chip/text
// content is painted into it -- the same "sample the rendered pixels" technique
// kinetik_baseline_tests.cpp's QComboBox field/border test already uses elsewhere in this suite.
// A tolerant "found a non-Surface pixel" check rather than an exact color match: the chip fills
// itself with the token color at partial opacity (paintChip()'s own withOpacity(color, 0.855)),
// composited over the Surface base the bar already painted, so the resulting pixel is a blend,
// never the raw token value.
//
// firstCall is deliberately never deleted: fixture.viewer (still alive) keeps a reference to it
// via statusBarFooter_ for its own session/preview-state signal handlers, exactly as a real
// EditorArea would keep the footer alive for the whole lifetime of the editor that produced it --
// deleting it early, before the fixture's own controller/bridge/scheduler have shut down, would
// dangle that reference into a live signal path and crash the test.
void testTakeFooterWidgetExposesTheStatusBarWithItsColorStateChip(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(makeTestProject("Take Footer Widget Test"));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the fixture's initial frame becomes ready");

    auto* firstCall = bloom::ui::test::footer(fixture.viewer);
    expectations.expect(firstCall != nullptr, "takeFooterWidget() returns a real widget");
    expectations.expect(bloom::ui::test::footer(fixture.viewer) == firstCall,
                        "a second call returns nullptr -- this ViewerEditor already gave its "
                        "footer away");
    if (firstCall == nullptr) {
        return;
    }

    // The zoom dropdown really moved into the returned widget (FORMAL AMENDMENT 1's "expose its
    // existing bottom status bar" -- not a duplicate, the SAME control).
    expectations.expect(fixture.viewer.zoomDropdownForTest()->parentWidget() == firstCall,
                        "the zoom dropdown is reparented into the returned footer widget");
    expectations.expect(ui::previewColorState(fixture.controller.state()).text ==
                            QStringLiteral("Reference (unqualified)"),
                        "the color-state wording is unchanged now that the chip reports from the "
                        "window status bar instead of this footer");

    firstCall->resize(400, ui::kit::px(ui::kit::Size::Control));
    QCoreApplication::processEvents();
    const QImage image = firstCall->grab().toImage();
    expectations.expect(!image.isNull(), "the returned footer widget renders offscreen");
    if (!image.isNull()) {
        const QColor surface = ui::kit::color(ui::kit::Color::Surface);
        const auto channelDistance = [](const QColor& left, const QColor& right) {
            return std::abs(left.red() - right.red()) + std::abs(left.green() - right.green()) +
                   std::abs(left.blue() - right.blue());
        };
        bool sawPaintedContent = false;
        for (int y = 0; y < image.height() && !sawPaintedContent; ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (channelDistance(image.pixelColor(x, y), surface) > 40) {
                    sawPaintedContent = true;
                    break;
                }
            }
        }
        expectations.expect(sawPaintedContent,
                            "real chip/readout content -- not a flat Surface-colored strip -- is "
                            "painted inside the returned footer widget (FORMAL AMENDMENT 1's "
                            "contract)");
    }

    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "the take-footer-widget fixture reaches asynchronous scheduler quiescence");
}

// The status bar's center readout reuses the timeline's own exact display shape ("Frame N ·

// Task S5, item 3b: the footer's dropped-frame readout. It is EMPTY unless the preview controller
// is counting, so outside a playback run the footer claims nothing at all -- and while counting it
// reports the count honestly, including zero.
void testStatusBarDroppedFrameReadoutOnlyClaimsWhatItMeasures(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(makeTestProject("Dropped Frame Readout Test"));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the dropped-frame readout fixture reaches its first ready frame");

    expectations.expect(ui::droppedFrameText(fixture.controller).isEmpty(),
                        "the footer says nothing about dropped frames outside a playback run, "
                        "because nothing is measuring");

    fixture.controller.beginDroppedFrameCounting();
    expectations.expect(ui::droppedFrameText(fixture.controller) == QStringLiteral("0 dropped"),
                        "once a run is counting the footer reports the count even at zero -- "
                        "silence would read as 'not measured', which is a different statement");

    fixture.controller.endDroppedFrameCounting();
    expectations.expect(ui::droppedFrameText(fixture.controller).isEmpty(),
                        "and it falls silent again when the run ends");

    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
}

// S.mmms")
// -- including an honest truncated (not rounded) subframe display, never a binary64 rounding of a
// non-terminating decimal (docs/architecture/animation-and-time.md).
void testStatusBarReadoutMatchesExactSessionTimeIncludingSubframe(Expectations& expectations) {
    using namespace bloom;
    QSettings().setValue("timeline/time-format", QStringLiteral("frames"));
    ViewerFixture fixture(makeTestProject("Readout Subframe Test"));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the fixture's initial frame becomes ready");

    // ADAPTED for task VIEW-1: the footer's one combined string became two controls -- the
    // Resolution readout ("Auto · 1", part of the Resolution dropdown) and the click-to-edit
    // frame/time readout. Both claims below are the same claims, read off the control that now
    // makes each of them.
    expectations.expect(fixture.viewer.statusBarReadoutTextForTest() == QStringLiteral("Auto · 1"),
                        "the resolution readout names the effective Auto factor");
    expectations.expect(fixture.viewer.timeReadoutTextForTest() == QStringLiteral("0"),
                        "the readout starts at frame 0, exact zero seconds");

    // 1/3 s has no terminating decimal expansion: truncated to 3 places this is EXACTLY "0.333s",
    // never "0.333...4" from a rounding of the binary64 approximation of 1/3.
    const auto subframeTime = core::RationalTime::create(1, 3);
    expectations.expect(subframeTime.has_value(), "1/3 second is a valid RationalTime");
    if (subframeTime.has_value()) {
        expectations.expect(fixture.session.setCurrentTime(*subframeTime),
                            "the session accepts an exact subframe time");
        expectations.expect(fixture.viewer.timeReadoutTextForTest() == QStringLiteral("8"),
                            "the readout projects exact one-third second to frame 8");
    }

    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "the readout fixture reaches asynchronous scheduler quiescence");
}

// Middle-drag pans the view (decision 2) without ever touching CompositionSession's
// position-interaction gesture (positionInteractionActive() stays false throughout) -- pan is
// pure Viewer-local state.
void testMiddleDragPans(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(makeTestProject("Middle Pan Test"));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the fixture's initial frame becomes ready");
    expectations.expect(fixture.viewer.viewTransformForTest().fitToWindow,
                        "the viewer starts in Fit mode with no pan");

    const QPointF pressPoint(150.0, 120.0);
    const QPointF movePoint(190.0, 96.0);
    QMouseEvent press(QEvent::MouseButtonPress, pressPoint, pressPoint, Qt::MiddleButton,
                      Qt::MiddleButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&fixture.viewer, &press);
    QMouseEvent move(QEvent::MouseMove, movePoint, movePoint, Qt::NoButton, Qt::MiddleButton,
                     Qt::NoModifier);
    QCoreApplication::sendEvent(&fixture.viewer, &move);

    expectations.expect(!fixture.session.positionInteractionActive(),
                        "a middle-drag pan never begins a CompositionSession position interaction");
    expectations.expect(!fixture.viewer.viewTransformForTest().fitToWindow,
                        "panning materializes the transform out of Fit mode");
    const QPointF pan = fixture.viewer.viewTransformForTest().pan;
    expectations.expect(near(pan.x(), movePoint.x() - pressPoint.x()) &&
                            near(pan.y(), movePoint.y() - pressPoint.y()),
                        "pan tracks the TOTAL screen displacement from the press point");

    QMouseEvent release(QEvent::MouseButtonRelease, movePoint, movePoint, Qt::MiddleButton,
                        Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&fixture.viewer, &release);
    expectations.expect(near(fixture.viewer.viewTransformForTest().pan.x(), pan.x()) &&
                            near(fixture.viewer.viewTransformForTest().pan.y(), pan.y()),
                        "releasing ends the pan gesture without resetting the accumulated pan");

    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "the middle-pan fixture reaches asynchronous scheduler quiescence");
}

// Task S1, item 8: Ctrl+0 fits and Ctrl+1 is actual size, the same pair the node canvas answers to.
// F and Z are retired in both and are bound by nothing here.
void testCtrlZeroFitsAndCtrlOneIsActualSize(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(makeTestProject("Viewer Zoom Keys Test"));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the fixture's initial frame becomes ready");
    expectations.expect(fixture.viewer.viewTransformForTest().fitToWindow,
                        "the viewer starts in Fit mode");

    const auto sendKey = [&fixture](const int key, const Qt::KeyboardModifiers modifiers) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers);
        QCoreApplication::sendEvent(&fixture.viewer, &event);
    };

    sendKey(Qt::Key_1, Qt::ControlModifier);
    expectations.expect(!fixture.viewer.viewTransformForTest().fitToWindow &&
                            near(fixture.viewer.viewTransformForTest().zoom, 1.0),
                        "Ctrl+1 leaves Fit for exactly 100%");
    sendKey(Qt::Key_0, Qt::ControlModifier);
    expectations.expect(fixture.viewer.viewTransformForTest().fitToWindow,
                        "and Ctrl+0 returns to Fit");

    // The retired keys do nothing: two ways to do one thing is exactly what item 8 removed.
    sendKey(Qt::Key_Z, Qt::NoModifier);
    expectations.expect(fixture.viewer.viewTransformForTest().fitToWindow,
                        "Z no longer reaches actual size");
    sendKey(Qt::Key_1, Qt::ControlModifier);
    sendKey(Qt::Key_F, Qt::NoModifier);
    expectations.expect(!fixture.viewer.viewTransformForTest().fitToWindow,
                        "and F no longer reaches Fit");

    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "the zoom-key fixture reaches asynchronous scheduler quiescence");
}

// Honest empty state (decision 5): with no composition at all, the canvas shows a quiet,
// product-neutral invitation instead of any evaluation warning or banner.
void testEmptyStateInvitationTextPresentWithoutComposition(Expectations& expectations) {
    using namespace bloom;
    // A genuinely composition-LESS project (never addComposition()'d) -- CompositionSession's own
    // constructor only falls back to lowestCompositionId() when at least one composition exists
    // (composition_session.cpp), so an empty project is the only way to make session.composition()
    // legitimately return nullptr.
    document::Project emptyProject(document::ProjectId::fromRaw(1), "Empty Viewer Project");
    document::Document document(std::move(emptyProject));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, document::CompositionId::fromRaw(1));
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);

    runtime::NodeDefinitionRegistry definitions;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(definitions),
                        "empty-state fixture registers built-in node definitions");
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

    expectations.expect(session.composition() == nullptr,
                        "the fixture genuinely has no composition -- this is the empty state, not "
                        "merely an unready one");
    const QImage image = viewer.grab().toImage();
    expectations.expect(!image.isNull(), "the empty-state canvas still renders offscreen (smoke)");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

void testAutoFollowsFitResize(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(
        document::makeNewProject("Fit resolution", "Main", core::RationalTime::fromInteger(1)));
    fixture.viewer.show();
    expectations.expect(fixture.controller.resolutionDivisor() == 4,
                        "a small fitted 1080p viewer chooses Quarter");
    fixture.viewer.resize(800, 500);
    expectations.expect(fixture.controller.resolutionDivisor() == 2,
                        "enlarging the fitted viewer raises Auto to Half");
    if (!fixture.controller.state().desiredIdentity.has_value()) {
        std::abort();
    }
    const auto generation = fixture.controller.state().desiredIdentity->requestGeneration;
    fixture.viewer.resize(850, 500);
    expectations.expect(fixture.controller.state().desiredIdentity.has_value() &&
                            fixture.controller.state().desiredIdentity->requestGeneration ==
                                generation,
                        "a resize within Half does not request another frame");
    fixture.viewer.resize(1200, 800);
    expectations.expect(fixture.controller.resolutionDivisor() == 1,
                        "a large fitted viewer raises Auto to Full");
    reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
}

void testProxyPaintingAndAutoZoom(Expectations& expectations) {
    using namespace bloom;
    const auto format = document::CompositionFormat::create(160, 120);
    if (!format.has_value()) {
        std::abort();
    }
    ViewerFixture fixture(document::makeNewProject("Proxy display geometry", "Main",
                                                   core::RationalTime::fromInteger(1), *format));
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Blue"), core::Color4d{0.0, 0.0, 1.0, 1.0}),
        "the proxy painting fixture has opaque content");
    fixture.viewer.show();
    fixture.viewer.zoomDropdownForTest()->setCurrentIndex(3);
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "Full is ready at actual size");
    const auto fullImage = fixture.viewer.grab().toImage();
    const auto inside = fixture.viewer.canvasRectForTest().center().toPoint() + QPoint(13, 11);
    const auto outside = fixture.viewer.canvasRectForTest().topLeft().toPoint() + QPoint(2, 2);
    const auto interior = fullImage.pixelColor(inside);
    expectations.expect(interior.blue() > 100 && interior.red() < 50 &&
                            interior != fullImage.pixelColor(outside),
                        "the sample lies inside the full composition");
    fixture.controller.setResolutionPolicy(runtime::PreviewResolutionPolicy::Quarter);
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "fixed Quarter is ready");
    const auto proxyImage = fixture.viewer.grab().toImage();
    expectations.expect(proxyImage.pixelColor(inside) == interior,
                        "Quarter upscales to the same actual-size composition rectangle");
    const auto proxyView = fixture.controller.state().frame->displayBufferView();
    expectations.expect(proxyView.has_value() && proxyView->displayWindow.extent().width() == 40,
                        "the painted proxy really has quarter-width pixels");
    fixture.controller.setResolutionPolicy(runtime::PreviewResolutionPolicy::Auto);
    fixture.viewer.zoomDropdownForTest()->setCurrentIndex(1);
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "Auto at 25 percent is ready");
    expectations.expect(
        fixture.controller.resolutionDivisor() == 4 &&
            fixture.viewer.statusBarReadoutTextForTest().startsWith(QStringLiteral("Auto · ¼")),
        "the footer readout names the effective Auto factor");
    QWheelEvent wheel(QPointF(200.0, 150.0), QPointF(200.0, 150.0), QPoint(), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&fixture.viewer, &wheel);
    expectations.expect(
        fixture.controller.resolutionDivisor() == 2 &&
            fixture.viewer.statusBarReadoutTextForTest().startsWith(QStringLiteral("Auto · ½")),
        "wheel zoom past Quarter raises Auto to Half and updates the readout");
    if (!fixture.controller.state().desiredIdentity.has_value()) {
        std::abort();
    }
    const auto generation = fixture.controller.state().desiredIdentity->requestGeneration;
    QCoreApplication::sendEvent(&fixture.viewer, &wheel);
    expectations.expect(fixture.controller.state().desiredIdentity.has_value() &&
                            fixture.controller.state().desiredIdentity->requestGeneration ==
                                generation,
                        "another wheel step within Half leaves the request alone");
    fixture.viewer.zoomDropdownForTest()->setCurrentIndex(3);
    expectations.expect(fixture.controller.resolutionDivisor() == 1,
                        "actual size restores Auto Full");
    reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
}

void testResolutionDropdownPersistsAndMovesWithFooter(Expectations& expectations) {
    using namespace bloom;
    QSettings().remove("viewer/resolution");
    {
        ViewerFixture fixture(makeTestProject("Resolution preference"));
        auto* dropdown = fixture.viewer.findChild<ui::kit::KDropdown*>("viewerResolutionDropdown");
        expectations.expect(dropdown != nullptr, "the footer exposes a named Resolution dropdown");
        if (dropdown == nullptr) {
            reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
            return;
        }
        expectations.expect(dropdown->accessibleName() == QStringLiteral("Resolution") &&
                                dropdown->count() == 4 &&
                                dropdown->currentText() == QStringLiteral("Auto"),
                            "Resolution defaults to Auto and offers four policies");
        dropdown->setCurrentIndex(2);
        expectations.expect(fixture.controller.settings().resolutionPolicy ==
                                    runtime::PreviewResolutionPolicy::Half &&
                                QSettings().value("viewer/resolution").toString() ==
                                    QStringLiteral("Half"),
                            "choosing Half updates the controller and preference");
        auto* footer = bloom::ui::test::footer(fixture.viewer);
        footer->resize(800, ui::kit::px(ui::kit::Size::Control));
        (void)footer->grab();
        expectations.expect(dropdown->parentWidget() == footer &&
                                dropdown->geometry().left() >
                                    fixture.viewer.zoomDropdownForTest()->geometry().right(),
                            "Resolution stays beside Zoom in the detached footer");
        reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
    }
    {
        ViewerFixture restored(makeTestProject("Restored resolution"));
        auto* dropdown = restored.viewer.findChild<ui::kit::KDropdown*>("viewerResolutionDropdown");
        expectations.expect(dropdown != nullptr &&
                                dropdown->currentText() == QStringLiteral("Half") &&
                                restored.controller.resolutionDivisor() == 2,
                            "a new viewer restores Half");
        reachQuiescence(restored.controller, restored.bridge, restored.scheduler, expectations);
    }
    QSettings().setValue("viewer/resolution", QStringLiteral("invalid"));
    {
        ViewerFixture invalid(makeTestProject("Invalid resolution preference"));
        expectations.expect(invalid.controller.settings().resolutionPolicy ==
                                runtime::PreviewResolutionPolicy::Auto,
                            "an unrecognized saved preference falls back to Auto");
        reachQuiescence(invalid.controller, invalid.bridge, invalid.scheduler, expectations);
    }
    QSettings().remove("viewer/resolution");
}

// --- Task VIEW-1: the viewer footer ------------------------------------------------------------

// The footer's controls, in the order the task fixes them: Channel, Zoom, Resolution, Background,
// the transport, and the frame/time readout. Order is asserted through laid-out geometry rather
// than child order, because the artist reads positions, not construction sequence.
void testFooterControlsAreOrderedLeftToRight(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(makeTestProject("Footer Order Test"));
    auto* footer = bloom::ui::test::footer(fixture.viewer);
    expectations.expect(footer != nullptr, "the viewer offers a footer widget");
    if (footer == nullptr) {
        reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
        return;
    }
    footer->resize(1400, ui::kit::px(ui::kit::Size::Control));
    // grab() forces the pending resize (and so the footer's own layout pass) to run now, the same
    // way testResolutionDropdownPersistsAndMovesWithFooter already does for a detached footer.
    (void)footer->grab();
    QCoreApplication::processEvents();

    const char* const ordered[] = {
        "viewerChannelDropdown",  "timelineRamPreviewButton", "viewerStepToStartButton",
        "timelineStepBackButton", "playPauseButton",          "timelineStepForwardButton",
        "viewerStepToEndButton",  "timelineLoopIndicator",    "viewerTimeReadout",
        "viewerZoomDropdown",     "viewerResolutionDropdown"};
    int previousRight = -1;
    for (const char* name : ordered) {
        auto* control = footer->findChild<QWidget*>(QString::fromLatin1(name));
        expectations.expect(control != nullptr, std::string{name} + " lives in the viewer footer");
        if (control == nullptr) {
            continue;
        }
        expectations.expect(control->geometry().left() > previousRight,
                            std::string{name} + " sits to the right of the control before it");
        previousRight = control->geometry().right();
    }
    reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
}

// task U8, issue #131, fix 7 carried over from the timeline: every icon-only transport button is a
// Size::Control square, and its glyph is the Control icon role's own 20px box (task VIEW-1).
void testTransportButtonsAreSquareAndUseTheControlIconRole(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(makeTestProject("Transport Button Metrics Test"));
    const int box = ui::kit::px(ui::kit::iconSize(ui::kit::IconRole::Control));
    for (const char* name :
         {"viewerStepToStartButton", "timelineStepBackButton", "playPauseButton",
          "timelineStepForwardButton", "viewerStepToEndButton", "timelineLoopIndicator"}) {
        auto* button = fixture.viewer.findChild<QToolButton*>(QString::fromLatin1(name));
        expectations.expect(button != nullptr, std::string{name} + " is reachable by name");
        if (button == nullptr) {
            continue;
        }
        expectations.expect(button->width() == button->height() &&
                                button->width() == ui::kit::px(ui::kit::Size::Control),
                            std::string{name} + " is exactly Size::Control square");
        expectations.expect(button->iconSize() == QSize(box, box),
                            std::string{name} + " carries a Control-role glyph box");
    }
    reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
}

// MOVED from timeline_editor_style_tests.cpp with the transport (task VIEW-1). Same claims, same
// objectName, same QToolButton text()/isChecked() contract -- only the panel and the icon role
// changed.
void testPlayPauseButtonIconSwapsWithState(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(makeTestProject("Play Pause Icon Test"));
    auto* button = fixture.viewer.findChild<QToolButton*>("playPauseButton");
    expectations.expect(button != nullptr, "the play/pause button is reachable by name");
    if (button == nullptr) {
        reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
        return;
    }

    const auto playIcon = ui::kit::icon(ui::kit::IconId::Play, ui::kit::IconRole::Control);
    const auto pauseIcon = ui::kit::icon(ui::kit::IconId::Pause, ui::kit::IconRole::Control);
    const int box = ui::kit::px(ui::kit::iconSize(ui::kit::IconRole::Control));
    const auto size = QSize(box, box);
    expectations.expect(button->icon().pixmap(size).toImage() == playIcon.pixmap(size).toImage(),
                        "the button starts showing the Play glyph");

    button->click();
    expectations.expect(button->isChecked() && button->text() == QStringLiteral("Pause"),
                        "the existing text()/isChecked() contract still flips on click");
    expectations.expect(button->icon().pixmap(size).toImage() == pauseIcon.pixmap(size).toImage(),
                        "clicking swaps the icon to Pause alongside the text");

    button->click();
    expectations.expect(button->icon().pixmap(size).toImage() == playIcon.pixmap(size).toImage(),
                        "clicking again swaps the icon back to Play");
    reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
}

// REPLACES timeline_editor_style_tests.cpp's "the loop indicator is never a clickable control"
// non-goal guard. That guard was honest while there was no command to turn looping off; task
// VIEW-1 added PlaybackController::setLooping(), so the control is a real toggle and the honest
// assertion is that clicking it actually changes the transport -- and that the preference persists.
void testLoopToggleDrivesThePlaybackControllerAndPersists(Expectations& expectations) {
    using namespace bloom;
    QSettings().remove("playback/loop");
    {
        ViewerFixture fixture(makeTestProject("Loop Toggle Test"));
        auto* loop = fixture.viewer.findChild<QToolButton*>("timelineLoopIndicator");
        expectations.expect(loop != nullptr && loop->isCheckable(),
                            "the loop control is a checkable button under its original name");
        if (loop == nullptr) {
            reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
            return;
        }
        auto& playback = fixture.controller.playbackController();
        expectations.expect(playback.isLooping() && loop->isChecked(),
                            "looping is on by default -- the behavior every Bloom transport has "
                            "shipped with");
        loop->click();
        expectations.expect(!playback.isLooping() && !loop->isChecked(),
                            "clicking it turns looping off on the shared transport");
        expectations.expect(!QSettings().value("playback/loop").toBool(),
                            "and the choice is persisted under playback/loop");
        reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
    }
    {
        ViewerFixture restored(makeTestProject("Loop Restore Test"));
        expectations.expect(!restored.controller.playbackController().isLooping(),
                            "a new viewer restores the persisted looping preference");
        reachQuiescence(restored.controller, restored.bridge, restored.scheduler, expectations);
    }
    QSettings().remove("playback/loop");
}

// The channel dropdown offers exactly the six documented views and drives the viewer's own channel
// state. The remap itself is viewer-only: the delivered display buffer is never touched, which is
// asserted by comparing the controller's own frame bytes before and after the switch.
void testChannelDropdownRemapsOnlyThePresentedImage(Expectations& expectations) {
    using namespace bloom;
    const auto format = document::CompositionFormat::create(160, 120);
    if (!format.has_value()) {
        std::abort();
    }
    ViewerFixture fixture(document::makeNewProject("Channel view", "Main",
                                                   core::RationalTime::fromInteger(1), *format));
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Blue"), core::Color4d{0.0, 0.0, 1.0, 1.0}),
        "the channel fixture has opaque coloured content");
    fixture.viewer.show();
    fixture.viewer.zoomDropdownForTest()->setCurrentIndex(3); // 100%
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the channel fixture reaches a ready frame");

    auto* channel = fixture.viewer.findChild<ui::kit::KDropdown*>("viewerChannelDropdown");
    expectations.expect(channel != nullptr && channel->count() == 6 &&
                            channel->currentText() == QStringLiteral("RGBA"),
                        "the footer offers RGBA/RGB/R/G/B/Alpha and starts on RGBA");
    if (channel == nullptr) {
        reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
        return;
    }

    const auto bufferBefore = fixture.controller.state().frame->displayBufferView();
    const QImage rgba = fixture.viewer.grab().toImage();

    channel->setCurrentIndex(2); // "R"
    expectations.expect(fixture.viewer.channelForTest() == ui::ViewerChannel::Red,
                        "choosing R puts the viewer in the red-channel view");
    QCoreApplication::processEvents();
    const QImage red = fixture.viewer.grab().toImage();
    expectations.expect(red != rgba, "a single-channel view really changes the presented pixels");

    const auto bufferAfter = fixture.controller.state().frame->displayBufferView();
    expectations.expect(
        bufferBefore.has_value() && bufferAfter.has_value() &&
            bufferBefore->pixels.data() == bufferAfter->pixels.data(),
        "and it never touches the delivered display buffer -- the viewer is looking "
        "at the SAME bytes, which is why a channel view can never reach an export");

    channel->setCurrentIndex(0); // back to RGBA
    QCoreApplication::processEvents();
    expectations.expect(fixture.viewer.grab().toImage() == rgba,
                        "returning to RGBA restores exactly the original presentation");
    reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
}

// The background dropdown chooses what the canvas surround is, defaults to Solid, and persists the
// choice under "viewer/background".
void testBackgroundDropdownChoosesTheSurroundAndPersists(Expectations& expectations) {
    using namespace bloom;
    QSettings().remove("viewer/background");
    {
        ViewerFixture fixture(makeTestProject("Background Test"));
        fixture.viewer.show();
        expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                            "the background fixture reaches a ready frame");
        auto* background =
            fixture.viewer.findChild<ui::kit::KDropdown*>("viewerBackgroundDropdown");
        expectations.expect(background != nullptr && background->count() == 4 &&
                                background->currentText() == QStringLiteral("Solid") &&
                                fixture.viewer.backgroundForTest() == ui::ViewerBackground::Solid,
                            "the footer offers four backgrounds and defaults to Solid");
        if (background == nullptr) {
            reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
            return;
        }
        // A corner of the fit rectangle at 25% zoom: the composition rectangle is a few pixels at
        // the centre, so the corner is pure surround (the frame casts no shadow any more).
        fixture.viewer.zoomDropdownForTest()->setCurrentIndex(1); // 25%
        QCoreApplication::processEvents();
        const auto corner = [&fixture] {
            return fixture.viewer.grab().toImage().pixelColor(
                fixture.viewer.canvasRectForTest().topLeft().toPoint() + QPoint(2, 2));
        };
        expectations.expect(corner() == QColor(Qt::black), "Solid defaults to opaque black");
        commands::Transaction backgroundEdit("Background", fixture.session.snapshot().revision());
        backgroundEdit.emplace<commands::SetCompositionBackgroundColor>(
            fixture.session.compositionId(), core::Color4d{0.2, 0.4, 0.6, 1.0});
        expectations.expect(
            fixture.session.executeTransaction(std::move(backgroundEdit)).succeeded(),
            "authored background edit commits");
        QCoreApplication::processEvents();
        expectations.expect(corner() == QColor(51, 102, 153),
                            "Solid paints the authored composition colour");
        background->setCurrentIndex(2); // Black
        QCoreApplication::processEvents();
        expectations.expect(fixture.viewer.backgroundForTest() == ui::ViewerBackground::Black &&
                                corner() == QColor(Qt::black),
                            "Black is literal black, not a token that merely reads dark");
        background->setCurrentIndex(3); // White
        QCoreApplication::processEvents();
        expectations.expect(corner() == QColor(Qt::white), "and White is literal white");
        // The surround fills the WHOLE content area, edge to edge (owner, 2026-09-15): the first
        // pixel right of the tool column at the very top, and the last pixel above the footer at
        // the far right, are both surround, not the canvas token behind it.
        {
            const QImage image = fixture.viewer.grab().toImage();
            const QRect content = fixture.viewer.contentRectForTest().toRect();
            const QColor topLeft = image.pixelColor(content.topLeft() + QPoint(1, 1));
            const QColor bottomRight = image.pixelColor(content.bottomRight() - QPoint(1, 1));
            expectations.expect(topLeft == QColor(Qt::white) && bottomRight == QColor(Qt::white),
                                "the surround reaches every edge of the content area");
        }
        expectations.expect(QSettings().value("viewer/background").toString() ==
                                QStringLiteral("White"),
                            "the choice is persisted under viewer/background");
        reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
    }
    {
        ViewerFixture restored(makeTestProject("Background Restore Test"));
        expectations.expect(restored.viewer.backgroundForTest() == ui::ViewerBackground::White,
                            "a new viewer restores the persisted background");
        reachQuiescence(restored.controller, restored.bridge, restored.scheduler, expectations);
    }
    QSettings().setValue("viewer/background", QStringLiteral("nonsense"));
    {
        ViewerFixture invalid(makeTestProject("Background Fallback Test"));
        expectations.expect(invalid.viewer.backgroundForTest() == ui::ViewerBackground::Solid,
                            "an unrecognized saved background falls back to Solid");
        reachQuiescence(invalid.controller, invalid.bridge, invalid.scheduler, expectations);
    }
    QSettings().remove("viewer/background");
}

// The readout: click to type an exact frame number, and a context menu that switches the label
// between a frame index and non-drop timecode (the SAME "timeline/time-format" preference the
// timeline's ruler reads, so the two surfaces cannot disagree about the format).
void testTimeReadoutEditsFramesAndSwitchesFormat(Expectations& expectations) {
    using namespace bloom;
    QSettings().setValue("timeline/time-format", QStringLiteral("frames"));
    ViewerFixture fixture(makeTestProject("Readout Edit Test"));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the readout fixture reaches a ready frame");

    auto* readout = fixture.viewer.findChild<QWidget*>("viewerTimeReadout");
    auto* editor = fixture.viewer.findChild<QLineEdit*>("viewerTimeReadoutEditor");
    expectations.expect(readout != nullptr && editor != nullptr,
                        "the readout and its inline editor are both reachable by name");
    if (readout == nullptr || editor == nullptr) {
        reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
        return;
    }
    expectations.expect(fixture.viewer.timeReadoutTextForTest() == QStringLiteral("0"),
                        "the readout starts on the frame-index format");

    // A click opens the editor seeded with the current frame; typing a number and pressing Return
    // moves session time to that frame's EXACT mapped time.
    const QPointF centre(readout->width() / 2.0, readout->height() / 2.0);
    QMouseEvent press(QEvent::MouseButtonPress, centre, centre, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(readout, &press);
    expectations.expect(editor->isVisible() || editor->text() == QStringLiteral("0"),
                        "clicking the readout opens an editor seeded with the current frame");
    editor->setText(QStringLiteral("5"));
    QMetaObject::invokeMethod(editor, "returnPressed");
    QCoreApplication::processEvents();
    const auto* const composition = fixture.session.composition();
    const auto expected =
        ui::frameTimeForIndex(composition->format().frameRate(), composition->duration(), 5);
    expectations.expect(expected.has_value() && fixture.session.currentTime() == *expected,
                        "committing a typed frame number lands on that frame's exact mapped time");

    // Out of range clamps rather than refusing, and an unparseable entry reverts in silence.
    QCoreApplication::sendEvent(readout, &press);
    editor->setText(QStringLiteral("999999"));
    QMetaObject::invokeMethod(editor, "returnPressed");
    QCoreApplication::processEvents();
    const auto maximum =
        ui::maxFrameIndex(composition->format().frameRate(), composition->duration());
    const auto lastTime = maximum.has_value()
                              ? ui::frameTimeForIndex(composition->format().frameRate(),
                                                      composition->duration(), *maximum)
                              : std::nullopt;
    expectations.expect(lastTime.has_value() && fixture.session.currentTime() == *lastTime,
                        "an out-of-range frame clamps to the last frame of the composition");

    // The context menu's own two format actions, triggered directly -- QMenu::exec() would block.
    // They are real children of the readout rather than menu-local, which is what makes them
    // reachable at all.
    auto* frames = fixture.viewer.findChild<QAction*>("viewerFramesAction");
    auto* timecode = fixture.viewer.findChild<QAction*>("viewerTimecodeAction");
    expectations.expect(frames != nullptr && timecode != nullptr && frames->isChecked() &&
                            !timecode->isChecked(),
                        "both format actions exist and Frames is the checked one");
    if (frames == nullptr || timecode == nullptr) {
        reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
        return;
    }
    (void)fixture.session.setCurrentTime(core::RationalTime::fromInteger(3));
    timecode->trigger();
    expectations.expect(fixture.viewer.timeReadoutTextForTest() == QStringLiteral("00:00:03:00"),
                        "Timecode shows the single non-drop readout");
    expectations.expect(QSettings().value("timeline/time-format").toString() ==
                            QStringLiteral("timecode"),
                        "the format is persisted under the SAME key the timeline ruler reads, so "
                        "the two surfaces can never disagree about it");
    QCoreApplication::sendEvent(readout, &press);
    const auto expectedTimecode = core::RationalTime::create(5, 2);
    editor->setText("00:00:02:12");
    QMetaObject::invokeMethod(editor, "returnPressed");
    QCoreApplication::processEvents();
    expectations.expect(expectedTimecode && fixture.session.currentTime() == *expectedTimecode,
                        "typed timecode maps to exact rational frame time");
    QCoreApplication::sendEvent(readout, &press);
    editor->setText("00:99:00:00");
    QMetaObject::invokeMethod(editor, "returnPressed");
    expectations.expect(expectedTimecode && fixture.session.currentTime() == *expectedTimecode,
                        "invalid timecode leaves time unchanged");
    (void)fixture.session.setCurrentTime(core::RationalTime::fromInteger(3));
    frames->trigger();
    expectations.expect(fixture.viewer.timeReadoutTextForTest() == QStringLiteral("72"),
                        "Frames restores the frame-index reading");

    reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
    QSettings().remove("timeline/time-format");
}

void testSelectedBoundsOverlayPixels(Expectations& expectations) {
    using namespace bloom;
    const auto format = document::CompositionFormat::create(160, 120);
    if (!format) {
        expectations.expect(false, "bounds format is valid");
        return;
    }
    ViewerFixture fixture(document::makeNewProject("Bounds overlay", "Main",
                                                   core::RationalTime::fromInteger(2), *format));
    auto footer = std::unique_ptr<QWidget>(bloom::ui::test::footer(fixture.viewer));
    footer->hide();
    expectations.expect(fixture.session.addSolidLayer("Bounds", {0, 0, 0, 1}),
                        "bounds fixture adds solid");
    const auto* selected = std::get_if<document::LayerId>(&fixture.session.selection().primary);
    if (!selected) {
        expectations.expect(false, "new layer selected");
        reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
        return;
    }
    const auto layer = *selected;
    const auto source = fixture.session.directSourceNodeForLayer(layer);
    if (!source) {
        expectations.expect(false, "bounds source is present");
        reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
        return;
    }
    const auto bindings = fixture.session.composition()->graph().findNode(*source)->parameters;
    for (const auto& binding : bindings) {
        if (binding.role == "width")
            (void)fixture.session.setParameterValue(binding.parameterId, 80.0, "Width");
        if (binding.role == "height")
            (void)fixture.session.setParameterValue(binding.parameterId, 40.0, "Height");
    }
    fixture.controller.setResolutionPolicy(runtime::PreviewResolutionPolicy::Full);
    fixture.viewer.show();
    fixture.viewer.zoomDropdownForTest()->setCurrentIndex(3);
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "bounds frame delivered");
    const auto bounds = fixture.controller.selectedLayerBounds();
    expectations.expect(bounds.size() == 1 && bounds[0].anchor == document::Vec2d{80, 60} &&
                            bounds[0].local == runtime::ContentBounds{0, 0, 80, 40},
                        "query returns delivered local bounds and composition anchor");
    expectations.expect(fixture.session.setCurrentTime(core::RationalTime::fromInteger(1)) &&
                            waitUntil([&] { return isReady(fixture.controller); }),
                        "a second bounds frame renders");
    expectations.expect(fixture.session.setCurrentTime({}) && fixture.controller.state().frame &&
                            !fixture.controller.state().frame->hasProcessFrame(),
                        "pixel probes use a cached display-only frame");
    const auto clearInteraction = [&] {
        fixture.viewer.clearFocus();
        for (auto* child : fixture.viewer.findChildren<QWidget*>()) {
            child->clearFocus();
            child->setAttribute(Qt::WA_UnderMouse, false);
        }
        fixture.viewer.setAttribute(Qt::WA_UnderMouse, false);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(&fixture.viewer, &leave);
        QCoreApplication::processEvents();
    };
    for (const int zoomIndex : {3, 4}) {
        fixture.viewer.zoomDropdownForTest()->setCurrentIndex(zoomIndex);
        const QPointF pressPoint(150, 120), movePoint(169, 107);
        QMouseEvent press(QEvent::MouseButtonPress, pressPoint, pressPoint, Qt::MiddleButton,
                          Qt::MiddleButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&fixture.viewer, &press);
        QMouseEvent move(QEvent::MouseMove, movePoint, movePoint, Qt::NoButton, Qt::MiddleButton,
                         Qt::NoModifier);
        QCoreApplication::sendEvent(&fixture.viewer, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, movePoint, movePoint, Qt::MiddleButton,
                            Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&fixture.viewer, &release);
        fixture.session.clearSelection();
        clearInteraction();
        const auto unselected = fixture.viewer.grab().toImage();
        fixture.session.selectLayer(layer);
        clearInteraction();
        const auto selectedImage = fixture.viewer.grab().toImage();
        const auto display = ui::viewTransformedDisplayRect(
            fixture.viewer.canvasRectForTest(), extent(160, 120), core::PixelAspectRatio::square(),
            fixture.viewer.viewTransformForTest());
        const auto screen = [&](double x, double y) {
            return QPoint(qRound(display.left() + x * display.width() / 160.0),
                          qRound(display.top() + y * display.height() / 120.0));
        };
        const auto brightness = [](QColor color) {
            return color.red() + color.green() + color.blue();
        };
        const auto anchor = screen(80, 60);
        const auto edge = screen(80, 40);
        expectations.expect(
            brightness(selectedImage.pixelColor(anchor)) >
                brightness(unselected.pixelColor(anchor)) + 50,
            "selected anchor is a filled bright six-pixel dot at the delivered point");
        int gain = 0;
        for (int dy = -1; dy <= 1; ++dy)
            gain = std::max(gain, brightness(selectedImage.pixelColor(edge + QPoint(0, dy))) -
                                      brightness(unselected.pixelColor(edge + QPoint(0, dy))));
        expectations.expect(
            gain > 50,
            "selected one-pixel outline follows the view zoom without object-name probes");
        expectations.expect(selectedImage.pixelColor(anchor + QPoint(6, 0)) ==
                                unselected.pixelColor(anchor + QPoint(6, 0)),
                            "anchor dot stays six screen pixels across at every zoom");
    }
    reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
}

void testViewerHeaderSelectorsMenusAndOverflow(Expectations& expectations) {
    using namespace bloom;
    QSettings().remove("viewer/overlay");
    ViewerFixture fixture(makeTestProject("Viewer Header Test"));

    auto* compositions =
        fixture.viewer.findChild<ui::kit::KDropdown*>(QStringLiteral("viewerCompositionSelector"));
    auto* objects =
        fixture.viewer.findChild<ui::kit::KDropdown*>(QStringLiteral("viewerObjectSelector"));
    auto* viewMenu =
        dynamic_cast<QMenu*>(fixture.viewer.findChild<QObject*>(QStringLiteral("viewerViewMenu")));
    auto* selectMenu = dynamic_cast<QMenu*>(
        fixture.viewer.findChild<QObject*>(QStringLiteral("viewerSelectMenu")));
    auto* fullscreen =
        fixture.viewer.findChild<QToolButton*>(QStringLiteral("viewerFullscreenButton"));
    expectations.expect(compositions != nullptr && objects != nullptr && viewMenu != nullptr &&
                            selectMenu != nullptr && fullscreen != nullptr,
                        "the Viewer header exposes both selectors, both menus, and fullscreen");
    if (compositions == nullptr || objects == nullptr || viewMenu == nullptr ||
        selectMenu == nullptr || fullscreen == nullptr) {
        fixture.controller.beginShutdown();
        fixture.bridge.beginShutdown();
        reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
        return;
    }
    expectations.expect(compositions->count() == 1 && compositions->itemText(0) == "Main" &&
                            objects->count() == 1 && objects->currentText() == "None",
                        "selectors start with the document composition and an honest None object");

    auto* safeAreas = fixture.viewer.findChild<QAction*>("viewerSafeAreasAction");
    auto* centreCross = fixture.viewer.findChild<QAction*>("viewerCentreCrossAction");
    auto* thirds = fixture.viewer.findChild<QAction*>("viewerThirdsAction");
    auto* rulers = fixture.viewer.findChild<QAction*>("viewerRulers");
    auto* pixelGrid = fixture.viewer.findChild<QAction*>("viewerPixelGridAction");
    expectations.expect(safeAreas != nullptr && centreCross != nullptr && thirds != nullptr &&
                            rulers != nullptr && pixelGrid != nullptr && !safeAreas->isChecked() &&
                            !centreCross->isChecked() && !thirds->isChecked() &&
                            !rulers->isChecked() && !pixelGrid->isChecked(),
                        "all viewer guides start disabled and are present in View");
    if (safeAreas != nullptr && centreCross != nullptr && thirds != nullptr && rulers != nullptr &&
        pixelGrid != nullptr) {
        expectations.expect(!safeAreas->shortcut().isEmpty() &&
                                !centreCross->shortcut().isEmpty() &&
                                !thirds->shortcut().isEmpty() && !rulers->shortcut().isEmpty() &&
                                !pixelGrid->shortcut().isEmpty(),
                            "guide actions expose their shortcuts for menu rendering");
    }

    expectations.expect(fixture.session.addSolidLayer(QStringLiteral("Layer One"), {0, 0, 0, 1}),
                        "the header fixture adds a selectable layer");
    expectations.expect(objects->count() == 2 && objects->currentText() == "Layer One",
                        "the object selector follows the current composition's layer display name");
    fixture.session.clearSelection();
    expectations.expect(objects->currentText() == "None", "clearing selection restores None");

    const auto copyId = ui::duplicateComposition(fixture.session, fixture.session.compositionId());
    expectations.expect(copyId.has_value() && compositions->count() == 2,
                        "the document composition selector refreshes after duplication");
    if (copyId.has_value()) {
        compositions->setCurrentIndex(1);
        expectations.expect(fixture.session.compositionId() == *copyId,
                            "choosing a composition selector item switches the shared session");
    }

    auto* header = fixture.viewer.findChild<QWidget*>("viewerHeaderMenuBar");
    auto* overflow = fixture.viewer.findChild<QToolButton*>("viewerHeaderOverflowButton");
    expectations.expect(header != nullptr && overflow != nullptr,
                        "the Viewer header has a dedicated overflow affordance");
    if (header != nullptr && overflow != nullptr) {
        header->show();
        header->resize(160, header->sizeHint().height());
        QCoreApplication::processEvents();
        expectations.expect(!overflow->isHidden(),
                            "narrow Viewer headers collapse menus into the ellipsis affordance");
    }

    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
}

// Regression: the header's collapse used to decide by showing every menu button and measuring,
// inside resizeEvent. Showing them changed the bar's minimum, the parent re-laid it out, the bar
// resized, and the collapse re-ran -- without bound, until the stack was gone (owner's desktop,
// 2026-09-14). Sweeping the width across the threshold several times would not return from the
// first crossing on the old code; on the new code the state must follow the width and the
// minimum size hint must not move with the state.
void testViewerHeaderCollapseIsStableAcrossResizes(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(makeTestProject("Viewer Header Collapse Test"));
    auto* header = fixture.viewer.findChild<QWidget*>("viewerHeaderMenuBar");
    auto* overflow = fixture.viewer.findChild<QToolButton*>("viewerHeaderOverflowButton");
    auto* viewButton = fixture.viewer.findChild<QToolButton*>("viewerViewMenuButton");
    expectations.expect(header != nullptr && overflow != nullptr,
                        "the collapse regression fixture finds the header and its overflow");
    if (header == nullptr || overflow == nullptr) {
        fixture.controller.beginShutdown();
        fixture.bridge.beginShutdown();
        reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
        return;
    }
    // A hidden widget receives no resize events, so the sweep drives the SHOWN viewer's width and
    // lets its layout hand the bar whatever width follows -- the path the desktop takes.
    fixture.viewer.show();
    fixture.viewer.resize(1400, 600);
    QCoreApplication::processEvents();
    header->show();
    const QSize wideMinimum = header->minimumSizeHint();
    bool stateFollowsWidth = true;
    bool minimumIsStable = true;
    bool sawCollapsed = false;
    bool sawExpanded = false;
    // The bar is resized directly (shown, so the resize event is delivered synchronously) and the
    // decision is checked twice per step: right after the resize, and again after the event loop
    // has let the parent layout answer -- which is exactly where the old code re-entered itself.
    const int height = header->height();
    const std::array<int, 5> widths{1200, 160, 1200, 120, 1200};
    auto consistent = [&]() {
        const bool collapsed = !overflow->isHidden();
        const bool expectCollapsed =
            header->property("collapseThreshold").toInt() > header->width();
        const bool buttonsAgree = viewButton == nullptr || viewButton->isHidden() == collapsed;
        return collapsed == expectCollapsed && buttonsAgree;
    };
    for (const int width : widths) {
        header->resize(width, height);
        if (!consistent()) {
            stateFollowsWidth = false;
        }
        (!overflow->isHidden() ? sawCollapsed : sawExpanded) = true;
        QCoreApplication::processEvents();
        if (!consistent()) {
            stateFollowsWidth = false;
        }
        if (header->minimumSizeHint() != wideMinimum) {
            minimumIsStable = false;
        }
    }
    expectations.expect(sawCollapsed && sawExpanded,
                        "the width sweep crosses the Viewer header's collapse threshold both ways");
    expectations.expect(stateFollowsWidth,
                        "the Viewer header collapses exactly when its menus do not fit");
    expectations.expect(minimumIsStable,
                        "the Viewer header's minimum size hint does not move with its collapse");
    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
}

void testViewerOverlayPixelsFollowTransformAndThreshold(Expectations& expectations) {
    using namespace bloom;
    const auto background = ui::kit::color(ui::kit::Color::Background);
    const auto brightness = [](const QColor color) {
        return color.red() + color.green() + color.blue();
    };
    ui::ViewerOverlayOptions options;
    options.safeAreas = true;
    options.centreCross = true;
    options.safeAreaSettings = {.action = 0.90, .title = 0.80};

    QImage first(420, 320, QImage::Format_ARGB32);
    first.fill(background);
    {
        QPainter painter(&first);
        const QRectF display(40.0, 30.0, 320.0, 240.0);
        ui::paintViewerOverlays(painter, QRectF(first.rect()), display, QSize(160, 120), 2.0,
                                options, {});
    }
    const QPoint firstCentre(200, 150);
    expectations.expect(brightness(first.pixelColor(firstCentre)) >
                            brightness(first.pixelColor(firstCentre + QPoint(11, 11))) + 30,
                        "centre-cross brightness is detectable relative to the transformed canvas");

    QImage second(700, 520, QImage::Format_ARGB32);
    second.fill(background);
    {
        QPainter painter(&second);
        const QRectF display(10.0, 5.0, 640.0, 480.0);
        ui::paintViewerOverlays(painter, QRectF(second.rect()), display, QSize(160, 120), 4.0,
                                options, {});
    }
    const QPoint secondCentre(330, 245);
    expectations.expect(brightness(second.pixelColor(secondCentre)) >
                            brightness(second.pixelColor(secondCentre + QPoint(17, 17))) + 30,
                        "the same guide remains readable after zoom and pan transformation");

    ui::ViewerOverlayOptions gridOnly;
    QImage below(180, 140, QImage::Format_ARGB32);
    below.fill(background);
    {
        QPainter painter(&below);
        ui::paintViewerOverlays(painter, QRectF(below.rect()), QRectF(0, 0, 160, 120),
                                QSize(160, 120), 3.0, gridOnly, {});
    }
    QImage atThreshold(180, 140, QImage::Format_ARGB32);
    atThreshold.fill(background);
    gridOnly.pixelGrid = true;
    {
        QPainter painter(&atThreshold);
        ui::paintViewerOverlays(painter, QRectF(atThreshold.rect()), QRectF(0, 0, 160, 120),
                                QSize(160, 120), 4.0, gridOnly, {});
    }
    expectations.expect(brightness(atThreshold.pixelColor(32, 60)) >
                            brightness(below.pixelColor(32, 60)) + 10,
                        "pixel grid is suppressed below 400% and appears at the threshold");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    QTemporaryDir settingsDirectory;
    QCoreApplication::setOrganizationName("BloomTests");
    QCoreApplication::setApplicationName("ViewerResolution");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    Expectations expectations;
    testToolColumnUsesViewGestures(expectations);
    testSelectedBoundsOverlayPixels(expectations);
    testResolutionDropdownPersistsAndMovesWithFooter(expectations);
    testAutoFollowsFitResize(expectations);
    testProxyPaintingAndAutoZoom(expectations);
    testSquarePixelFitting(expectations);
    testPixelAspectFitting(expectations);
    testDegenerateAvailableRect(expectations);
    testViewTransformFitMatchesFitDisplayRect(expectations);
    testViewTransformActualSizeMatchesActualPixelRect(expectations);
    testViewTransformZoomAndPanCompose(expectations);
    testZoomAboutCursorInvariantHoldsAtNonIdentityZoom(expectations);
    testZoomClampsToTheDocumentedRange(expectations);
    testViewerRendersQualifiedFrameAndReportsColorState(expectations);
    testViewerStatusSurfaceReflectsFailClosedColorState(expectations);
    testStatusBarZoomDropdownDrivesViewTransform(expectations);
    testTakeFooterWidgetExposesTheStatusBarWithItsColorStateChip(expectations);
    testStatusBarReadoutMatchesExactSessionTimeIncludingSubframe(expectations);
    testStatusBarDroppedFrameReadoutOnlyClaimsWhatItMeasures(expectations);
    testFooterControlsAreOrderedLeftToRight(expectations);
    testTransportButtonsAreSquareAndUseTheControlIconRole(expectations);
    testPlayPauseButtonIconSwapsWithState(expectations);
    testLoopToggleDrivesThePlaybackControllerAndPersists(expectations);
    testChannelDropdownRemapsOnlyThePresentedImage(expectations);
    testBackgroundDropdownChoosesTheSurroundAndPersists(expectations);
    testTimeReadoutEditsFramesAndSwitchesFormat(expectations);
    testViewerHeaderSelectorsMenusAndOverflow(expectations);
    testViewerHeaderCollapseIsStableAcrossResizes(expectations);
    testViewerOverlayPixelsFollowTransformAndThreshold(expectations);
    testMiddleDragPans(expectations);
    testCtrlZeroFitsAndCtrlOneIsActualSize(expectations);
    testEmptyStateInvitationTextPresentWithoutComposition(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
