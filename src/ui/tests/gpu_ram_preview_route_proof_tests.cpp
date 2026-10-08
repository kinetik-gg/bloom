// Genuine `route.preview.ram_preview` proof harness.
//
// It drives the REAL ui::RamPreviewController over a real multi-frame animated composition. Each
// frame is submitted through the real GpuPreviewDisplayService resident overload (Wayland
// presentation) via the controller's submitter, so the controller's shared PreviewFrameCache is
// filled with genuine GPU-resident frames. The run is then replayed: every cached resident frame is
// taken back out of the cache, verified against the CPU oracle (deep plan identity, request
// identity, evaluated geometry, real CPU pixels), and the warm run must perform zero additional
// native dispatches. No fake service, no relabelled viewer proof.
//
// The proof is the real executed provenance: the positive cold native dispatch delta, the actual
// device ownership epoch, the ordered per-frame ProcessFrameIdentity digest, and the CPU-oracle
// sparse-sample evidence digest. Preview routes carry no readback fields.

#include "gpu_route_proof_harness_support.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_gpu_scene_stage.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QSettings>
#include <QVulkanInstance>
#include <QWindow>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace bloom::ui::verticalproof;
namespace routeproof = bloom::ui::routeproof;
using namespace std::chrono_literals;

namespace {

using namespace bloom;

constexpr std::int64_t kFrameRate = 25;
constexpr std::int64_t kFrameCount = 4;

struct HarnessOptions final {
    std::filesystem::path loader;
    std::filesystem::path proofDirectory;
    bool requireDevice = false;
    bool valid = true;
    bool sequence100 = false;
    bool half = false;
    bool expectPartial = false;
    bool expireBeforeAdmission = false;
    std::uint64_t poolBytes = 1ULL << 30;
};

[[nodiscard]] HarnessOptions parseHarnessOptions(const int argc, char** argv) {
    HarnessOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader" && index + 1 < argc) {
            options.loader = argv[++index];
        } else if (argument == "--proof-dir" && index + 1 < argc) {
            options.proofDirectory = argv[++index];
        } else if (argument == "--sequence-100-half") {
            options.sequence100 = true;
            options.half = true;
        } else if (argument == "--sequence-100-full") {
            options.sequence100 = true;
        } else if (argument == "--pool-2gib") {
            options.poolBytes = 2ULL << 30;
        } else if (argument == "--pool-3gib") {
            options.poolBytes = 3ULL << 30;
        } else if (argument == "--expire-before-admission") {
            options.expireBeforeAdmission = true;
        } else if (argument == "--expect-partial") {
            options.expectPartial = true;
        } else if (argument == "--require-device") {
            options.requireDevice = true;
        } else {
            options.valid = false;
            return options;
        }
    }
    return options;
}

