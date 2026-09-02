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
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPoint>
#include <QRectF>
#include <QWheelEvent>

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
        viewer.statusBarColorChipTextForTest() == QStringLiteral("Reference (unqualified)"),
        "the status bar's own color chip text (decision 3) agrees with the accessible description");

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
        viewer.statusBarColorChipTextForTest() == QStringLiteral("Qualified · Bloom Neutral"),
        "the status bar's own color chip text (decision 3) agrees with the accessible description");

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
    expectations.expect(viewer.statusBarColorChipTextForTest() == controller.state().message,
                        "the status bar's Error chip shows the fail-closed diagnostic verbatim "
                        "(decision 3: \"Error text on fail-closed\")");

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

// The status bar's center readout reuses the timeline's own exact display shape ("Frame N ·
// S.mmms")
// -- including an honest truncated (not rounded) subframe display, never a binary64 rounding of a
// non-terminating decimal (docs/architecture/animation-and-time.md).
void testStatusBarReadoutMatchesExactSessionTimeIncludingSubframe(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(makeTestProject("Readout Subframe Test"));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the fixture's initial frame becomes ready");

    expectations.expect(fixture.viewer.statusBarReadoutTextForTest() ==
                            QStringLiteral("Frame 0 · 0.000s"),
                        "the readout starts at frame 0, exact zero seconds");

    // 1/3 s has no terminating decimal expansion: truncated to 3 places this is EXACTLY "0.333s",
    // never "0.333...4" from a rounding of the binary64 approximation of 1/3.
    const auto subframeTime = core::RationalTime::create(1, 3);
    expectations.expect(subframeTime.has_value(), "1/3 second is a valid RationalTime");
    if (subframeTime.has_value()) {
        expectations.expect(fixture.session.setCurrentTime(*subframeTime),
                            "the session accepts an exact subframe time");
        expectations.expect(
            fixture.viewer.statusBarReadoutTextForTest().contains(QStringLiteral("0.333s")),
            "the readout truncates 1/3 second to exactly 0.333s (never a rounded 0.334s)");
    }

    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "the readout fixture reaches asynchronous scheduler quiescence");
}

// Space-hold + left-drag pans the view (decision 2) without ever touching CompositionSession's
// position-interaction gesture (positionInteractionActive() stays false throughout) -- pan is
// pure Viewer-local state.
void testSpaceHoldLeftDragPans(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(makeTestProject("Space Pan Test"));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the fixture's initial frame becomes ready");
    expectations.expect(fixture.viewer.viewTransformForTest().fitToWindow,
                        "the viewer starts in Fit mode with no pan");

    QKeyEvent spaceDown(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QCoreApplication::sendEvent(&fixture.viewer, &spaceDown);

    const QPointF pressPoint(150.0, 120.0);
    const QPointF movePoint(190.0, 96.0);
    QMouseEvent press(QEvent::MouseButtonPress, pressPoint, pressPoint, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&fixture.viewer, &press);
    QMouseEvent move(QEvent::MouseMove, movePoint, movePoint, Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QCoreApplication::sendEvent(&fixture.viewer, &move);

    expectations.expect(!fixture.session.positionInteractionActive(),
                        "a space-drag pan never begins a CompositionSession position interaction");
    expectations.expect(!fixture.viewer.viewTransformForTest().fitToWindow,
                        "panning materializes the transform out of Fit mode");
    const QPointF pan = fixture.viewer.viewTransformForTest().pan;
    expectations.expect(near(pan.x(), movePoint.x() - pressPoint.x()) &&
                            near(pan.y(), movePoint.y() - pressPoint.y()),
                        "pan tracks the TOTAL screen displacement from the press point");

    QMouseEvent release(QEvent::MouseButtonRelease, movePoint, movePoint, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&fixture.viewer, &release);
    QKeyEvent spaceUp(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QCoreApplication::sendEvent(&fixture.viewer, &spaceUp);
    expectations.expect(near(fixture.viewer.viewTransformForTest().pan.x(), pan.x()) &&
                            near(fixture.viewer.viewTransformForTest().pan.y(), pan.y()),
                        "releasing ends the pan gesture without resetting the accumulated pan");

    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "the space-pan fixture reaches asynchronous scheduler quiescence");
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

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
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
    testStatusBarReadoutMatchesExactSessionTimeIncludingSubframe(expectations);
    testSpaceHoldLeftDragPans(expectations);
    testEmptyStateInvitationTextPresentWithoutComposition(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
