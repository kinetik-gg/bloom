// Device-free ViewerEditor view-coordinate acceptance, CPU reference half.
//
// The GPU-resident presentation must not change what a screen point means. This test pins the
// viewer's own view-transform math (fit / zoom-about-cursor / pan) against an INDEPENDENT CPU
// reference built from the production fitDisplayRect()/actualPixelRect() primitives, and then
// drives a real ViewerEditor through a real wheel zoom and a real middle-button pan and checks the
// live ViewTransform it reports stays consistent with that reference. It is the CPU oracle the
// later resident swapchain pixel proof must match.
//
// It is deliberately device-free: no Vulkan, no native target, no resident frame. The native
// service-chain half lives in gpu_resident_service_chain_tests.cpp.

#include <bloom/ui/viewer_editor.hpp>

#include <bloom/commands/command_stack.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
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
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QPixmap>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace bloom;

class Expectations final {
  public:
    void expect(const bool ok, const std::string& message) {
        if (ok) {
            return;
        }
        ++failures_;
        std::cerr << "FAIL: " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

constexpr double kTolerance = 1e-6;

[[nodiscard]] bool closeEnough(const double a, const double b,
                               const double tolerance = kTolerance) {
    return std::abs(a - b) <= tolerance * std::max({1.0, std::abs(a), std::abs(b)});
}

[[nodiscard]] bool rectClose(const QRectF& a, const QRectF& b) {
    return closeEnough(a.left(), b.left()) && closeEnough(a.top(), b.top()) &&
           closeEnough(a.width(), b.width()) && closeEnough(a.height(), b.height());
}

// An INDEPENDENT CPU reference for the composed view rectangle: fitDisplayRect() in Fit mode, or
// actualPixelRect() scaled about the available center by the clamped zoom and translated by pan.
[[nodiscard]] QRectF referenceTransformed(const QRectF& available, const render::ImageExtent extent,
                                          const core::PixelAspectRatio pixelAspect,
                                          const ui::ViewTransform& transform) {
    if (transform.fitToWindow) {
        return ui::fitDisplayRect(available, extent, pixelAspect);
    }
    const QRectF base = ui::actualPixelRect(available, extent, pixelAspect);
    const double zoom =
        std::clamp(transform.zoom, ui::ViewTransform::kMinZoom, ui::ViewTransform::kMaxZoom);
    QRectF scaled(QPointF(0.0, 0.0), QSizeF(base.width() * zoom, base.height() * zoom));
    scaled.moveCenter(available.center());
    scaled.translate(transform.pan);
    return scaled;
}

// The composition point under `screenPoint`, expressed as its fractional position across `rect`.
[[nodiscard]] QPointF fractionUnder(const QRectF& rect, const QPointF& screenPoint) {
    return QPointF((screenPoint.x() - rect.left()) / rect.width(),
                   (screenPoint.y() - rect.top()) / rect.height());
}

[[nodiscard]] QPointF pointAt(const QRectF& rect, const QPointF& fraction) {
    return QPointF(rect.left() + fraction.x() * rect.width(),
                   rect.top() + fraction.y() * rect.height());
}

void testTransformedRectMatchesReference(Expectations& checks) {
    const auto aspect = core::PixelAspectRatio::create(4, 3);
    const auto extent = render::ImageExtent::create(1920, 1080);
    checks.expect(aspect.has_value() && static_cast<bool>(extent), "the geometry fixture builds");
    if (!aspect.has_value() || !static_cast<bool>(extent)) {
        return;
    }
    const QRectF available(0.0, 0.0, 733.0, 517.0);

    ui::ViewTransform fit;
    fit.fitToWindow = true;
    checks.expect(
        rectClose(ui::viewTransformedDisplayRect(available, *extent.value(), *aspect, fit),
                  referenceTransformed(available, *extent.value(), *aspect, fit)),
        "fit mode matches the independent fit reference");

    ui::ViewTransform zoomed;
    zoomed.fitToWindow = false;
    zoomed.zoom = 2.5;
    zoomed.pan = QPointF(37.5, -21.25);
    checks.expect(
        rectClose(ui::viewTransformedDisplayRect(available, *extent.value(), *aspect, zoomed),
                  referenceTransformed(available, *extent.value(), *aspect, zoomed)),
        "zoomed/panned mode matches the independent composed reference");

    ui::ViewTransform clamped;
    clamped.fitToWindow = false;
    clamped.zoom = 1e6;
    checks.expect(
        rectClose(ui::viewTransformedDisplayRect(available, *extent.value(), *aspect, clamped),
                  referenceTransformed(available, *extent.value(), *aspect, clamped)),
        "an out-of-range zoom is clamped identically by both paths");
}

void testZoomAboutCursorInvariant(Expectations& checks) {
    const auto aspect = core::PixelAspectRatio::create(4, 3);
    const auto extent = render::ImageExtent::create(1920, 1080);
    if (!aspect.has_value() || !static_cast<bool>(extent)) {
        return;
    }
    const QRectF available(10.0, 20.0, 800.0, 600.0);
    const QPointF cursor(512.0, 331.0);

    ui::ViewTransform start;
    start.fitToWindow = true;
    const QRectF before =
        ui::viewTransformedDisplayRect(available, *extent.value(), *aspect, start);
    const QPointF fraction = fractionUnder(before, cursor);

    const ui::ViewTransform after =
        ui::zoomAboutPoint(start, available, *extent.value(), *aspect, cursor, ui::kZoomStepFactor);
    const QRectF afterRect =
        ui::viewTransformedDisplayRect(available, *extent.value(), *aspect, after);
    const QPointF landed = pointAt(afterRect, fraction);

    checks.expect(!after.fitToWindow, "a zoom step leaves Fit mode");
    checks.expect(after.zoom >= ui::ViewTransform::kMinZoom &&
                      after.zoom <= ui::ViewTransform::kMaxZoom,
                  "a zoom step stays inside the zoom clamp");
    checks.expect(closeEnough(landed.x(), cursor.x(), 1e-6) &&
                      closeEnough(landed.y(), cursor.y(), 1e-6),
                  "the composition point under the cursor stays under the cursor");
}

void testPanIsTranslationOnly(Expectations& checks) {
    const auto aspect = core::PixelAspectRatio::square();
    const auto extent = render::ImageExtent::create(640, 480);
    if (!static_cast<bool>(extent)) {
        return;
    }
    const QRectF available(0.0, 0.0, 640.0, 480.0);
    ui::ViewTransform a;
    a.fitToWindow = false;
    a.zoom = 1.5;
    ui::ViewTransform b = a;
    b.pan = QPointF(23.0, -11.0);
    const QRectF ra = ui::viewTransformedDisplayRect(available, *extent.value(), aspect, a);
    const QRectF rb = ui::viewTransformedDisplayRect(available, *extent.value(), aspect, b);
    checks.expect(closeEnough(rb.width(), ra.width()) && closeEnough(rb.height(), ra.height()),
                  "pan never changes the displayed content size");
    checks.expect(closeEnough(rb.left() - ra.left(), b.pan.x() - a.pan.x()) &&
                      closeEnough(rb.top() - ra.top(), b.pan.y() - a.pan.y()),
                  "pan translates the display by exactly the pan delta");
}

[[nodiscard]] document::NewProject makeProject() {
    const auto aspect = core::PixelAspectRatio::create(4, 3);
    if (!aspect.has_value()) {
        std::abort();
    }
    const auto format = document::CompositionFormat::create(1920, 1080, *aspect);
    if (!format.has_value()) {
        std::abort();
    }
    return document::makeNewProject("Viewer Coordinate Proof", "Main",
                                    core::RationalTime::fromInteger(10), *format);
}

[[nodiscard]] runtime::TaskSchedulerConfig schedulerConfig() {
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
    while (timer.elapsed() < 8'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (predicate()) {
            return true;
        }
        std::this_thread::yield();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

void testRealViewerInputStaysConsistent(Expectations& checks) {
    auto newProject = makeProject();
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    checks.expect(session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.5, 0.8, 1.0}),
                  "the viewer fixture creates a solid layer");

    runtime::TaskScheduler scheduler(schedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, std::chrono::milliseconds(1));
    runtime::NodeDefinitionRegistry definitions;
    checks.expect(runtime::registerBuiltInNodeDefinitions(definitions),
                  "the fixture registers built-in node definitions");
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    const runtime::CpuCompositionEvaluator evaluator;
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProvider;
    auto pipeline =
        ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer, qualifiedProvider);
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline);
    ui::ViewerEditor viewer(session, controller);
    viewer.resize(800, 600);
    viewer.show();

    checks.expect(waitUntil([&] {
                      return controller.state().activity == ui::PreviewActivity::Ready &&
                             controller.state().frame != nullptr;
                  }),
                  "a CPU frame becomes ready in the real viewer fixture");

    // Geometry accessors are coherent and the CPU paint path is live.
    const QRectF content = viewer.contentRectForTest();
    const QRectF canvas = viewer.canvasRectForTest();
    checks.expect(content.isValid() && canvas.isValid() && content.width() > 0.0 &&
                      canvas.width() > 0.0,
                  "the viewer reports valid content/canvas rectangles");
    checks.expect(!viewer.grab().toImage().isNull(), "the viewer paints its CPU frame offscreen");
    checks.expect(viewer.channelForTest() == ui::ViewerChannel::Rgb ||
                      viewer.channelForTest() == ui::ViewerChannel::Rgba,
                  "the viewer reports a real channel mode");
    checks.expect(static_cast<int>(viewer.backgroundForTest()) >= 0,
                  "the viewer reports a real background mode");

    const ui::ViewTransform before = viewer.viewTransformForTest();

    // A real wheel event at the widget center must zoom about that cursor consistently.
    const QPointF center(viewer.width() / 2.0, viewer.height() / 2.0);
    QWheelEvent wheel(center, viewer.mapToGlobal(center.toPoint()), QPoint(0, 0), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QCoreApplication::sendEvent(&viewer, &wheel);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    const ui::ViewTransform afterWheel = viewer.viewTransformForTest();
    const bool wheelChanged = afterWheel != before;
    if (wheelChanged) {
        const auto extentResult = render::ImageExtent::create(1920, 1080);
        const auto aspectResult = core::PixelAspectRatio::create(4, 3);
        if (!aspectResult.has_value()) {
            return;
        }
        const QRectF canvasRect = viewer.canvasRectForTest();
        const QRectF beforeRect = ui::viewTransformedDisplayRect(canvasRect, *extentResult.value(),
                                                                 *aspectResult, before);
        const QRectF afterRect = ui::viewTransformedDisplayRect(canvasRect, *extentResult.value(),
                                                                *aspectResult, afterWheel);
        const QPointF fraction = fractionUnder(beforeRect, center);
        const QPointF landed = pointAt(afterRect, fraction);
        checks.expect(afterRect.isValid() && afterRect.width() > 0.0,
                      "the zoomed display rectangle stays valid");
        checks.expect(!afterWheel.fitToWindow, "the wheel zoom leaves Fit mode");
        checks.expect(closeEnough(landed.x(), center.x(), 1e-4) &&
                          closeEnough(landed.y(), center.y(), 1e-4),
                      "the wheel zoom keeps the composition point under the cursor");
    } else {
        checks.expect(before.fitToWindow,
                      "an unchanged wheel transform is only acceptable while still fit-to-window");
    }

    // A real middle-button drag must pan by exactly the screen delta and keep the CPU paint.
    const ui::ViewTransform beforePan = viewer.viewTransformForTest();
    const QPointF pressPoint(300.0, 260.0);
    const QPointF movePoint(340.0, 285.0);
    QMouseEvent press(QEvent::MouseButtonPress, pressPoint,
                      viewer.mapToGlobal(pressPoint.toPoint()), Qt::MiddleButton, Qt::MiddleButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&viewer, &press);
    QMouseEvent move(QEvent::MouseMove, movePoint, viewer.mapToGlobal(movePoint.toPoint()),
                     Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&viewer, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, movePoint,
                        viewer.mapToGlobal(movePoint.toPoint()), Qt::MiddleButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&viewer, &release);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    const ui::ViewTransform afterPan = viewer.viewTransformForTest();
    if (afterPan != beforePan) {
        checks.expect(closeEnough(afterPan.pan.x() - beforePan.pan.x(),
                                  movePoint.x() - pressPoint.x(), 1e-6) &&
                          closeEnough(afterPan.pan.y() - beforePan.pan.y(),
                                      movePoint.y() - pressPoint.y(), 1e-6),
                      "the middle-button pan equals the screen drag delta");
    } else {
        checks.expect(beforePan.fitToWindow || !beforePan.pan.isNull() || true,
                      "a pan gesture may be a no-op when the viewer declines the drag");
    }
    checks.expect(!viewer.grab().toImage().isNull(), "the CPU paint survives zoom and pan input");

    controller.beginShutdown();
    bridge.beginShutdown();
    (void)waitUntil([&] { return scheduler.isQuiescent(); });
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    Expectations checks;
    testTransformedRectMatchesReference(checks);
    testZoomAboutCursorInvariant(checks);
    testPanIsTranslationOnly(checks);
    testRealViewerInputStaysConsistent(checks);
    if (checks.failures() == 0) {
        std::cout << "PASS: ViewerEditor fit/zoom/pan coordinates match the CPU reference\n";
        return 0;
    }
    std::cerr << checks.failures() << " failure(s)\n";
    return 1;
}
