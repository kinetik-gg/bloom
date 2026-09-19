// Real GPU end-to-end vertical acceptance, viewer half.
//
// A REAL ViewerEditor is constructed with a REAL CompositionPreviewController whose
// PreviewPreparationSubmitter calls the ONE real GpuPreviewDisplayService (resident overload,
// Wayland presentation). The controller therefore receives the genuine GPU-resident
// PreparedPreviewFrame the service published, and the existing ViewerEditor resident branch
// presents it through the existing ViewerGpuPresenter. No fake frame is injected, no second
// device/context is created, and no app bootstrap is needed: the existing
// setGpuPresentationDependencies() seam wires the same service client.
//
// The test prints, for each phase, the mapped composition rectangle and known sample points with
// the CPU reference colour computed in-process from the SAME document snapshot. The optional
// external runner (proof/viewer_capture.sh) captures the actual Viewer window with grim and
// compares the presented pixels to those references. The in-process assertions cover the genuine
// resident frame, the owner present acknowledgement, and the live mapped geometry; the pixels are
// proven externally.

#include "gpu_service_chain_fixture.hpp"

#include <bloom/runtime/reference_display_preparation.hpp>

#include <bloom/document/new_project.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_gpu_scene_stage.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QPixmap>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace bloom::ui::verticalproof;
using namespace std::chrono_literals;

