// Production offscreen GPU preparation -> one packed transfer -> ordinary EditorArea/Viewer.
// All observation windows run only under the caller's private compositor.
#include "gpu_service_chain_fixture.hpp"
#include "image_source.hpp"
#include "packed_host_retirement_private.hpp"
#include <bloom/ui/application_shutdown_coordinator.hpp>

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/host/gpu_export_tool_package.hpp>
#include <bloom/output/flat_exr_output_adapter.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>
#include <bloom/runtime/gpu_prepared_packed_preview_product.hpp>
#include <bloom/runtime/gpu_prepared_upload_cache.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/gpu_viewer_bootstrap.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/memory_budget_controller.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <bloom/ui/window_status_bar.hpp>

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPointer>
#include <QSettings>
#include <QSplitter>
#include <QTemporaryDir>
#include <QUrl>
#include <QVBoxLayout>
#include <future>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

using namespace bloom;
using namespace bloom::ui::verticalproof;
using namespace std::chrono_literals;

namespace {
void exactPackedCacheAdmission(Expectations& checks, const ui::PreparedPreviewFrameHandle& frame) {
    checks.expect(frame && frame->provenance().provider ==
                               runtime::PreviewDisplayProvider::GpuPreparedPacked,
                  "cache admission uses a genuine sealed GPU-packed product");
    if (!frame ||
        frame->provenance().provider != runtime::PreviewDisplayProvider::GpuPreparedPacked)
        return;
    const auto actual = frame->displayOnlyFrame()->displayByteCost();
    const auto view = frame->displayBufferView();
    const auto proof = frame->provenance().gpuPreparedPacked;
    checks.expect(
        view && proof && actual > view->pixels.size_bytes() &&
            proof->transfer.hostStorageBytes == view->pixels.size_bytes(),
        "retained cost includes actual tight capacity and proof/envelope/identity metadata");
    if (!view || !proof)
        return;
    ui::PreviewFrameCache below(actual - 1);
    checks.expect(below.insert(frame).outcome ==
                          ui::PreviewFrameCache::InsertOutcome::RejectedBudget &&
                      below.size() == 0 && below.residentBytes() == 0,
                  "one byte below actual packed charge refuses without retaining pixels");
    ui::PreviewFrameCache exact(actual);
    checks.expect(exact.insert(frame).outcome == ui::PreviewFrameCache::InsertOutcome::Retained &&
                      exact.residentBytes() == actual && exact.gpuResidentBytes() == 0,
                  "exact packed admission charges host bytes and no native allocation");
    const auto retained = exact.take(frame->desiredIdentity());
    const auto retainedView = retained ? retained->displayBufferView() : std::nullopt;
    checks.expect(retainedView && retainedView->pixels.data() == view->pixels.data(),
                  "cache retention/restamp shares the completed immutable transfer without a copy");
    std::cout << "PACKED_CACHE actual=" << actual << " host=" << proof->transfer.hostStorageBytes
              << " source=" << proof->transfer.sourceAllocationBytes
              << " staging=" << proof->transfer.stagingAllocationBytes << '\n';
}

void observe(const char* name, ui::ViewerEditor& viewer, QSplitter& splitter, QWidget& host,
             runtime::GpuPreviewDisplayService& service,
             ui::CompositionPreviewController& controller) {
    const auto rect = [](QWidget& widget) {
        const auto top = widget.mapToGlobal(QPoint{});
        std::cout << '[' << top.x() << ',' << top.y() << ',' << widget.width() << ','
                  << widget.height() << ']';
    };
    const auto actual = viewer.displayedPreviewState();
    std::cout << "PACKED_OBSERVATION {\"phase\":\"" << name << "\",\"host\":";
    rect(host);
    std::cout << ",\"viewer\":";
    rect(viewer);
    std::cout << ",\"left\":";
    rect(*splitter.widget(0));
    std::cout << ",\"right\":";
    rect(*splitter.widget(2));
    const auto content = viewer.contentRectForTest();
    const auto origin = viewer.mapToGlobal(QPoint{});
    std::cout << ",\"content\":[" << origin.x() + content.x() << ',' << origin.y() + content.y()
              << ',' << content.width() << ',' << content.height() << ']'
              << ",\"freshness\":" << static_cast<int>(actual.freshness)
              << ",\"activity\":" << static_cast<int>(actual.activity)
              << ",\"ownerApplied\":" << actual.ownerAppliedDisplay
              << ",\"native\":" << viewer.residentPresentationActiveForTest()
              << ",\"covered\":" << viewer.gpuCpuCoverVisibleForTest()
              << ",\"transfers\":" << service.status().counters.packedTransfers;
    const auto resources = service.status().counters;
    std::cout << ",\"transferReservationCeiling\":" << resources.packedReservedBytes
              << ",\"transferSourceBytes\":" << resources.packedRetainedSourceBytes
              << ",\"transferStagingBytes\":" << resources.packedRetainedStagingBytes
              << ",\"transferHostBytes\":" << resources.packedRetainedHostBytes;
    if (actual.frame) {
        const auto format = actual.frame->processIdentity().plan->format();
        const auto extent = render::ImageExtent::create(format.width(), format.height());
        const auto view = actual.frame->displayBufferView();
        if (!view || !extent)
            return;
        const auto descriptor = render::ReferenceDisplayBufferDescriptor::create(
            view->displayWindow, view->pixelAspect);
        if (!descriptor)
            return;
        const ui::ViewerMapping mapping{
            ui::viewTransformedDisplayRect(viewer.canvasRectForTest(), *extent.value(),
                                           format.pixelAspect(), viewer.viewTransformForTest()),
            format, actual.frame->desiredIdentity().resolution, view->pixelAspect,
            *descriptor.value()};
        const auto display = mapping.displayRect.translated(origin);
        std::cout << ",\"display\":[" << display.x() << ',' << display.y() << ',' << display.width()
                  << ',' << display.height() << ']';
        const auto selected = controller.selectedLayerBounds(actual.frame);
        if (!selected.empty()) {
            const auto handles = ui::viewerHandlePoints(mapping, selected.front());
            std::cout << ",\"handles\":[";
            for (std::size_t index = 0; index < handles.size(); ++index) {
                if (index)
                    std::cout << ',';
                std::cout << '[' << origin.x() + handles[index].x() << ','
                          << origin.y() + handles[index].y() << ']';
            }
            std::cout << ']';
        }
    }
    if (actual.frame) {
        const auto view = actual.frame->displayBufferView();
        std::cout << ",\"provider\":" << static_cast<int>(actual.frame->provenance().provider)
                  << ",\"time\":" << actual.frame->desiredIdentity().time.toSeconds();
        if (view) {
            const auto data = view->dataWindow.value_or(view->displayWindow);
            std::cout << ",\"data\":[" << data.originX() << ',' << data.originY() << ','
                      << data.extent().width() << ',' << data.extent().height() << ']';
        }
    }
    std::cout << "}\nPHASE_PACKED_" << name << '\n' << std::flush;
    const auto deadline = std::chrono::steady_clock::now() + 1750ms;
    while (std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        std::this_thread::sleep_for(1ms);
    }
}
} // namespace

