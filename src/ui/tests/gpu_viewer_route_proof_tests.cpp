// Genuine `route.preview.viewer` proof harness.
//
// It drives the real production viewer chain over a genuine document -- real Document/CommandStack/
// CompositionSession, the real GpuPreviewDisplayService (resident overload, Wayland presentation),
// the real CompositionPreviewController, and the real ViewerEditor presenting the service resident
// frame over a real Wayland VkSurface. No fake frame is injected and no second device/context is
// created.
//
// The proof it publishes is the real executed provenance: the cold native dispatch delta from the
// service counters, the actual device ownership epoch, the ordered per-frame production
// ProcessFrameIdentity digest, and a CPU-oracle sparse-sample evidence digest. Every frame is
// verified against the CPU oracle (deep plan identity, request identity, evaluated geometry, real
// CPU pixels). The warm identical refresh must perform zero additional native dispatches and must
// be served from the content cache. An absent Wayland/device writes no proof and reports so.

#include "gpu_route_proof_harness_support.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_gpu_scene_stage.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QSettings>
#include <QVBoxLayout>
#include <QWidget>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

using namespace bloom::ui::verticalproof;
namespace routeproof = bloom::ui::routeproof;
using namespace std::chrono_literals;

namespace {

using namespace bloom;

struct HarnessOptions final {
    std::filesystem::path loader;
    std::filesystem::path proofDirectory;
    bool requireDevice = false;
    bool valid = true;
};

[[nodiscard]] HarnessOptions parseHarnessOptions(const int argc, char** argv) {
    HarnessOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader" && index + 1 < argc) {
            options.loader = argv[++index];
        } else if (argument == "--proof-dir" && index + 1 < argc) {
            options.proofDirectory = argv[++index];
        } else if (argument == "--require-device") {
            options.requireDevice = true;
        } else {
            options.valid = false;
            return options;
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    const HarnessOptions options = parseHarnessOptions(argc, argv);
    if (!options.valid) {
        return 2;
    }
    QApplication application(argc, argv);
    const QString platformName = QApplication::platformName();
    if (platformName != QStringLiteral("wayland") &&
        platformName != QStringLiteral("wayland-egl")) {
        std::cout << "SKIP: viewer route proof needs a real Wayland platform (platform="
                  << platformName.toStdString() << ")\n";
        return options.requireDevice ? 1 : 0;
    }
    if (options.loader.empty() || options.proofDirectory.empty()) {
        std::cout << "SKIP: viewer route proof needs --loader and --proof-dir\n";
        return options.requireDevice ? 1 : 0;
    }
    qputenv("QT_VULKAN_LIB", options.loader.string().c_str());

    const auto settingsRoot =
        std::filesystem::temp_directory_path() /
        ("bloom-viewer-route-proof-" + std::to_string(QCoreApplication::applicationPid()));
    std::filesystem::create_directories(settingsRoot);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString(settingsRoot.string()));
    QCoreApplication::setOrganizationName(QStringLiteral("BloomRouteProof"));
    QCoreApplication::setApplicationName(QStringLiteral("ViewerRouteProof"));

    std::string nonce;
    if (!routeproof::readProofNonce(options.proofDirectory, nonce)) {
        std::cout << "SKIP: no fresh run nonce in " << options.proofDirectory.string() << '\n';
        return options.requireDevice ? 1 : 0;
    }

    Expectations checks;
    const auto mediaDirectory =
        std::filesystem::temp_directory_path() /
        ("bloom-viewer-route-" + std::to_string(QCoreApplication::applicationPid()));
    std::filesystem::create_directories(mediaDirectory);
    Fixture fixture(mediaDirectory);
    if (fixture.processor == nullptr) {
        std::cout << "SKIP: the Bloom Neutral v1 processor is unavailable\n";
        return options.requireDevice ? 1 : 0;
    }

    auto newProject =
        document::makeNewProject("Viewer Route Proof", "Main", core::RationalTime::fromInteger(10),
                                 format1920x1080NonSquare());
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    checks.expect(
        session.addSolidLayer(QStringLiteral("Lower"), core::Color4d{0.15, 0.35, 0.85, 1.0}),
        "the lower Solid layer is created");
    checks.expect(
        session.addSolidLayer(QStringLiteral("Upper"), core::Color4d{0.9, 0.25, 0.1, 1.0}),
        "the upper Solid layer is created");
    checks.expect(session.setSelectedPosition(13.5, -7.25),
                  "the upper Solid carries a fractional translation");
    checks.expect(session.setSelectedBlendMode(core::BlendMode::Normal),
                  "the upper Solid composites source-over");
    const auto snapshot = session.snapshot();