[[nodiscard]] core::RationalTime frameTime(const std::int64_t index) {
    const auto value = core::RationalTime::create(index, kFrameRate);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

[[nodiscard]] document::CompositionFormat ramFormat() {
    const auto rate = document::FrameRate::create(kFrameRate, 1);
    if (!rate.has_value()) {
        std::abort();
    }
    const auto format =
        document::CompositionFormat::create(1920U, 1080U, core::PixelAspectRatio::square(), *rate);
    if (!format.has_value()) {
        std::abort();
    }
    return *format;
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
        std::cout << "SKIP: RAM route proof needs a real Wayland platform (platform="
                  << platformName.toStdString() << ")\n";
        return options.requireDevice ? 1 : 0;
    }
    if (options.loader.empty() || options.proofDirectory.empty()) {
        std::cout << "SKIP: RAM route proof needs --loader and --proof-dir\n";
        return options.requireDevice ? 1 : 0;
    }
    qputenv("QT_VULKAN_LIB", options.loader.string().c_str());

    const auto settingsRoot =
        std::filesystem::temp_directory_path() /
        ("bloom-ram-route-proof-" + std::to_string(QCoreApplication::applicationPid()));
    std::filesystem::create_directories(settingsRoot);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString(settingsRoot.string()));
    QCoreApplication::setOrganizationName(QStringLiteral("BloomRouteProof"));
    QCoreApplication::setApplicationName(QStringLiteral("RamRouteProof"));

    std::string nonce;
    if (!routeproof::readProofNonce(options.proofDirectory, nonce)) {
        std::cout << "SKIP: no fresh run nonce in " << options.proofDirectory.string() << '\n';
        return options.requireDevice ? 1 : 0;
    }

    Expectations checks;
    const auto mediaDirectory =
        std::filesystem::temp_directory_path() /
        ("bloom-ram-route-" + std::to_string(QCoreApplication::applicationPid()));
    std::filesystem::create_directories(mediaDirectory);
    Fixture fixture(mediaDirectory);
    if (fixture.processor == nullptr) {
        std::cout << "SKIP: the Bloom Neutral v1 processor is unavailable\n";
        return options.requireDevice ? 1 : 0;
    }

    const auto frameCount = options.sequence100 ? std::int64_t{100} : kFrameCount;
    auto sequenceAsset = fixture.mediaAsset;
    if (options.sequence100) {
        sequenceAsset.kind = document::AssetKind::Sequence;
        sequenceAsset.width = 1920;
        sequenceAsset.height = 1080;
        sequenceAsset.frames = 100;
        sequenceAsset.rate = 25;
        sequenceAsset.manifest.pattern = "ram.####.exr";
        sequenceAsset.manifest.padding = 4;
        sequenceAsset.manifest.first = 0;
        sequenceAsset.manifest.last = 99;
        for (std::int64_t index = 0; index < frameCount; ++index) {
            const auto path = mediaDirectory / ("ram." + std::to_string(index) + ".exr");
            const auto red = static_cast<float>(index + 1) / 128.0F;
            writeExrRgba(
                path, 1920, 1080,
                std::vector<ExrPixel>(std::size_t{1920} * 1080, ExrPixel{red, 0.3F, 0.7F, 1.0F}));
            const auto member = imageAsset(path, "RAM sequence member", 901);
            sequenceAsset.manifest.members.push_back({index, member.locator, member.contentDigest});
        }
        sequenceAsset.locator = sequenceAsset.manifest.members.front().locator;
        sequenceAsset.contentDigest = sequenceAsset.manifest.members.front().contentDigest;
        fixture.evaluator.operationCache()->setByteBudget(std::size_t{256} * 1024 * 1024);
    }
    const auto duration = core::RationalTime::create(frameCount, kFrameRate);
    if (!duration.has_value()) {
        return 2;
    }
    auto newProject = document::makeNewProject("RAM Route Proof", "Main", *duration, ramFormat());
    if (options.sequence100)
        checks.expect(newProject.project.addAsset(sequenceAsset), "the sequence asset validates");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    if (options.sequence100) {
        commands::Transaction addImage("Add RAM sequence");
        addImage.emplace<commands::AddImageLayer>(compositionId, sequenceAsset.id);
        checks.expect(commands.execute(std::move(addImage)).succeeded(),
                      "the genuine sequence layer is added");
    } else {
        checks.expect(session.addSolidLayer(QStringLiteral("Moving Solid"),
                                            core::Color4d{0.2, 0.4, 0.8, 1.0}),
                      "the animated RAM solid layer is created");
        checks.expect(session.currentTime() == frameTime(0) || session.setCurrentTime(frameTime(0)),
                      "the RAM session is at frame 0");
        checks.expect(session.toggleKeyframe("position"), "the position keyframe is created");
        checks.expect(session.setCurrentTime(frameTime(frameCount - 1)),
                      "the RAM session moves to the last frame");
        checks.expect(session.setSelectedPosition(12.0, 9.0), "the animated position is set");
        checks.expect(session.setCurrentTime(frameTime(0)), "the RAM session returns to frame 0");
    }

    auto scheduling = schedulerConfig();
    if (options.sequence100)
        scheduling.gpuRequestOwnedByteCapacity = 2ULL << 30;
    TaskScheduler scheduler(scheduling);
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    auto nativeOptions = serviceOptions(options.loader);
    if (options.sequence100) {
        const auto configured = runtime::gpuResidentCapacityPlanConfigured(options.poolBytes);
        nativeOptions.residentLeaseBudgets = {configured.leaseBytes,
                                              static_cast<std::size_t>(configured.leaseEntries)};
        nativeOptions.residentSceneCacheBudgets.maxRetainedBytes = configured.sceneCacheBytes;
        nativeOptions.previewByteAllowance = configured.requestBytes;
    }
    GpuPreviewDisplayService service(scheduler, fixture.productionGpuStage(),
                                     fixture.productionCpuStage(),
                                     fixture.productionCpuFallback(displayPreparer), nativeOptions);
    const bool terminal = waitUntil(
        [&] { return service.status().state != GpuPreviewDisplayServiceState::Initializing; }, 90s);
    const auto status = service.status();
    const bool residentReady =
        terminal && status.state == GpuPreviewDisplayServiceState::Ready &&
        status.residentQualification != nullptr && status.residentQualification->eligible() &&
        status.presentationClient != nullptr &&
        status.presentationAvailability == render::GpuPresentationAvailability::Ready;
    if (!residentReady) {
        std::cout << "SKIP: RAM resident route unavailable: " << status.residentDetail << " / "
                  << status.presentationDetail << '\n';
        service.beginShutdown();
        return options.requireDevice ? 1 : 0;
    }
    const auto core = runtime::detail::GpuPreviewDisplayServiceTestAccess::coreOf(service);
    checks.expect(core != nullptr, "the service core is reachable");
    const auto epoch =
        core == nullptr
            ? 0U
            : runtime::detail::GpuPreviewDisplayServiceTestAccess::ownershipEpoch(*core);
    checks.expect(epoch > 0U, "the service publishes a genuine nonzero device ownership epoch");

    // The resident route requires a live presentation target on this service generation; attach a
    // real Wayland VkSurfaceKHR through the borrowed instance, exactly as the product viewer does.
    const std::shared_ptr<GpuPresentationClient> client = status.presentationClient;
    const GpuBorrowedInstanceView view = client->instanceView();
    checks.expect(view.valid, "the RAM service client publishes the borrowed instance view");
    if (!view.valid) {
        service.beginShutdown();
        return options.requireDevice ? 1 : 0;
    }
    QVulkanInstance instance;
    // clang-format off
    instance.setVkInstance(reinterpret_cast<VkInstance>(static_cast<std::uintptr_t>(view.instance_bits)));  // NOLINT(performance-no-int-to-ptr)
    // clang-format on
    if (!instance.create() || !instance.isValid()) {
        std::cout << "SKIP: QVulkanInstance could not adopt the borrowed instance\n";
        service.beginShutdown();
        return options.requireDevice ? 1 : 0;
    }
    auto window = std::make_unique<QWindow>();
    window->setSurfaceType(QSurface::VulkanSurface);
    window->setVulkanInstance(&instance);
    window->setTitle(QString::fromStdString("BloomRamRouteProof-" +
                                            std::to_string(QCoreApplication::applicationPid())));
    window->resize(320, 240);
    window->show();
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    checks.expect(waitUntil(
                      [&] {
                          surface = QVulkanInstance::surfaceForWindow(window.get());
                          return surface != VK_NULL_HANDLE;
                      },
                      10s),
                  "the RAM proof QWindow produced a Wayland VkSurfaceKHR");
    if (surface == VK_NULL_HANDLE) {
        instance.destroy();
        service.beginShutdown();
        return 1;
    }
    GpuBorrowedSurface borrowed;
    borrowed.surface_bits = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(surface));
    borrowed.epoch = view.epoch;
    const auto attached = client->attach(borrowed, 320U, 240U);
    checks.expect(attached.code == GpuPresentationPortCode::Accepted,
                  "the RAM service client admitted the Wayland surface");
    checks.expect(waitUntil(
                      [&] {
                          return client->status(attached.target).state ==
                                 GpuPresentationTargetState::Active;
                      },
                      15s),
                  "the RAM service owner activated the presentation target");

    ui::TaskUiBridge bridge(scheduler, nullptr, std::chrono::milliseconds(1));
    auto pipeline = ui::makeCompositionPreviewPipeline(fixture.compiler, fixture.evaluator,
                                                       displayPreparer, fixture.provider);
    ui::CompositionPreviewSettings settings;
    settings.pixelStorageByteLimit = kBudget;
    if (options.sequence100)
        settings.resolutionPolicy = options.half ? runtime::PreviewResolutionPolicy::Half
                                                 : runtime::PreviewResolutionPolicy::Full;
    const ui::PreviewPreparationSubmitter submitter =
        [&service](runtime::TaskRequest request, const document::Snapshot& snap,
                   const runtime::PreviewRequestIdentity& identity, const std::size_t limit,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides)
        -> runtime::TaskSubmission<runtime::PreviewPreparationResultHandle> {
        return service.submit(std::move(request), snap, identity, limit, overrides);
    };
    auto frameCache = std::make_shared<ui::PreviewFrameCache>();
    const auto effective = service.status().residentCapacityPlan;
    frameCache->setGpuResidentLimits(static_cast<std::size_t>(effective.cacheBytes),
                                     static_cast<std::size_t>(effective.cacheEntries));
    std::cout << "CAPACITY pool=" << effective.poolBytes << " cacheBytes=" << effective.cacheBytes
              << " cacheEntries=" << effective.cacheEntries
              << " leaseBytes=" << effective.leaseBytes
              << " leaseEntries=" << effective.leaseEntries
              << " available=" << effective.capacity.availableBytes
              << " request=" << effective.requestBytes << '\n';
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline, settings,
                                                frameCache, nullptr, submitter);
    ui::RamPreviewController ram(session, controller, scheduler, bridge, pipeline, nullptr,
                                 submitter);
    checks.expect(
        waitUntil([&] { return controller.state().activity != ui::PreviewActivity::Rendering; },
                  30s),
        "the foreground frame settled before the RAM run");
    if (options.expireBeforeAdmission) {
        frameCache->setByteBudget(ui::PreviewFrameCache::frameByteCost(*controller.state().frame));
    } else {
        frameCache->clear();
    }

    const auto beforeCold = service.status().counters;
    ram.start();
    if (options.expireBeforeAdmission) {
        // Hold UI delivery while genuine production tasks finish, then invalidate their real
        // leases through normal owner shutdown. No fake GPU product or foreign registry mutation.
        const auto readyDeadline = std::chrono::steady_clock::now() + 30s;
        while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < readyDeadline)
            std::this_thread::sleep_for(1ms);
        checks.expect(scheduler.isQuiescent(),
                      "genuine pending RAM products settled before expiry");
        static_cast<void>(client->retire(attached.target, 5U));
        const auto retireDeadline = std::chrono::steady_clock::now() + 30s;
        while (!client->status(attached.target).surfaceSafeToDestroy &&
               std::chrono::steady_clock::now() < retireDeadline)
            std::this_thread::sleep_for(1ms);
        if (!client->status(attached.target).surfaceSafeToDestroy) {
            std::cerr << "FATAL: unproved expiry-fixture surface retirement\n";
            std::_Exit(1);
        }
        window->hide();
        window.reset();
        instance.destroy();
        service.beginShutdown();
        const auto stopDeadline = std::chrono::steady_clock::now() + 30s;
        while (service.status().state != GpuPreviewDisplayServiceState::Stopped &&
               std::chrono::steady_clock::now() < stopDeadline)
            std::this_thread::sleep_for(1ms);
        checks.expect(service.status().state == GpuPreviewDisplayServiceState::Stopped,
                      "owner shutdown invalidated the genuine leases before UI admission");
        auto foreign = *controller.cacheKeyForTime(frameTime(0));
        foreign.resolutionPolicy = runtime::PreviewResolutionPolicy::Half;
        const auto halfExtent = render::ImageExtent::create(960, 540);
        foreign.resolution = runtime::ProxyResolution{*halfExtent.value()};
        runtime::PreviewRequestIdentity foreignIdentity =
            controller.state().frame->desiredIdentity();
        foreignIdentity.resolutionPolicy = foreign.resolutionPolicy;
        foreignIdentity.resolution = foreign.resolution;
        const auto foreignFrame =
            snapshotCpuReferenceAtResolution(fixture, session.snapshot(), foreignIdentity);
        checks.expect(foreignFrame != nullptr, "the foreign-policy CPU oracle is genuine");
        if (foreignFrame) {
            const auto inserted = frameCache->insert(foreignFrame);
            checks.expect(inserted.evictions != 0 && frameCache->statistics().staleDrops != 0,
                          "a foreign-policy admission evicts an expired requested lease with "
                          "explicit stale evidence");
        }
    }
    const bool coldComplete = waitUntil([&] { return !ram.isCaching(); }, 120s);
    const auto afterCold = service.status().counters;
    const auto coldDispatches = afterCold.nativeDispatches - beforeCold.nativeDispatches;
    checks.expect(coldComplete && ram.totalFrameCount() == static_cast<std::uint64_t>(frameCount),
                  "the requested RAM range terminates");
    const auto expectedCompletion =
        options.expireBeforeAdmission ? ui::RamPreviewController::Completion::Failed
        : options.expectPartial       ? ui::RamPreviewController::Completion::PartialBudget
                                      : ui::RamPreviewController::Completion::Complete;
    checks.expect(ram.lastCompletion() == expectedCompletion,
                  "RAM completion reports measured full/partial/expired admission");
    checks.expect((options.expectPartial || options.expireBeforeAdmission) ? !ram.canAutoPlay()
                                                                           : ram.canAutoPlay(),
                  "only fully retained current coverage permits autoplay");
    std::cout << "RETENTION requested=" << ram.totalFrameCount()
              << " prepared=" << ram.preparedFrameCount() << " retained=" << ram.cachedFrameCount()
              << " charged=" << frameCache->gpuResidentBytes();
    if (ram.playableSpan())
        std::cout << " spanStart=" << ram.playableSpan()->start.toSeconds()
                  << " spanEnd=" << ram.playableSpan()->end.toSeconds()
                  << " spanFrames=" << ram.playableSpan()->frameCount;
    std::cout << '\n';
    checks.expect(afterCold.fullFrameReadbacks == beforeCold.fullFrameReadbacks,
                  "the cold RAM preview performed no full-frame readback");
    std::cout << "PHASE ram-cold frames=" << ram.cachedFrameCount()
              << " coldDispatches=" << coldDispatches << " epoch=" << epoch << '\n';

    const auto identityForIndex = [](const std::int64_t index,
                                     const ui::PreviewFrameCacheKey& key) {
        return runtime::PreviewRequestIdentity{.projectId = key.projectId,
                                               .compositionId = key.compositionId,
                                               .sourceRevision = key.sourceRevision,
                                               .requestGeneration =
                                                   static_cast<std::uint64_t>(index + 1),
                                               .time = key.time,
                                               .output = key.output,
                                               .resolution = key.resolution,
                                               .quality = key.quality,
                                               .colorIntent = key.colorIntent,
                                               .resolutionPolicy = key.resolutionPolicy,
                                               .roi = key.roi,
                                               .viewAdjust = key.viewAdjust,
                                               .displayName = key.displayName,
                                               .viewName = key.viewName,
                                               .showLook = key.showLook};
    };

    std::vector<routeproof::FrameEvidence> evidence;
    std::vector<routeproof::PreparedPreviewFrameHandle> frames;
    for (const auto retainedIndex : ram.retainedFrameIndices()) {
        const auto index = static_cast<std::int64_t>(retainedIndex);
        const auto key = controller.cacheKeyForTime(frameTime(index));
        checks.expect(key.has_value(), "the cached frame has a cache key");
        if (!key.has_value()) {
            continue;
        }
        checks.expect(frameCache->contains(*key), "the cached frame is retained in the cache");
        const auto identity = identityForIndex(index, *key);
        auto frame = frameCache->take(identity);
        checks.expect(frame != nullptr && frame->provenance().provider ==
                                              runtime::PreviewDisplayProvider::GpuResident,
                      "the RAM frame is a genuine GPU-resident frame");
        if (frame == nullptr) {
            continue;
        }
        checks.expect(frame->isDisplayValid(), "the resident RAM lease is live");
        const auto& lease = frame->residentFrame()->lease();
        const auto width = options.half ? 960U : 1920U;
        const auto height = options.half ? 540U : 1080U;
        checks.expect(lease.width() == width && lease.height() == height,
                      "the native retained image has the actual Full/Half dimensions");
        checks.expect(lease.allocationBytes() >= static_cast<std::uint64_t>(width) * height * 4U,
                      "actual native RGBA8 allocation charges cover the physical extent");
        std::cout << "FRAME index=" << index << " width=" << lease.width()
                  << " height=" << lease.height() << " nativeBytes=" << lease.allocationBytes()
                  << " retainedBytes=" << ui::PreviewFrameCache::frameByteCost(*frame) << '\n';

        const auto reference = snapshotCpuReferenceAtResolution(
            fixture, session.evaluationSnapshotForTime(key->time), identity);
        checks.expect(reference != nullptr && reference->displayBufferView().has_value(),
                      "the CPU oracle evaluates the RAM frame identity into real pixels");
        if (reference != nullptr) {
            std::string mismatchDetail;
            checks.expect(routeproof::sameOracleIdentity(*frame, *reference, mismatchDetail),
                          "the RAM frame matches the CPU oracle identity/geometry: " +
                              mismatchDetail);
            routeproof::FrameEvidence frameEvidence;
            std::string evidenceDetail;
            checks.expect(routeproof::buildFrameEvidence(service, *frame, *reference, frameEvidence,
                                                         evidenceDetail),
                          "the actual resident GPU pixels match the CPU oracle: " + evidenceDetail);
            if (checks.failures() == 0) {
                evidence.push_back(std::move(frameEvidence));
            }
        }
        frames.push_back(std::move(frame));
    }
    std::uint64_t sparseBytes = 0;
    std::uint64_t sparseSubmissions = 0;
    for (const auto& frameEvidence : evidence) {
        sparseBytes += frameEvidence.sparseBytes;
        sparseSubmissions += frameEvidence.sparseSubmissions;
    }
    const auto processDigest = routeproof::frameIdentityDigest(frames);
    const auto capturedEvidenceDigest = routeproof::evidenceDigest(evidence);

    // Warm replay: the same range must be answered entirely from the controller cache with zero new
    // native work, and every cached resident frame must actually replay through the real presenter
    // (taking it back out of the controller cache and presenting it), not merely still be held.
    const auto beforeWarm = service.status().counters;
    if (!options.expectPartial && !options.expireBeforeAdmission)
        ram.start();
    const bool warmComplete = waitUntil(
        [&] {
            return !ram.isCaching() &&
                   (options.expectPartial || options.expireBeforeAdmission ||
                    ram.cachedFrameCount() == static_cast<std::uint64_t>(frameCount));
        },
        30s);
    const auto afterWarm = service.status().counters;
    checks.expect(warmComplete, "the warm RAM replay completed from the cache");
    checks.expect(afterWarm.residentGraphJobs == beforeWarm.residentGraphJobs &&
                      afterWarm.cpuFallbacks == beforeWarm.cpuFallbacks &&
                      (options.expectPartial || options.expireBeforeAdmission ||
                       ram.preparedFrameCount() == 0),
                  "warm range reuse performs zero preparation, including no CPU fallback");
    checks.expect(afterWarm.nativeDispatches == beforeWarm.nativeDispatches,
                  "the warm RAM replay performed zero additional native dispatches");
    checks.expect(afterWarm.fullFrameReadbacks == beforeWarm.fullFrameReadbacks,
                  "the warm RAM replay performed no full-frame readback");
    std::size_t replayed = 0;
    std::uint32_t sequence = 0;
    auto replayIndices = ram.retainedFrameIndices();
    if (options.sequence100) {
        const auto forward = replayIndices;
        replayIndices.insert(replayIndices.end(), forward.rbegin(), forward.rend());
    }
    for (const auto retainedIndex : replayIndices) {
        const auto index = static_cast<std::int64_t>(retainedIndex);
        const auto key = controller.cacheKeyForTime(frameTime(index));
        if (!key.has_value()) {
            continue;
        }
        const auto frame = frameCache->take(identityForIndex(index, *key));
        if (frame == nullptr || !frame->isDisplayValid() ||
            frame->provenance().provider != runtime::PreviewDisplayProvider::GpuResident) {
            continue;
        }
        const auto lease = frame->residentFrame()->lease();
        // Give the Wayland compositor time to release the previous swapchain image before the next
        // present; a tight back-to-back present loop can otherwise starve vkAcquireNextImageKHR.
        {
            const auto settleUntil = std::chrono::steady_clock::now() + 150ms;
            while (std::chrono::steady_clock::now() < settleUntil) {
                QApplication::processEvents();
                std::this_thread::sleep_for(2ms);
            }
        }
        const auto update =
            client->update(attached.target, sequence + 1U, makeUpdate(lease, 320U, 240U));
        if (!update.accepted()) {
            continue;
        }
        ++sequence;
        // Wait for the present to COMPLETE (presentCount), not merely to be submitted
        // (appliedSequence), so the next replay never replaces an in-flight present.
        const bool applied = waitUntil(
            [&] { return client->status(attached.target).presentCount >= sequence; }, 15s);
        std::cout << "PHASE ram-replay index=" << index << " code=" << static_cast<int>(update.code)
                  << " seq=" << sequence
                  << " presentCount=" << client->status(attached.target).presentCount
                  << " state=" << static_cast<int>(client->status(attached.target).state)
                  << " targetCode=" << static_cast<int>(client->status(attached.target).targetCode)
                  << " message=" << client->status(attached.target).message << '\n';
        if (!applied) {
            continue;
        }
        ++replayed;
    }
    checks.expect(
        replayed == replayIndices.size(),
        "every cached resident frame replayed through the real controller cache and presenter");
    const auto afterPresentation = service.status().counters;
    checks.expect(afterPresentation.residentGraphJobs == beforeWarm.residentGraphJobs &&
                      afterPresentation.cpuFallbacks == beforeWarm.cpuFallbacks &&
                      afterPresentation.nativeDispatches == beforeWarm.nativeDispatches &&
                      afterPresentation.fullFrameReadbacks == beforeWarm.fullFrameReadbacks,
                  "forward/reverse warm presentation performs zero evaluation or pixel dispatch");
    std::cout << "PHASE ram-warm cached=" << ram.cachedFrameCount()
              << " warmDispatches=" << (afterWarm.nativeDispatches - beforeWarm.nativeDispatches)
              << " replayed=" << replayed
              << " sparseSamples=" << sparseBytes / sizeof(render::Rgba8)
              << " sparseBytes=" << sparseBytes << " sparseSubmissions=" << sparseSubmissions
              << " productionFullFrameReadbacks=" << afterWarm.fullFrameReadbacks << '\n';

    // The expiry case already retired this surface before invalidating the borrowed instance.
    if (window) {
        static_cast<void>(client->retire(attached.target, 5U));
        const bool safe =
            waitUntil([&] { return client->status(attached.target).surfaceSafeToDestroy; }, 30s);
        if (!safe) {
            std::cerr << "FATAL: unproved RAM proof surface retirement\n";
            std::_Exit(1);
        }
        window->hide();
        window.reset();
        instance.destroy();
    }

    ram.beginShutdown();
    controller.beginShutdown();
    bridge.beginShutdown();
    (void)waitUntil([&] { return scheduler.isQuiescent(); }, 10s);
    service.beginShutdown();
    (void)waitUntil(
        [&] { return service.status().state == GpuPreviewDisplayServiceState::Stopped; }, 30s);
    checks.expect(service.status().counters.fullFrameReadbacks == 0U,
                  "the whole RAM run performed no full-frame readback");
    std::error_code ignored;
    std::filesystem::remove_all(mediaDirectory, ignored);

    if (checks.failures() != 0) {
        std::cerr << "RAM route proof: " << checks.failures()
                  << " verification failure(s); no proof written\n";
        return 1;
    }
    if (options.expireBeforeAdmission) {
        checks.expect(ram.cachedFrameCount() == 0 && !ram.canAutoPlay(),
                      "expired admission is failure with zero live coverage");
        std::cout << "PASS: genuine expired RAM admission reports failure, not budget completion\n";
        return checks.failures() == 0 ? 0 : 1;
    }
    const auto written = routeproof::publishPreviewProof(
        options.proofDirectory, nonce, "route.preview.ram_preview",
        runtime::GpuRouteHarnessKind::RamPreview, epoch, coldDispatches, frames.size(),
        processDigest, capturedEvidenceDigest);
    if (!written.written) {
        std::cerr << "RAM route proof was rejected: " << written.detail << '\n';
        return 1;
    }
    std::cout << "PASS: route.preview.ram_preview proof written (frames=" << frames.size()
              << ", coldDispatches=" << coldDispatches << ", epoch=" << epoch << ")\n";
    return 0;
}