#include "gpu_packed_animation_proof.ipp"
#include "gpu_packed_host_retirement_proof.ipp"
#include "gpu_packed_stress_proof.ipp"
// Cache qualification reuses the explicit retirement teardown guard above.
#include "gpu_packed_cache100_proof.ipp"
#include "gpu_packed_playing_proof.ipp"

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    bool cache100 = false, half = false, partial = false, playing = false, animatedDrag = false;
    bool hostOwnership = false, hostRetirement = false, sustained = false;
    bool deliveredDefaults = false;
    std::size_t preparationWidth = 1;
    std::string sequenceManifest;
    std::vector<char*> forwarded{argv[0]};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--sequence-manifest") {
            if (++index >= argc)
                return 2;
            sequenceManifest = argv[index];
        } else if (argument == "--delivered-defaults") {
            deliveredDefaults = true;
        } else if (argument == "--sustained") {
            sustained = cache100 = true;
        } else if (argument == "--width-one") {
            preparationWidth = 1;
        } else if (argument == "--width-two") {
            preparationWidth = ui::RamPreviewPipeline::kMaxInFlight;
        } else if (argument == "--cache100-half") {
            cache100 = true;
            half = true;
        } else if (argument == "--cache100-full")
            cache100 = true;
        else if (argument == "--partial")
            partial = true;
        else if (argument == "--playing")
            playing = true;
        else if (argument == "--animated-drag")
            animatedDrag = true;
        else if (argument == "--host-retirement")
            hostOwnership = hostRetirement = true;
        else if (argument == "--host-ownership")
            hostOwnership = true;
        else
            forwarded.push_back(argv[index]);
    }
    const auto options = parseOptions(static_cast<int>(forwarded.size()), forwarded.data());
    if (!options.valid || (!sequenceManifest.empty() && !cache100) ||
        (deliveredDefaults && !cache100) || (sustained && partial))
        return 2;
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QCoreApplication::setOrganizationName("BloomProof");
    QCoreApplication::setApplicationName("PackedViewer");
    QSettings().setValue("viewer/resolution", "Full");
    if (options.loader_path.empty() || !QApplication::platformName().startsWith("wayland")) {
        std::cout << "Unsupported: private Wayland and genuine Vulkan device are required\n";
        return options.require_device ? 1 : 77;
    }
    if (cache100)
        return runPackedCache100(options, temporary, half, partial, sequenceManifest, sustained,
                                 preparationWidth, deliveredDefaults);
    Expectations checks;
    Fixture fixture(temporary.path().toStdString());
    auto format = document::CompositionFormat::create(1920, 1080);
    if (!format)
        return 2;
    auto project = document::makeNewProject("Packed Viewer", "Main",
                                            core::RationalTime::fromInteger(4), *format);
    document::Document document(std::move(project.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, project.initialCompositionId);
    checks.expect(session.addSolidLayer("Marker", {0.125, 0.25, 0.5, 1.0}),
                  "real marker layer authored");
    checks.expect(session.setSelectedScale(0.65, 0.65),
                  "marker leaves visible surrounding background");
    if (animatedDrag) {
        const auto solid = session.selection();
        checks.expect(session.toggleKeyframe(document::kPositionParameterRole),
                      "solid position start key authored");
        checks.expect(session.setCurrentTime(core::RationalTime::fromInteger(4)),
                      "solid animation end time");
        checks.expect(session.setSelectedPosition(1180.0, 600.0),
                      "solid position end key authored");
        checks.expect(session.setCurrentTime(core::RationalTime::fromInteger(0)),
                      "return to animation start");
        checks.expect(session.addTextLayer(QStringLiteral("Animated text"),
                                           QStringLiteral("Bloom GPU"), 150.0, {1.0, 0.2, 0.5, 1.0},
                                           document::Vec2d{600.0, 430.0}),
                      "animated text authored through session");
        checks.expect(session.toggleKeyframe(document::kOpacityParameterRole),
                      "text opacity start key authored");
        checks.expect(session.setCurrentTime(core::RationalTime::fromInteger(4)),
                      "text animation end time");
        checks.expect(session.setSelectedOpacity(0.2), "text opacity end key authored");
        checks.expect(session.setCurrentTime(core::RationalTime::fromInteger(0)),
                      "animation returns to start");
        if (const auto* layer = std::get_if<document::LayerId>(&solid.primary))
            session.selectLayer(*layer);
    }
    runtime::TaskScheduler scheduler(schedulerConfig());
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    auto resolver =
        host::makePackagedGpuOcioResolver(QCoreApplication::applicationFilePath().toStdString());
    checks.expect(resolver != nullptr, "actual packaged application OCIO resolver is available");
    const auto retirementOwner = render::PackedHostRetirementOwner::create();
    PackedHostTestTeardown retirementTeardown(retirementOwner);
    auto processingPolicy =
        hostOwnership ? std::make_shared<runtime::GpuProcessingPhasePolicy>(4, retirementOwner)
                      : nullptr;
    runtime::GpuScenePreparationPhase processingPhase;
    if (processingPolicy) {
        processingPhase = {.policy = processingPolicy,
                           .envelope = {.metadataBytes = 1024ULL * 1024,
                                        .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                                        .requestedExecutorBytes = kBudget,
                                        .deviceWorkingBytes = 832ULL * 1024 * 1024}};
    }
    auto gpuStage = ui::makeSessionRefreshingGpuSceneStage(
        fixture.compiler, fixture.evaluator, fixture.provider,
        std::make_shared<runtime::GpuSceneCoverageCache>(),
        std::make_shared<runtime::GpuPreparedUploadCache>(), nullptr, resolver, processingPhase);
    if (processingPolicy) {
        struct SampleRoot {
            std::mutex mutex;
            std::unique_ptr<runtime::MemoryBudgetLedger> ledger;
        };
        auto sampleRoot = std::make_shared<SampleRoot>();
        gpuStage = [base = std::move(gpuStage), processingPolicy, sampleRoot](
                       const document::Snapshot& snapshot,
                       const runtime::PreviewRequestIdentity& identity, const std::size_t bytes,
                       const std::vector<runtime::SnapshotParameterOverride>& overrides,
                       runtime::TaskContext& context) {
            // Real sampling happens only on this production CPU-stage worker, before admission.
            auto sample = runtime::memoryBudgetSample(context.cancellation());
            {
                std::lock_guard lock(sampleRoot->mutex);
                if (!sampleRoot->ledger) {
                    sampleRoot->ledger = std::make_unique<runtime::MemoryBudgetLedger>(
                        sample.physicalBytes, sample.machine.availableBytes.value_or(0));
                    sampleRoot->ledger->beginProcessSampling();
                }
                const auto at = runtime::GpuProcessingPhasePolicy::Clock::now();
                const auto state = sampleRoot->ledger->applySample(std::move(sample), at);
                processingPolicy->applyHostSample(state, sampleRoot->ledger->physicalByteBudget(),
                                                  at);
            }
            return base(snapshot, identity, bytes, overrides, context);
        };
    }
    auto serviceSettings = serviceOptions(options.loader_path);
    serviceSettings.product = runtime::GpuPreviewDisplayProductMode::GpuPreparedPacked;
    serviceSettings.presentation = runtime::GpuPreviewDisplayServicePresentationMode::Disabled;
    serviceSettings.processingPhasePolicy = processingPolicy;
    runtime::GpuPreviewDisplayService service(scheduler, gpuStage, fixture.productionCpuStage(),
                                              fixture.productionCpuFallback(displayPreparer),
                                              serviceSettings);
    checks.expect(waitUntil(
                      [&] {
                          return service.status().state !=
                                 runtime::GpuPreviewDisplayServiceState::Initializing;
                      },
                      90s),
                  "production GPU service initializes");
    if (service.status().state != runtime::GpuPreviewDisplayServiceState::Ready) {
        std::cout << "Unsupported: " << service.status().residentDetail << '\n';
        return options.require_device ? 1 : 77;
    }
    checks.expect(!service.status().presentationClient,
                  "offscreen preparation has no native target");
    auto hold = std::make_shared<std::atomic_bool>(false);
    auto polls = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto core = runtime::detail::GpuPreviewDisplayServiceTestAccess::coreOf(service);
    core->packedTransferPollOverride = [hold, polls](render::GpuDisplayTransfer& transfer) {
        if (hold->load()) {
            ++*polls;
            return render::GpuDisplayTransferState::Pending;
        }
        return transfer.poll();
    };
    ui::TaskUiBridge bridge(
        scheduler); // Production 100 ms delivery default, not a fixture fast poll.
    auto pipeline = ui::makeCompositionPreviewPipeline(fixture.compiler, fixture.evaluator,
                                                       displayPreparer, fixture.provider);
    const ui::PreviewPreparationSubmitter submitter =
        [&service](runtime::TaskRequest request, const document::Snapshot& snapshot,
                   const runtime::PreviewRequestIdentity& identity, std::size_t bytes,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides) {
            return service.submit(std::move(request), snapshot, identity, bytes, overrides);
        };
    ui::CompositionPreviewSettings settings;
    settings.pixelStorageByteLimit = kBudget;
    settings.resolutionPolicy = runtime::PreviewResolutionPolicy::Full;
    auto frameCache =
        hostOwnership ? std::make_shared<ui::PreviewFrameCache>(64ULL * 1024 * 1024) : nullptr;
    auto controllerOwner = std::make_unique<ui::CompositionPreviewController>(
        session, scheduler, bridge, pipeline, settings, frameCache, nullptr, submitter,
        std::shared_ptr<ui::PreviewFrameTrace>{}, processingPolicy);
    auto& controller = *controllerOwner;
    ui::EditorRegistry registry;
    checks.expect(ui::registerFoundationEditors(registry, session, controller),
                  "actual production editor registry registers");
    QWidget host;
    host.setWindowTitle("bloom-packed-viewer-proof");
    host.resize(1000, 640);
    QVBoxLayout layout(&host);
    layout.setContentsMargins(0, 0, 0, 0);
    QSplitter splitter(Qt::Horizontal, &host);
    const auto sentinel = [&](const QColor color) {
        auto* panel = new QWidget(&splitter);
        auto palette = panel->palette();
        palette.setColor(QPalette::Window, color);
        panel->setPalette(palette);
        panel->setAutoFillBackground(true);
        panel->setMinimumWidth(40);
        splitter.addWidget(panel);
    };
    sentinel({235, 37, 19});
    auto* area = new ui::EditorArea(registry, "bloom.viewer", "packed-proof", &splitter);
    splitter.addWidget(area);
    sentinel({23, 229, 47});
    splitter.setChildrenCollapsible(true);
    splitter.setSizes({120, 620, 260});
    layout.addWidget(&splitter);
    host.show();
    auto* viewer = area->findChild<ui::ViewerEditor*>();
    if (!viewer)
        return 1;
    ui::WindowStatusBar status(session, &controller);
    status.setDisplayedViewer(viewer);
    const auto current = [&] {
        return waitUntil(
            [&] {
                return viewer->displayedPreviewState().freshness == ui::FrameFreshness::Current;
            },
            30s);
    };
    const auto recover = [&] {
        hold->store(false);
        checks.expect(current() && viewer->displayedFrameForTest() &&
                          viewer->displayedFrameForTest()->provenance().provider ==
                              runtime::PreviewDisplayProvider::GpuPreparedPacked,
                      "each short held transfer recovers with genuine GPU-packed pixels");
    };
    const auto requestHeld = [&](const core::Color4d color) {
        polls->store(0);
        hold->store(true);
        checks.expect(session.setSelectedSolidColor(color), "new GPU display work requested");
        checks.expect(waitUntil([&] { return polls->load() > 0; }, 30s),
                      "real final GPU copy is held before fence observation");
    };
    checks.expect(current(), "genuine GPU-prepared product is actually QWidget-painted Current");
    checks.expect(
        waitUntil([&] { return status.colorChipTextForTest() == "GPU-prepared / CPU-painted"; },
                  3s) &&
            !viewer->displayedPreviewState().ownerAppliedDisplay &&
            !viewer->residentPresentationActiveForTest(),
        "painted provenance is explicit and never native/ownerApplied");
    exactPackedCacheAdmission(checks, viewer->displayedFrameForTest());
    if (hostRetirement)
        return exercisePackedHostRetirement(checks, *viewer, *area, registry, splitter, controller,
                                            bridge, service, processingPolicy, retirementOwner);
    if (hostOwnership) {
        auto external = viewer->displayedFrameForTest();
        const auto view = external ? external->displayBufferView() : std::nullopt;
        const auto proof = external ? external->provenance().gpuPreparedPacked : nullptr;
        if (!external || !view || !proof) {
            checks.expect(false, "rooted Viewer proof requires actual packed pixels/provenance");
            return 1;
        }
        const auto* const originalPixels = view->pixels.data();
        const auto publishedBytes = proof->transfer.hostStorageBytes;
        checks.expect(
            external && view &&
                external->displayOnlyFrame()->packedHostOwnership() ==
                    render::PackedRgba8Ownership::RootedPublished &&
                waitUntil(
                    [&] {
                        const auto root = processingPolicy->snapshot().reservations;
                        return root.admittedPhases == 0 && root.hostReservedBytes == 0 &&
                               root.ownedPackedHostAllocations == 1 && root.ownedNativeImages > 0 &&
                               root.ownedNativeImageBytes > 0 &&
                               root.ownedPackedHostBytes ==
                                   external->provenance()
                                       .gpuPreparedPacked->transfer.hostStorageBytes;
                    },
                    2s),
            "actual production QWidget frame owns one independent rooted packed allocation");
        checks.expect(
            session.setCurrentTime(core::RationalTime::fromInteger(1)) && current() &&
                waitUntil(
                    [&] { return processingPolicy->snapshot().reservations.admittedPhases == 0; },
                    2s) &&
                processingPolicy->snapshot().reservations.ownedPackedHostAllocations == 2,
            "second exact Viewer time cold-prepares a second actual owned allocation");
        const auto beforeWarm = service.status().counters;
        const auto nativeBeforeWarm = processingPolicy->snapshot().reservations;
        checks.expect(
            session.setCurrentTime(core::RationalTime::fromInteger(0)) && current() &&
                service.status().counters.nativeDispatches == beforeWarm.nativeDispatches &&
                service.status().counters.residentGraphJobs == beforeWarm.residentGraphJobs &&
                service.status().counters.packedTransfers == beforeWarm.packedTransfers &&
                service.status().counters.fullFrameReadbacks == beforeWarm.fullFrameReadbacks &&
                processingPolicy->snapshot().reservations.ownedPackedHostAllocations == 2 &&
                processingPolicy->snapshot().reservations.ownedNativeImageBytes ==
                    nativeBeforeWarm.ownedNativeImageBytes &&
                processingPolicy->snapshot().reservations.ownedNativeImages ==
                    nativeBeforeWarm.ownedNativeImages,
            "ordinary warm cached Viewer return shares owned backing with no GPU work or new "
            "allocation");
        controller.purgePreviewCache();
        viewer->update();
        checks.expect(
            waitUntil(
                [&] {
                    const auto painted = viewer->actualDisplayDescriptor();
                    return painted && painted->frame &&
                           painted->frame->displayOnlyFrame() == external->displayOnlyFrame() &&
                           painted->mapping && !painted->native && !painted->cover;
                },
                3s) &&
                frameCache->size() == 0 && frameCache->residentBytes() == 0 &&
                processingPolicy->snapshot().reservations.ownedPackedHostBytes == publishedBytes &&
                processingPolicy->snapshot().reservations.ownedPackedHostAllocations == 1,
            "real cache purge leaves Viewer-painted/external aliases charged and readable");
        {
            const auto paintedFrame = viewer->displayedFrameForTest();
            const auto paintedView =
                paintedFrame ? paintedFrame->displayBufferView() : std::nullopt;
            checks.expect(paintedView && paintedView->pixels.data() == originalPixels,
                          "purged current Viewer still reads the exact original packed allocation");
        }
        controller.beginShutdown();
        delete area; // Destroy the actual Viewer and its displayed frame/presentation aliases.
        controllerOwner.reset();
        const auto externalView = external->displayBufferView();
        checks.expect(
            processingPolicy->snapshot().reservations.ownedPackedHostAllocations == 1 &&
                externalView && externalView->pixels.data() == originalPixels,
            "external frame alone preserves backing after actual Viewer/controller destruction");
        const auto revision = processingPolicy->snapshot().reservations.admissionRevision;
        external.reset();
        checks.expect(
            waitUntil(
                [&] {
                    const auto root = processingPolicy->snapshot().reservations;
                    return root.ownedPackedHostAllocations == 0 && root.ownedPackedHostBytes == 0 &&
                           root.totalHostClaimBytes == 0 && root.admissionRevision == revision + 1;
                },
                2s),
            "last external alias releases one owned allocation and advances admission once");
        bridge.beginShutdown();
        service.beginShutdown();
        checks.expect(waitUntil(
                          [&] {
                              return scheduler.isQuiescent() &&
                                     service.status().state ==
                                         runtime::GpuPreviewDisplayServiceState::Stopped;
                          },
                          30s),
                      "rooted QWidget proof retires asynchronously");
        const auto nativeRetired = processingPolicy->snapshot().reservations;
        checks.expect(nativeRetired.ownedNativeImageBytes == 0 &&
                          nativeRetired.ownedNativeImages == 0 &&
                          nativeRetired.ownedNonlocalNativeHostBytes == 0 &&
                          nativeRetired.totalDeviceClaimBytes == 0,
                      "actual packed Viewer native cache backing retires on the service owner");
        std::cout << "PACKED_HOST_VIEWER bytes=" << publishedBytes
                  << " coldTransfers=" << beforeWarm.packedTransfers
                  << " cacheAfterPurge=" << frameCache->residentBytes() << " finalOwned="
                  << processingPolicy->snapshot().reservations.ownedPackedHostBytes << '\n'
                  << (checks.failures() ? "FAIL" : "PASS")
                  << ": rooted packed Viewer/cache purge ownership\n";
        return checks.failures() ? 1 : 0;
    }
    if (playing || animatedDrag) {
        if (animatedDrag)
            exercisePackedAnimation(checks, fixture, scheduler, *viewer, splitter, host, session,
                                    controller, service, hold);
        else
            exercisePackedPlaying(checks, *viewer, *area, splitter, host, session, controller,
                                  service);
        controller.beginShutdown();
        bridge.beginShutdown();
        service.beginShutdown();
        checks.expect(waitUntil(
                          [&] {
                              return scheduler.isQuiescent() &&
                                     service.status().state ==
                                         runtime::GpuPreviewDisplayServiceState::Stopped;
                          },
                          30s),
                      "playing/input service retires asynchronously");
        std::cout << (checks.failures() ? "FAIL" : "PASS")
                  << ": production GPU-packed playing/input\n";
        return checks.failures() ? 1 : 0;
    }
    const auto baseline = viewer->displayedFrameForTest();
    const auto beforeGeometry = service.status().counters;
    observe("BASE", *viewer, splitter, host, service, controller);
    requestHeld({0.5, 0.125, 0.25, 1.0});
    splitter.setSizes({200, 320, 480});
    QApplication::processEvents();
    splitter.setSizes({250, 220, 530});
    QApplication::processEvents();
    checks.expect(viewer->displayedFrameForTest() == baseline &&
                      viewer->displayedPreviewState().freshness == ui::FrameFreshness::Stale,
                  "held transfer retains the old coherent scene/bounds through shrink");
    observe("PENDING_SHRINK", *viewer, splitter, host, service, controller);
    recover();
    requestHeld({0.125, 0.25, 0.5, 1.0});
    const auto width = area->width();
    splitter.setSizes({350, width, splitter.width() - width - 350 - 2 * splitter.handleWidth()});
    QApplication::processEvents();
    observe("PENDING_MOVE", *viewer, splitter, host, service, controller);
    recover();
    requestHeld({0.5, 0.125, 0.25, 1.0});
    host.resize(600, 420);
    QApplication::processEvents();
    observe("TOP_SHRINK", *viewer, splitter, host, service, controller);
    recover();
    requestHeld({0.125, 0.25, 0.5, 1.0});
    splitter.setSizes({250, 0, 350});
    QApplication::processEvents();
    observe("COLLAPSED", *viewer, splitter, host, service, controller);
    host.resize(1000, 640);
    splitter.setSizes({120, 620, 260});
    QApplication::processEvents();
    recover();
    observe("RECOVERED", *viewer, splitter, host, service, controller);
    controller.setRegionOfInterest(QRectF(320, 180, 640, 360));
    checks.expect(current(), "ROI product paints with data-window mapping");
    observe("ROI", *viewer, splitter, host, service, controller);
    controller.setRegionOfInterest(std::nullopt);
    checks.expect(current(), "full display recovers after ROI");
    checks.expect(service.status().counters.cpuFallbacks == 0 &&
                      service.status().counters.packedTransfers > beforeGeometry.packedTransfers,
                  "required widget fixture executes genuine GPU preparation/final transfer without "
                  "CPU evaluation fallback");
    // Explicit Refresh intentionally bypasses cache for derivation/qualification changes.
    // Ordinary time transport is the warm playback path, so prewarm a second exact time then
    // return through the normal cached seek rather than silently changing Refresh semantics.
    checks.expect(session.setCurrentTime(core::RationalTime::fromInteger(1)) && current(),
                  "a second exact playback time prepares");
    const auto warm = service.status().counters;
    checks.expect(session.setCurrentTime(core::RationalTime::fromInteger(0)) && current(),
                  "ordinary warm playback paints the retained packed product");
    const auto afterWarm = service.status().counters;
    checks.expect(afterWarm.residentGraphJobs == warm.residentGraphJobs &&
                      afterWarm.nativeDispatches == warm.nativeDispatches &&
                      afterWarm.packedTransfers == warm.packedTransfers &&
                      afterWarm.fullFrameReadbacks == warm.fullFrameReadbacks,
                  "warm retained widget display adds no GPU evaluation/dispatch/transfer");
    controller.beginShutdown();
    bridge.beginShutdown();
    service.beginShutdown();
    checks.expect(waitUntil(
                      [&] {
                          return scheduler.isQuiescent() &&
                                 service.status().state ==
                                     runtime::GpuPreviewDisplayServiceState::Stopped;
                      },
                      30s),
                  "widget/service shutdown retires asynchronously");
    std::cout << (checks.failures() ? "FAIL" : "PASS")
              << ": production GPU-packed QWidget Viewer\n";
    return checks.failures() ? 1 : 0;
}