    TaskScheduler scheduler(schedulerConfig());
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    GpuPreviewDisplayService service(
        scheduler, fixture.productionGpuStage(), fixture.productionCpuStage(),
        fixture.productionCpuFallback(displayPreparer), serviceOptions(options.loader));

    const bool terminal = waitUntil(
        [&] { return service.status().state != GpuPreviewDisplayServiceState::Initializing; }, 90s);
    const auto status = service.status();
    const bool residentReady =
        terminal && status.state == GpuPreviewDisplayServiceState::Ready &&
        status.residentQualification != nullptr && status.residentQualification->eligible() &&
        status.presentationClient != nullptr &&
        status.presentationAvailability == render::GpuPresentationAvailability::Ready;
    if (!residentReady) {
        std::cout << "SKIP: viewer resident route unavailable: " << status.residentDetail << " / "
                  << status.presentationDetail << '\n';
        service.beginShutdown();
        return options.requireDevice ? 1 : 0;
    }
    const std::shared_ptr<GpuPresentationClient> client = status.presentationClient;
    const auto afterReady = service.status().counters;
    const auto core = runtime::detail::GpuPreviewDisplayServiceTestAccess::coreOf(service);
    checks.expect(core != nullptr, "the service core is reachable");
    const auto epoch =
        core == nullptr
            ? 0U
            : runtime::detail::GpuPreviewDisplayServiceTestAccess::ownershipEpoch(*core);
    checks.expect(epoch > 0U, "the service publishes a genuine nonzero device ownership epoch");