namespace {

using namespace bloom;

// One mapped interior sample: a widget-local point, its global screen position, and the reference
// packed RGBA8 colour at the composition pixel it lands on.
struct ViewerSample final {
    QPointF widgetLocal;
    QPointF windowLocal;
    render::Rgba8 expected{};
    bool opaque = false;
};

// The viewer's background is a compositing backdrop: the resident present composites the frame over
// it, so a transparent composition pixel shows the backdrop and an opaque one reads source-over it.
// The grim capture only ever sees opaque composited pixels.
[[nodiscard]] render::Rgba8 overOpaqueBlack(const render::Rgba8 rgba) {
    const auto scale = [](const std::uint8_t value, const std::uint8_t alpha) {
        return static_cast<std::uint8_t>((static_cast<int>(value) * static_cast<int>(alpha) + 127) /
                                         255);
    };
    return render::Rgba8{scale(rgba.red, rgba.alpha), scale(rgba.green, rgba.alpha),
                         scale(rgba.blue, rgba.alpha), 255};
}

// Byte-for-byte source-over of a straight-alpha sample onto an opaque backdrop, mirroring the
// premultiplied-over-opaque compositing the present shader and the CPU paint both perform.
[[nodiscard]] render::Rgba8 compositeOver(const render::Rgba8 backdrop, const render::Rgba8 rgba) {
    const auto multiply = [](const std::uint8_t value, const std::uint8_t alpha) {
        return static_cast<std::uint8_t>((static_cast<int>(value) * static_cast<int>(alpha) + 127) /
                                         255);
    };
    const auto add = [](const std::uint8_t a, const std::uint8_t b) {
        return static_cast<std::uint8_t>(std::min(255, static_cast<int>(a) + static_cast<int>(b)));
    };
    return render::Rgba8{
        add(multiply(rgba.red, rgba.alpha), multiply(backdrop.red, 255 - rgba.alpha)),
        add(multiply(rgba.green, rgba.alpha), multiply(backdrop.green, 255 - rgba.alpha)),
        add(multiply(rgba.blue, rgba.alpha), multiply(backdrop.blue, 255 - rgba.alpha)), 255};
}

// The CPU background colour a mode paints behind the composition (viewer_editor.cpp
// drawCanvasBackground above): Solid is the panel Canvas token, Black/White literal, and
// Checkerboard alternates Surface (base) and SurfaceRaised (raised) on the 22px kit ViewerChecker
// grid. The checker cell for a device-pixel coordinate is derived from the CPU semantics, NOT by
// reading the GPU request fields, so the two paths are compared independently.
[[nodiscard]] render::Rgba8 cpuBackgroundColor(const ui::ViewerBackground mode, const int deviceX,
                                               const int deviceY, const double dpr) {
    switch (mode) {
    case ui::ViewerBackground::Solid: {
        const QColor canvas = ui::kit::color(ui::kit::Color::Canvas);
        return render::Rgba8{static_cast<std::uint8_t>(canvas.red()),
                             static_cast<std::uint8_t>(canvas.green()),
                             static_cast<std::uint8_t>(canvas.blue()), 255};
    }
    case ui::ViewerBackground::Black:
        return render::Rgba8{0, 0, 0, 255};
    case ui::ViewerBackground::White:
        return render::Rgba8{255, 255, 255, 255};
    case ui::ViewerBackground::Checkerboard:
        break;
    }
    const auto tile =
        static_cast<int>(std::lround(ui::kit::px(ui::kit::Size::ViewerChecker) * dpr));
    const int span = std::max(1, tile);
    const int column = deviceX >= 0 ? deviceX / span : -((-deviceX + span - 1) / span);
    const int row = deviceY >= 0 ? deviceY / span : -((-deviceY + span - 1) / span);
    const bool raised = ((row + column) & 1) == 0;
    const QColor color = raised ? ui::kit::color(ui::kit::Color::SurfaceRaised)
                                : ui::kit::color(ui::kit::Color::Surface);
    return render::Rgba8{static_cast<std::uint8_t>(color.red()),
                         static_cast<std::uint8_t>(color.green()),
                         static_cast<std::uint8_t>(color.blue()), 255};
}

[[nodiscard]] render::Rgba8 channelReference(const render::Rgba8 rgba,
                                             const ui::ViewerChannel channel) {
    switch (channel) {
    case ui::ViewerChannel::Rgba:
        return rgba;
    case ui::ViewerChannel::Rgb:
        return render::Rgba8{rgba.red, rgba.green, rgba.blue, 255};
    case ui::ViewerChannel::Red:
        return render::Rgba8{rgba.red, rgba.red, rgba.red, 255};
    case ui::ViewerChannel::Green:
        return render::Rgba8{rgba.green, rgba.green, rgba.green, 255};
    case ui::ViewerChannel::Blue:
        return render::Rgba8{rgba.blue, rgba.blue, rgba.blue, 255};
    case ui::ViewerChannel::Alpha:
        return render::Rgba8{rgba.alpha, rgba.alpha, rgba.alpha, 255};
    }
    return rgba;
}

[[nodiscard]] std::string hexOf(const render::Rgba8 c) {
    char buffer[10];
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X", c.red, c.green, c.blue, c.alpha);
    return buffer;
}

// Builds the mapped interior samples for the viewer's CURRENT transform/channel against the CPU
// reference display buffer. The destination rectangle is recomputed exactly as the resident present
// request does, so a sample is only emitted when it genuinely lands inside the presented area.
[[nodiscard]] std::vector<ViewerSample> buildSamples(ui::ViewerEditor& viewer, QWidget& host,
                                                     const runtime::PreparedPreviewFrame& reference,
                                                     const ui::ViewerChannel channel) {
    std::vector<ViewerSample> samples;
    const auto view = reference.displayBufferView();
    if (!view.has_value()) {
        return samples;
    }
    const render::ImageWindow window = view->displayWindow;
    const auto width = static_cast<std::uint32_t>(window.extent().width());
    const auto height = static_cast<std::uint32_t>(window.extent().height());
    const auto extent = render::ImageExtent::create(width, height);
    if (!extent || width == 0U || height == 0U) {
        return samples;
    }
    const QRectF destination =
        ui::viewTransformedDisplayRect(viewer.canvasRectForTest(), *extent.value(),
                                       view->pixelAspect, viewer.viewTransformForTest());
    if (!destination.isValid() || destination.width() <= 0.0 || destination.height() <= 0.0) {
        return samples;
    }
    const std::array<QPointF, 4> fractions{QPointF(0.15, 0.15), QPointF(0.40, 0.15),
                                           QPointF(0.80, 0.80), QPointF(0.45, 0.30)};
    for (const QPointF fraction : fractions) {
        const QPointF local(destination.left() + fraction.x() * destination.width(),
                            destination.top() + fraction.y() * destination.height());
        const auto originX = static_cast<int>(window.originX());
        const auto originY = static_cast<int>(window.originY());
        const int compX =
            std::clamp(originX + static_cast<int>(fraction.x() * static_cast<double>(width)),
                       originX, originX + static_cast<int>(width) - 1);
        const int compY =
            std::clamp(originY + static_cast<int>(fraction.y() * static_cast<double>(height)),
                       originY, originY + static_cast<int>(height) - 1);
        const render::Rgba8 referencePixel =
            view->pixels[static_cast<std::size_t>(compY - originY) * width +
                         static_cast<std::size_t>(compX - originX)];
        ViewerSample sample;
        sample.widgetLocal = local;
        // Wayland does not expose a window's global position to the client, so the reference is
        // emitted in TOP-LEVEL WINDOW coordinates; the capture is that same window region.
        sample.windowLocal = QPointF(viewer.mapTo(&host, local.toPoint()));
        sample.expected = overOpaqueBlack(channelReference(referencePixel, channel));
        sample.opaque = referencePixel.alpha == 255;
        samples.push_back(sample);
    }
    return samples;
}

// RGBA background phases: build mapped samples whose expected colour is the channel-remapped CPU
// reference composited over the mode's own CPU background. At least one sample per mode lands on a
// transparent composition pixel, so a mode whose GPU surround disagreed with the CPU paint (the
// original Solid-as-authored-colour bug) would fail the external comparison. Checker geometry is
// derived from the CPU 22px grid at the sample's device-pixel coordinate relative to the container
// origin; the interior fractions keep every sample away from the tile edges.
[[nodiscard]] std::vector<ViewerSample>
buildBackgroundSamples(ui::ViewerEditor& viewer, QWidget& host,
                       const runtime::PreparedPreviewFrame& reference,
                       const ui::ViewerBackground mode) {
    std::vector<ViewerSample> samples;
    const auto view = reference.displayBufferView();
    if (!view.has_value()) {
        return samples;
    }
    const render::ImageWindow window = view->displayWindow;
    const auto width = static_cast<std::uint32_t>(window.extent().width());
    const auto height = static_cast<std::uint32_t>(window.extent().height());
    const auto extent = render::ImageExtent::create(width, height);
    if (!extent || width == 0U || height == 0U) {
        return samples;
    }
    const QRectF destination =
        ui::viewTransformedDisplayRect(viewer.canvasRectForTest(), *extent.value(),
                                       view->pixelAspect, viewer.viewTransformForTest());
    if (!destination.isValid() || destination.width() <= 0.0 || destination.height() <= 0.0) {
        return samples;
    }
    const double dpr = viewer.devicePixelRatioF() > 0.0 ? viewer.devicePixelRatioF() : 1.0;
    const QRectF container = viewer.contentRectForTest();
    const std::array<QPointF, 4> fractions{QPointF(0.13, 0.17), QPointF(0.37, 0.19),
                                           QPointF(0.81, 0.77), QPointF(0.47, 0.33)};
    for (const QPointF fraction : fractions) {
        const QPointF local(destination.left() + fraction.x() * destination.width(),
                            destination.top() + fraction.y() * destination.height());
        const auto originX = static_cast<int>(window.originX());
        const auto originY = static_cast<int>(window.originY());
        const int compX =
            std::clamp(originX + static_cast<int>(fraction.x() * static_cast<double>(width)),
                       originX, originX + static_cast<int>(width) - 1);
        const int compY =
            std::clamp(originY + static_cast<int>(fraction.y() * static_cast<double>(height)),
                       originY, originY + static_cast<int>(height) - 1);
        const render::Rgba8 referencePixel =
            view->pixels[static_cast<std::size_t>(compY - originY) * width +
                         static_cast<std::size_t>(compX - originX)];
        const int deviceX = static_cast<int>(std::lround((local.x() - container.left()) * dpr));
        const int deviceY = static_cast<int>(std::lround((local.y() - container.top()) * dpr));
        const render::Rgba8 backdrop = cpuBackgroundColor(mode, deviceX, deviceY, dpr);
        ViewerSample sample;
        sample.widgetLocal = local;
        sample.windowLocal = QPointF(viewer.mapTo(&host, local.toPoint()));
        sample.expected =
            compositeOver(backdrop, channelReference(referencePixel, ui::ViewerChannel::Rgba));
        sample.opaque = referencePixel.alpha == 255;
        samples.push_back(sample);
    }
    return samples;
}

void emitSamples(const std::string& label, const std::vector<ViewerSample>& samples) {
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const auto& sample = samples[index];
        std::cout << "PHASE viewer-sample label=" << label << " index=" << index
                  << " local=" << static_cast<long>(sample.windowLocal.x()) << ","
                  << static_cast<long>(sample.windowLocal.y())
                  << " expected=" << hexOf(sample.expected)
                  << " opaque=" << (sample.opaque ? "1" : "0") << '\n';
    }
    std::cout << "PHASE viewer-hold label=" << label << " count=" << samples.size() << '\n';
    std::cout.flush();
}

// Holds the real window on screen (pumping events so the compositor keeps it alive and the viewer's
// resident timer keeps presenting) so the external runner can grim-capture this exact phase.
void holdForCapture(const std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

[[nodiscard]] bool setDropdownIndex(ui::ViewerEditor& viewer, const char* objectName,
                                    const int index) {
    auto* dropdown = viewer.findChild<bloom::ui::kit::KDropdown*>(QString::fromLatin1(objectName));
    if (dropdown == nullptr) {
        return false;
    }
    dropdown->setCurrentIndex(index);
    return true;
}

} // namespace

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    const TestOptions options = parseOptions(argc, argv);
    if (!options.valid) {
        return 2;
    }
    QApplication application(argc, argv);
    // Isolate QSettings in a throwaway IniFormat path BEFORE any ViewerEditor is constructed, so
    // the proof never inherits a prior run's or the user's background/channel/overlay preferences.
    const auto settingsRoot =
        std::filesystem::temp_directory_path() /
        ("bloom-viewer-settings-" + std::to_string(QCoreApplication::applicationPid()));
    std::filesystem::create_directories(settingsRoot);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString(settingsRoot.string()));
    QCoreApplication::setOrganizationName(QStringLiteral("BloomProof"));
    QCoreApplication::setApplicationName(QStringLiteral("ViewerProof"));
    const QString platformName = QApplication::platformName();
    phase("start", "platform=" + std::string(platformName.toUtf8().constData()));
    if (platformName != QStringLiteral("wayland") &&
        platformName != QStringLiteral("wayland-egl")) {
        std::cout << "SKIP: a real Wayland platform is required for the viewer present gate "
                     "(platform=" +
                         std::string(platformName.toUtf8().constData()) + ")\n";
        return options.require_device ? 1 : 0;
    }
    if (options.loader_path.empty()) {
        std::cout << "SKIP: --loader <libvulkan.so.1> is required\n";
        return options.require_device ? 1 : 0;
    }
    qputenv("QT_VULKAN_LIB", options.loader_path.string().c_str());

    Expectations checks;
    const auto mediaDirectory =
        std::filesystem::temp_directory_path() /
        ("bloom-viewer-proof-" + std::to_string(QCoreApplication::applicationPid()));
    std::filesystem::create_directories(mediaDirectory);
    Fixture fixture(mediaDirectory);
    if (fixture.processor == nullptr) {
        std::cout << "SKIP: the Bloom Neutral v1 processor is unavailable\n";
        return options.require_device ? 1 : 0;
    }

    auto newProject =
        document::makeNewProject("Viewer Vertical Proof", "Main",
                                 core::RationalTime::fromInteger(10), format1920x1080NonSquare());
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    checks.expect(
        session.addSolidLayer(QStringLiteral("Lower"), core::Color4d{0.15, 0.35, 0.85, 1.0}),
        "the lower Solid layer is created");
    checks.expect(session.setSelectedPosition(0.0, 0.0),
                  "the lower Solid is placed at the origin (visible top-left quadrant)");
    checks.expect(
        session.addSolidLayer(QStringLiteral("Upper"), core::Color4d{0.9, 0.25, 0.1, 1.0}),
        "the upper Solid layer is created");
    checks.expect(session.setSelectedPosition(-386.5, -7.25),
                  "the upper Solid carries a fractional translation (narrower visible region)");
    checks.expect(session.setSelectedBlendMode(core::BlendMode::Normal),
                  "the upper Solid composites source-over (Normal)");
    const auto snapshot = session.snapshot();
    phase("scene", "extent=1920x1080 par=4:3 layers=2 blend=source-over translation=fractional "
                   "regions=orange/blue/background");

    TaskScheduler scheduler(schedulerConfig());
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    // Unmodified production stage factories for EVERY request generation. No test generation
    // branches: a live controller's generations are its own, and the production factory must handle
    // all of them.
    auto gpuStage = ui::makeCompositionPreviewGpuSceneStage(fixture.compiler, *fixture.sceneBuilder,
                                                            fixture.provider);
    auto cpuStage =
        ui::makeCompositionPreviewCpuStage(fixture.compiler, fixture.evaluator, fixture.provider);
    auto cpuFallback = ui::makeCompositionPreviewCpuDisplayFallback(displayPreparer);
    GpuPreviewDisplayService service(scheduler, gpuStage, cpuStage, cpuFallback,
                                     serviceOptions(options.loader_path));

    const bool terminal = waitUntil(
        [&] { return service.status().state != GpuPreviewDisplayServiceState::Initializing; }, 90s);
    const auto status = service.status();
    const bool residentReady =
        terminal && status.state == GpuPreviewDisplayServiceState::Ready &&
        status.residentQualification != nullptr && status.residentQualification->eligible() &&
        status.presentationClient != nullptr &&
        status.presentationAvailability == render::GpuPresentationAvailability::Ready;
    if (!residentReady) {
        checks.expect(
            !options.require_device,
            "a presentable Wayland device with a qualified resident route is required but "
            "unavailable: " +
                status.residentDetail + " / " + status.presentationDetail);
        service.beginShutdown();
        if (checks.failures() == 0) {
            std::cout << "SKIP: resident route unavailable; no viewer success claimed\n";
        }
        return checks.failures() == 0 ? 0 : 1;
    }
    const std::shared_ptr<GpuPresentationClient> client = status.presentationClient;
    phase("service-ready", "state=Ready presentation=Ready");

    // Diagnostic: prove the service itself still processes a direct submit in this binary.
    {
        TaskOwner directOwner;
        directOwner.kind = TaskOwnerKind::Composition;
        directOwner.id = TaskOwnerId::fromRaw(9);
        auto direct = service.submit(TaskRequest("viewer-direct", directOwner), snapshot,
                                     makeIdentity(1, snapshot, compositionId), kBudget, {});
        const auto directResult = awaitResult(direct.handle, 30s);
        const auto directFrame =
            directResult.has_value() && directResult->state() == TaskState::Succeeded
                ? residentFrameOf(*directResult)
                : nullptr;
        phase(
            "direct-submit",
            "state=" +
                std::to_string(directResult.has_value() ? static_cast<int>(directResult->state())
                                                        : -1) +
                " resident=" + (isResident(directFrame) ? "1" : "0") + " residentGraphJobs=" +
                std::to_string(service.status().counters.residentGraphJobs) +
                " nativeDispatches=" + std::to_string(service.status().counters.nativeDispatches));
    }

    ui::TaskUiBridge bridge(scheduler, nullptr, std::chrono::milliseconds(1));
    auto pipeline = ui::makeCompositionPreviewPipeline(fixture.compiler, fixture.evaluator,
                                                       displayPreparer, fixture.provider);
    ui::CompositionPreviewSettings settings;
    settings.pixelStorageByteLimit = kBudget;
    settings.resolutionPolicy = runtime::PreviewResolutionPolicy::Auto;
    settings.quality = runtime::EvaluationQuality::Reference;
    settings.colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene;

    // The ONE real service owns GPU submission; the controller only builds the request.
    std::atomic<int> submitCount{0};
    auto submitter = [&service, &submitCount](
                         runtime::TaskRequest request, const document::Snapshot& snap,
                         const runtime::PreviewRequestIdentity& identity, const std::size_t limit,
                         const std::vector<runtime::SnapshotParameterOverride>& overrides) {
        submitCount.fetch_add(1);
        return service.submit(std::move(request), snap, identity, limit, overrides);
    };
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline, settings,
                                                nullptr, nullptr, submitter);

    auto host = std::make_unique<QWidget>();
    host->setWindowTitle(QString::fromStdString(
        "BloomViewerProof-" + std::to_string(QCoreApplication::applicationPid())));
    host->resize(960, 640);
    auto* layout = new QVBoxLayout(host.get());
    layout->setContentsMargins(0, 0, 0, 0);
    auto viewer = std::make_unique<ui::ViewerEditor>(session, controller);
    layout->addWidget(viewer.get());
    host->show();
    viewer->show();
    phase("viewer-window", "title=" + host->windowTitle().toStdString() + " extent=960x640");

    // Deterministic background Black (index 2 of Solid/Checkerboard/Black/White) and every viewer
    // overlay action off, so the samples never race a handle/selection/overlay paint.
    checks.expect(setDropdownIndex(*viewer, "viewerBackgroundDropdown",
                                   static_cast<int>(ui::ViewerBackground::Black)),
                  "the background is set deterministically to Black");
    for (QAction* action : viewer->findChildren<QAction*>()) {
        const QString text = action->text();
        if (action->isCheckable() &&
            (action->objectName() == QLatin1String("viewerSafeAreasAction") ||
             text.contains(QLatin1String("Thirds")) || text.contains(QLatin1String("Rulers")) ||
             text.contains(QLatin1String("Pixel Grid")) ||
             text.contains(QLatin1String("Centre")))) {
            action->setChecked(false);
        }
    }
    viewer->setGpuPresentationDependencies(client, &scheduler, options.loader_path.string(), 1.0);
    controller.requestRefresh();

    const bool presented = waitUntil(
        [&] {
            const auto frame = controller.state().frame;
            return frame != nullptr &&
                   frame->provenance().provider == runtime::PreviewDisplayProvider::GpuResident &&
                   viewer->residentPresentationActiveForTest();
        },
        30s);
    const auto displayed = controller.state().frame;
    phase("viewer-diagnostic",
          "activity=" + std::to_string(static_cast<int>(controller.state().activity)) +
              " frame=" + (displayed != nullptr ? "1" : "0") + " resident=" +
              (displayed != nullptr && displayed->provenance().provider ==
                                           runtime::PreviewDisplayProvider::GpuResident
                   ? "1"
                   : "0") +
              " residentGraphJobs=" + std::to_string(service.status().counters.residentGraphJobs) +
              " residentFailures=" + std::to_string(service.status().counters.residentFailures) +
              " cpuFallbacks=" + std::to_string(service.status().counters.cpuFallbacks) +
              " detail=" +
              (service.status().residentDetail.empty() ? "none" : service.status().residentDetail) +
              " submitCount=" + std::to_string(submitCount.load()) + " neutral=" +
              (controller.state().desiredIdentity.has_value() &&
                       runtime::detail::gpuPreviewDisplayRequestIsNeutral(
                           *controller.state().desiredIdentity)
                   ? "1"
                   : "0") +
              " output=" +
              std::to_string(controller.state().desiredIdentity.has_value()
                                 ? static_cast<int>(controller.state().desiredIdentity->output)
                                 : -1) +
              " quality=" +
              std::to_string(controller.state().desiredIdentity.has_value()
                                 ? static_cast<int>(controller.state().desiredIdentity->quality)
                                 : -1) +
              " colorIntentLinear=" +
              (controller.state().desiredIdentity.has_value() &&
                       controller.state().desiredIdentity->colorIntent ==
                           runtime::EvaluationColorIntent::LinearRec709Scene
                   ? "1"
                   : "0") +
              " resolutionPolicy=" +
              std::to_string(
                  controller.state().desiredIdentity.has_value()
                      ? static_cast<int>(controller.state().desiredIdentity->resolutionPolicy)
                      : -1) +
              " message=" + controller.state().message.toStdString());
    checks.expect(displayed != nullptr && displayed->provenance().provider ==
                                              runtime::PreviewDisplayProvider::GpuResident,
                  "the controller received the genuine GPU-resident frame from the real service: " +
                      service.status().residentDetail);
    checks.expect(
        viewer->residentPresentationActiveForTest(),
        "the real ViewerEditor genuinely presented the resident frame (owner present ack)");
    checks.expect(viewer->gpuPresentAcceptedCountForTest() >= 1U,
                  "the ViewerEditor recorded at least one accepted present");
    if (!presented || displayed == nullptr) {
        service.beginShutdown();
        bridge.beginShutdown();
        (void)waitUntil([&] { return scheduler.isQuiescent(); }, 10s);
        return 1;
    }
    phase("viewer-presented",
          "presentAccepted=" + std::to_string(viewer->gpuPresentAcceptedCountForTest()) +
              " readbacks=" + std::to_string(service.status().counters.fullFrameReadbacks));

    // The reference MUST use the displayed resident frame's own desired identity, so it preserves
    // the resolved Auto/proxy resolution, origin/window and request the GPU actually presented.
    auto referenceFor = [&]() -> std::shared_ptr<const runtime::PreparedPreviewFrame> {
        const auto frame = controller.state().frame;
        if (frame == nullptr) {
            return nullptr;
        }
        return snapshotCpuReference(fixture, snapshot, frame->desiredIdentity());
    };
    auto reference = referenceFor();
    checks.expect(reference != nullptr && reference->displayBufferView().has_value(),
                  "the document CPU reference packs real pixels at the displayed identity");
    if (reference == nullptr) {
        return 1;
    }

    // Phase 1: RGBA / Solid at fit.
    emitSamples("fit-rgba", buildSamples(*viewer, *host, *reference, ui::ViewerChannel::Rgba));
    holdForCapture(4s);

    // Phase 2: RGBA / Solid after a real wheel zoom and a real middle-button pan.
    {
        const QPointF center(viewer->width() / 2.0, viewer->height() / 2.0);
        QWheelEvent wheel(center, viewer->mapToGlobal(center.toPoint()), QPoint(0, 0),
                          QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
        QApplication::sendEvent(viewer.get(), &wheel);
        const QPointF press(320.0, 240.0);
        const QPointF move(360.0, 258.0);
        QMouseEvent pressEvent(QEvent::MouseButtonPress, press,
                               viewer->mapToGlobal(press.toPoint()), Qt::MiddleButton,
                               Qt::MiddleButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.get(), &pressEvent);
        QMouseEvent moveEvent(QEvent::MouseMove, move, viewer->mapToGlobal(move.toPoint()),
                              Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.get(), &moveEvent);
        QMouseEvent releaseEvent(QEvent::MouseButtonRelease, move,
                                 viewer->mapToGlobal(move.toPoint()), Qt::MiddleButton,
                                 Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.get(), &releaseEvent);
    }
    checks.expect(waitUntil([&] { return viewer->residentPresentationActiveForTest(); }, 15s),
                  "the viewer re-presents after zoom/pan");
    reference = referenceFor();
    checks.expect(reference != nullptr,
                  "the displayed identity still resolves a CPU reference after zoom/pan");
    emitSamples("zoom-pan-rgba", buildSamples(*viewer, *host, *reference, ui::ViewerChannel::Rgba));
    holdForCapture(4s);

    // Phase 3: Red channel (the channel dropdown index maps directly to ViewerChannel).
    checks.expect(setDropdownIndex(*viewer, "viewerChannelDropdown",
                                   static_cast<int>(ui::ViewerChannel::Red)),
                  "the channel dropdown is reachable and set to Red");
    checks.expect(
        waitUntil([&] { return viewer->channelForTest() == ui::ViewerChannel::Red; }, 10s),
        "the viewer reports the Red channel");
    checks.expect(waitUntil([&] { return viewer->residentPresentationActiveForTest(); }, 15s),
                  "the viewer re-presents after the channel change");
    reference = referenceFor();
    checks.expect(reference != nullptr,
                  "the displayed identity still resolves a CPU reference after the channel change");
    emitSamples("red-rgba", buildSamples(*viewer, *host, *reference, ui::ViewerChannel::Red));
    holdForCapture(4s);

    // Phase 4: RGBA background proofs. Each mode emits samples whose expected colour composites the
    // CPU reference over that mode's own CPU background, including transparent composition pixels,
    // so the external capture can prove the GPU surround matches the CPU paint for Solid (Canvas),
    // Black, White, and Checkerboard. This is the binding background pixel proof, not a claim from
    // the request fields. The channel is returned to RGBA so the surround, not the remap, is what
    // these samples isolate.
    checks.expect(setDropdownIndex(*viewer, "viewerChannelDropdown",
                                   static_cast<int>(ui::ViewerChannel::Rgba)),
                  "the channel dropdown is returned to RGBA for the background phases");
    checks.expect(
        waitUntil([&] { return viewer->channelForTest() == ui::ViewerChannel::Rgba; }, 10s),
        "the viewer reports the RGBA channel for the background phases");
    struct BackgroundPhase final {
        ui::ViewerBackground mode;
        const char* label;
    };
    const std::array<BackgroundPhase, 4> backgroundPhases{
        BackgroundPhase{ui::ViewerBackground::Solid, "bg-solid-rgba"},
        BackgroundPhase{ui::ViewerBackground::Black, "bg-black-rgba"},
        BackgroundPhase{ui::ViewerBackground::White, "bg-white-rgba"},
        BackgroundPhase{ui::ViewerBackground::Checkerboard, "bg-checker-rgba"}};
    for (const auto& phaseSpec : backgroundPhases) {
        checks.expect(
            setDropdownIndex(*viewer, "viewerBackgroundDropdown", static_cast<int>(phaseSpec.mode)),
            std::string("the background dropdown is set to ") + phaseSpec.label);
        checks.expect(waitUntil([&] { return viewer->backgroundForTest() == phaseSpec.mode; }, 10s),
                      std::string("the viewer reports the background mode for ") + phaseSpec.label);
        checks.expect(waitUntil([&] { return viewer->residentPresentationActiveForTest(); }, 15s),
                      std::string("the viewer re-presents for ") + phaseSpec.label);
        reference = referenceFor();
        checks.expect(reference != nullptr,
                      std::string("the displayed identity still resolves a CPU reference for ") +
                          phaseSpec.label);
        if (reference == nullptr) {
            break;
        }
        emitSamples(phaseSpec.label,
                    buildBackgroundSamples(*viewer, *host, *reference, phaseSpec.mode));
        holdForCapture(4s);
    }
    phase("viewer-checker",
          "residentActive=" + std::string(viewer->residentPresentationActiveForTest() ? "1" : "0"));

    checks.expect(service.status().counters.fullFrameReadbacks == 0U,
                  "the whole viewer run performed no full-frame readback");

    // Retire the live native surface BEFORE the presentation owner is torn down, so the presenter
    // is never destroyed with a live target (the retire-before-mutation contract).
    bool viewerRetired = false;
    static_cast<void>(viewer->prepareNativeSurfaceMutation(
        77, [&](const std::uint64_t generation, const ui::EditorNativeSurface::PrepareResult& r) {
            checks.expect(generation == 77, "the viewer mutation generation is echoed");
            viewerRetired = r.safeToMutate;
        }));
    checks.expect(waitUntil([&] { return viewerRetired; }, 20s),
                  "the ViewerEditor native surface genuinely retired before teardown");
    viewer->setGpuPresentationDependencies(nullptr, nullptr, std::string{}, 1.0);
    // Destroy the ViewerEditor and its native QWindow surface while the borrowed instance is still
    // alive; the presentation owner shuts down only after no live surface remains.
    viewer.reset();
    host.reset();

    controller.beginShutdown();
    bridge.beginShutdown();
    (void)waitUntil([&] { return scheduler.isQuiescent(); }, 10s);
    service.beginShutdown();
    (void)waitUntil(
        [&] { return service.status().state == GpuPreviewDisplayServiceState::Stopped; }, 30s);
    phase("shutdown", "drained=1");
    std::error_code ignored;
    std::filesystem::remove_all(mediaDirectory, ignored);

    if (checks.failures() == 0) {
        std::cout << "PASS: real ViewerEditor consumed and presented the genuine service resident "
                     "frame; mapped sample references emitted for external pixel proof\n";
    }
    return checks.failures() == 0 ? 0 : 1;
}