    ui::TaskUiBridge bridge(scheduler, nullptr, std::chrono::milliseconds(1));
    auto pipeline = ui::makeCompositionPreviewPipeline(fixture.compiler, fixture.evaluator,
                                                       displayPreparer, fixture.provider);
    ui::CompositionPreviewSettings settings;
    settings.pixelStorageByteLimit = kBudget;
    ui::PreviewPreparationSubmitter submitter =
        [&service](runtime::TaskRequest request, const document::Snapshot& snap,
                   const runtime::PreviewRequestIdentity& identity, const std::size_t limit,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides)
        -> runtime::TaskSubmission<runtime::PreviewPreparationResultHandle> {
        return service.submit(std::move(request), snap, identity, limit, overrides);
    };
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline, settings,
                                                nullptr, nullptr, submitter);

    auto host = std::make_unique<QWidget>();
    host->setWindowTitle(QString::fromStdString(
        "BloomViewerRouteProof-" + std::to_string(QCoreApplication::applicationPid())));
    host->resize(960, 640);
    auto* layout = new QVBoxLayout(host.get());
    layout->setContentsMargins(0, 0, 0, 0);
    auto viewer = std::make_unique<ui::ViewerEditor>(session, controller);
    layout->addWidget(viewer.get());
    host->show();
    viewer->show();
    viewer->setGpuPresentationDependencies(client, &scheduler, options.loader.string(), 1.0);
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
    checks.expect(presented && displayed != nullptr, "the controller produced a resident frame");
    checks.expect(displayed != nullptr && displayed->provenance().provider ==
                                              runtime::PreviewDisplayProvider::GpuResident,
                  "the displayed frame is the genuine GPU-resident arm");
    checks.expect(viewer->residentPresentationActiveForTest(),
                  "the real ViewerEditor genuinely presented the resident frame");
    if (!presented || displayed == nullptr) {
        std::cout << "PHASE viewer-present-failed activity="
                  << static_cast<int>(controller.state().activity)
                  << " frame=" << (displayed != nullptr ? 1 : 0)
                  << " residentDetail=" << service.status().residentDetail
                  << " presentationDetail=" << service.status().presentationDetail << '\n';
        viewer->setGpuPresentationDependencies(nullptr, nullptr, std::string{}, 1.0);
        viewer.reset();
        host.reset();
        controller.beginShutdown();
        bridge.beginShutdown();
        service.beginShutdown();
        return 1;
    }
    const auto afterCold = service.status().counters;
    const auto coldDispatches = afterCold.nativeDispatches - afterReady.nativeDispatches;
    checks.expect(coldDispatches > 0U, "the cold viewer frame performed real native dispatches");
    checks.expect(afterCold.fullFrameReadbacks == 0U,
                  "the resident viewer frame performed no full-frame readback");

    const auto reference =
        snapshotCpuReferenceAtResolution(fixture, snapshot, displayed->desiredIdentity());
    checks.expect(reference != nullptr && reference->displayBufferView().has_value(),
                  "the CPU oracle evaluates the displayed identity into real pixels");
    std::string mismatchDetail;
    if (reference != nullptr) {
        checks.expect(routeproof::sameOracleIdentity(*displayed, *reference, mismatchDetail),
                      "the resident frame matches the CPU oracle identity/geometry: " +
                          mismatchDetail);
    }

    std::vector<routeproof::FrameEvidence> evidence;
    if (reference != nullptr) {
        routeproof::FrameEvidence frameEvidence;
        std::string evidenceDetail;
        checks.expect(routeproof::buildFrameEvidence(service, *displayed, *reference, frameEvidence,
                                                     evidenceDetail),
                      "the actual resident GPU pixels match the CPU oracle: " + evidenceDetail);
        if (checks.failures() == 0) {
            evidence.push_back(std::move(frameEvidence));
        }
    }
    std::uint64_t sparseBytes = 0;
    std::uint64_t sparseSubmissions = 0;
    for (const auto& frameEvidence : evidence) {
        sparseBytes += frameEvidence.sparseBytes;
        sparseSubmissions += frameEvidence.sparseSubmissions;
    }
    const std::array<routeproof::PreparedPreviewFrameHandle, 1> frames{displayed};
    const auto processDigest = routeproof::frameIdentityDigest(frames);
    const auto capturedEvidenceDigest = routeproof::evidenceDigest(evidence);

    // Warm identical refresh: must be served from the content cache with zero new native work.
    const auto beforeWarm = service.status().counters;
    const auto presentBefore = viewer->gpuPresentAcceptedCountForTest();
    controller.requestRefresh();
    const bool warmPresented = waitUntil(
        [&] {
            return viewer->gpuPresentAcceptedCountForTest() > presentBefore &&
                   viewer->residentPresentationActiveForTest();
        },
        20s);
    const auto afterWarm = service.status().counters;
    checks.expect(warmPresented, "the warm viewer refresh re-presented");
    checks.expect(afterWarm.nativeDispatches == beforeWarm.nativeDispatches,
                  "the warm viewer refresh performed zero additional native dispatches");
    checks.expect(afterWarm.gpuCacheHits > beforeWarm.gpuCacheHits,
                  "the warm viewer refresh was served from the content cache");
    std::cout << "PHASE viewer-cold coldDispatches=" << coldDispatches
              << " cacheHits=" << afterCold.gpuCacheHits << " epoch=" << epoch
              << " sparseSamples=" << sparseBytes / sizeof(render::Rgba8)
              << " sparseBytes=" << sparseBytes << " sparseSubmissions=" << sparseSubmissions
              << " productionFullFrameReadbacks=" << afterWarm.fullFrameReadbacks << '\n';

    bool viewerRetired = false;
    static_cast<void>(viewer->prepareNativeSurfaceMutation(
        91,
        [&](const std::uint64_t generation, const ui::EditorNativeSurface::PrepareResult& result) {
            checks.expect(generation == 91, "the viewer mutation generation is echoed");
            viewerRetired = result.safeToMutate;
        }));
    checks.expect(waitUntil([&] { return viewerRetired; }, 20s),
                  "the ViewerEditor native surface retired before teardown");
    viewer->setGpuPresentationDependencies(nullptr, nullptr, std::string{}, 1.0);
    viewer.reset();
    host.reset();

    controller.beginShutdown();
    bridge.beginShutdown();
    (void)waitUntil([&] { return scheduler.isQuiescent(); }, 10s);
    service.beginShutdown();
    (void)waitUntil(
        [&] { return service.status().state == GpuPreviewDisplayServiceState::Stopped; }, 30s);
    checks.expect(service.status().counters.fullFrameReadbacks == 0U,
                  "the whole viewer run performed no full-frame readback");
    std::error_code ignored;
    std::filesystem::remove_all(mediaDirectory, ignored);

    if (checks.failures() != 0) {
        std::cerr << "viewer route proof: " << checks.failures()
                  << " verification failure(s); "
                     "no proof written\n";
        return 1;
    }
    const auto written = routeproof::publishPreviewProof(
        options.proofDirectory, nonce, "route.preview.viewer",
        runtime::GpuRouteHarnessKind::ViewerPreview, epoch, coldDispatches, frames.size(),
        processDigest, capturedEvidenceDigest);
    if (!written.written) {
        std::cerr << "viewer route proof was rejected: " << written.detail << '\n';
        return 1;
    }
    std::cout << "PASS: route.preview.viewer proof written (frames=" << frames.size()
              << ", coldDispatches=" << coldDispatches << ", epoch=" << epoch << ")\n";
    return 0;
}
