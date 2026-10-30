// Benchmark mode of the packed RAM-fill harness: a saved project instead of a synthetic sequence.
// It is wired like the desktop application (apps/bloom/main.cpp): app scheduler and budget view,
// process ledger, MemoryBudgetController, qualified display bootstrap, settings-derived cache
// budgets, the packed GPU preview service, RAM preview fill width from measured capacity, and
// background read-ahead for playback. It measures; it does not qualify parity (the cache100 and
// route-proof gates do). tools/bench/run_bench.py drives it, one fresh process per measurement.
#include <bloom/media/video/session.hpp>
#include <bloom/scripting/live_session_host.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/audio_playback_session.hpp>
#include <bloom/ui/background_preview_controller.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_gpu_scene_stage.hpp>
#include <bloom/ui/live_session_adapter.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/qualified_display_processor_bootstrap.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <sys/resource.h>
#endif

#include <iomanip>
#include <set>
#include <sstream>

namespace {
std::string gBenchProject;
bool gBenchPlayback = false;          // playback-only run: no RAM fill, read-ahead from cold
double gBenchPlaySeconds = 6.0;       // transport run length for either playback measurement
bool gBenchWorkspace = false;         // the application's window and editors, not a bare Viewer
bool gBenchWorkspaceAssets = true;    // the workspace also runs the application's AssetController
std::chrono::seconds gBenchTimeout{1800};

[[nodiscard]] std::string benchJson(const std::string& text) {
    std::ostringstream out;
    out << '"';
    for (const char c : text) {
        const auto code = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\')
            out << '\\' << c;
        else if (code < 0x20)
            out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int{code} << std::dec;
        else
            out << c;
    }
    out << '"';
    return out.str();
}

[[nodiscard]] std::uint64_t benchPeakRssBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)))
        return 0;
    return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
#else
    rusage usage{};
    ::getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss); // macOS reports bytes
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024ULL; // Linux reports KiB
#endif
#endif
}

// Wall-clock epoch seconds, so the runner can align its GPU/CPU samples to the measured window.
[[nodiscard]] double benchEpochSeconds() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] double benchProcessCpuSeconds() {
#if defined(_WIN32)
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user))
        return 0.0;
    const auto seconds = [](const FILETIME& value) {
        const auto ticks = (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
                           value.dwLowDateTime;
        return static_cast<double>(ticks) / 1e7; // 100 ns units
    };
    return seconds(kernel) + seconds(user);
#else
    rusage usage{};
    ::getrusage(RUSAGE_SELF, &usage);
    const auto seconds = [](const timeval& value) {
        return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1e6;
    };
    return seconds(usage.ru_utime) + seconds(usage.ru_stime);
#endif
}

struct BenchPlayback {
    double seconds = 0.0;
    std::uint64_t transportFrames = 0; // distinct times the transport asked for
    std::uint64_t paintedFrames = 0;   // distinct times the Viewer actually showed Current
    std::uint64_t paintedChanges = 0;  // times the shown Current frame changed (counts loops)
    double uiBusySeconds = 0.0;        // time spent inside event-loop slices on the UI thread
    double maxSliceMs = 0.0;           // the longest single event-loop slice
    std::uint64_t longSlices = 0;      // slices longer than one frame period
};

// Plays from the start for gBenchPlaySeconds and counts what the Viewer really showed.
BenchPlayback benchPlay(ui::CompositionSession& session, ui::PlaybackController& playback,
                        ui::ViewerEditor& viewer) {
    double framePeriod = 1.0 / 24.0;
    if (const auto* composition = session.composition(); composition != nullptr) {
        const auto rate = composition->format().frameRate();
        if (rate.numerator() > 0)
            framePeriod = static_cast<double>(rate.denominator()) / rate.numerator();
    }
    BenchPlayback result;
    static_cast<void>(session.setCurrentTime(core::RationalTime::fromInteger(0)));
    std::set<std::int64_t> requested, painted;
    std::optional<std::int64_t> lastPainted;
    const auto toKey = [](const core::RationalTime& time) {
        return static_cast<std::int64_t>(std::llround(time.toSeconds() * 1e6));
    };
    const auto started = std::chrono::steady_clock::now();
    playback.play();
    const auto deadline = started + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                        std::chrono::duration<double>(gBenchPlaySeconds));
    std::cout << "BENCH_PLAY_START\n" << std::flush;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto sliceStarted = std::chrono::steady_clock::now();
        QApplication::processEvents();
        const auto slice =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - sliceStarted).count();
        result.uiBusySeconds += slice;
        result.maxSliceMs = std::max(result.maxSliceMs, slice * 1000.0);
        if (slice > framePeriod)
            ++result.longSlices;
        requested.insert(toKey(session.currentTime()));
        const auto shown = viewer.displayedPreviewState();
        if (shown.freshness == ui::FrameFreshness::Current && shown.frame) {
            const auto key = toKey(shown.frame->desiredIdentity().time);
            painted.insert(key);
            if (lastPainted != key)
                ++result.paintedChanges;
            lastPainted = key;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    playback.pause();
    std::cout << "BENCH_PLAY_END\n" << std::flush;
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    result.transportFrames = requested.size();
    result.paintedFrames = painted.size();
    return result;
}

int runPackedBenchProject(const TestOptions& options, const bool half) {
    const char* resolution = gQuarter ? "Quarter" : (half ? "Half" : "Full");
    const std::filesystem::path projectPath = std::filesystem::absolute(gBenchProject);
    QSettings().setValue("viewer/resolution", resolution);
    const auto processStarted = std::chrono::steady_clock::now();

    runtime::processMemoryBudgetLedger().beginProcessSampling();
    const auto taskBudgetView = std::make_shared<runtime::TaskBudgetView>();
    auto taskConfig = runtime::TaskSchedulerConfig::defaults();
    taskConfig.budgetView = taskBudgetView;
    runtime::TaskScheduler scheduler(taskConfig);
    ui::ProjectHost projectHost(scheduler);
    bool replaced = false;
    std::optional<ui::ProjectHostOperationOutcome> openOutcome;
    QString openMessage;
    QObject::connect(&projectHost, &ui::ProjectHost::sessionReplaced, [&] { replaced = true; });
    QObject::connect(&projectHost, &ui::ProjectHost::openFinished,
                     [&](const ui::ProjectHostOperationOutcome outcome, const QString& message) {
                         openOutcome = outcome;
                         openMessage = message;
                     });
    projectHost.setUnsavedChangeDecisionProvider([] { return ui::UnsavedChangeDecision::Discard; });
    projectHost.setOpenPathProvider([&] { return std::optional(projectPath); });
    projectHost.requestOpen();
    if (!waitUntil([&] { return replaced || openOutcome.has_value(); }, 300s) || !replaced) {
        std::cout << "BENCH_ERROR open " << projectPath << " failed: "
                  << openMessage.toStdString() << '\n';
        return 2;
    }
    auto [document, commandStack] = projectHost.liveDocumentAndStack();
    if (document == nullptr || commandStack == nullptr) {
        std::cout << "BENCH_ERROR project opened read-only\n";
        return 2;
    }
    ui::CompositionSession session(*document, *commandStack, projectHost.lowestCompositionId());
    if (const auto* color = projectHost.colorSettings(); color != nullptr)
        session.setColorSettings(*color);

    runtime::NodeDefinitionRegistry definitions;
    if (!runtime::registerBuiltInNodeDefinitions(definitions))
        return 2;
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    runtime::CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(projectPath.parent_path());
    const QSettings playbackSettings;
    const auto cacheBudgets = ui::cacheMemoryBudgetsFromSettings(playbackSettings);
    evaluator.setVideoCacheByteBudget(
        media::video::automaticDecodedVideoCacheBytes(runtime::physicalMemoryBytes()));
    evaluator.operationCache()->setByteBudget(cacheBudgets.operationCacheByteBudget);
    runtime::CpuReferenceDisplayPreparer referenceDisplayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProvider;
    const auto retirementOwner = render::PackedHostRetirementOwner::create();
    PackedHostTestTeardown retirementTeardown(retirementOwner);
    ui::TaskUiBridge bridge(scheduler);
    const auto policy =
        std::make_shared<runtime::GpuProcessingPhasePolicy>(32, retirementOwner, taskBudgetView);
    ui::MemoryBudgetController memoryController(
        scheduler, bridge, runtime::processMemoryBudgetLedger(),
        [policy](const runtime::CancellationToken& cancellation) {
            return policy->sampleHostMemory(cancellation);
        });
    QObject::connect(&memoryController, &ui::MemoryBudgetController::sampleApplied, &bridge,
                     [policy, &playbackSettings] {
                         auto& ledger = runtime::processMemoryBudgetLedger();
                         const auto allocation =
                             ui::cacheMemoryBudgetAllocationFromSettings(playbackSettings, ledger);
                         policy->applyHostSample(ledger.state(), ledger.physicalByteBudget(),
                                                 allocation);
                     });
    ui::QualifiedDisplayProcessorBootstrap bootstrap(scheduler, bridge, qualifiedProvider);
    auto frameCache =
        std::make_shared<ui::PreviewFrameCache>(cacheBudgets.previewFrameCacheByteBudget);
    bool cacheCapacityInitialized = false;
    QObject::connect(&memoryController, &ui::MemoryBudgetController::sampleApplied, &bridge, [&] {
        if (cacheCapacityInitialized ||
            runtime::processMemoryBudgetLedger().physicalByteBudget() == 0)
            return;
        cacheCapacityInitialized = true;
        const QSettings current;
        static_cast<void>(ui::applyCacheMemoryBudgetsFromSettings(
            current, *evaluator.operationCache(), *frameCache));
    });
    auto planCache = std::make_shared<ui::CompiledPlanCache>();
    const auto pipeline = ui::makeCompositionPreviewPipeline(
        compiler, evaluator, referenceDisplayPreparer, qualifiedProvider, planCache);
    runtime::GpuPreviewDisplayServiceOptions serviceOptionsApp;
    serviceOptionsApp.enabled = true;
    serviceOptionsApp.loaderPath = options.loader_path;
    serviceOptionsApp.product = runtime::GpuPreviewDisplayProductMode::GpuPreparedPacked;
    serviceOptionsApp.presentation = runtime::GpuPreviewDisplayServicePresentationMode::Disabled;
    const auto residentPlan = ui::gpuResidentBudgetPlanFor(cacheBudgets.previewFrameCacheByteBudget);
    ui::applyGpuResidentBudgetPlan(serviceOptionsApp, residentPlan);
    serviceOptionsApp.previewByteAllowance.reset();
    ui::applyGpuResidentCacheLimits(*frameCache, residentPlan);
    auto coverageCache = std::make_shared<runtime::GpuSceneCoverageCache>();
    auto preparedCache = std::make_shared<runtime::GpuPreparedUploadCache>(
        evaluator.operationCache(), retirementOwner);
    std::shared_ptr<runtime::GpuOcioContextResolver> resolver =
        host::makePackagedGpuOcioResolver(QCoreApplication::applicationFilePath().toStdString());
    if (resolver == nullptr)
        resolver = std::make_shared<runtime::GpuOcioContextResolver>();
    const auto deviceWorkingBytes = (512ULL << 20) + serviceOptionsApp.packedTransferByteAllowance;
    auto gpuStage = ui::makeSessionRefreshingGpuSceneStage(
        compiler, evaluator, qualifiedProvider, coverageCache, preparedCache, planCache, resolver,
        {.policy = policy,
         .envelope = {.metadataBytes = 16ULL << 20,
                      .retainedCoverageByteLimit = 16ULL << 20,
                      .requestedExecutorBytes = std::nullopt,
                      .deviceWorkingBytes = deviceWorkingBytes}});
    serviceOptionsApp.processingPhasePolicy = policy;
    serviceOptionsApp.cpuStoragePlanner =
        ui::makeCompositionPreviewCpuStoragePlanner(compiler, evaluator, planCache);
    auto serviceOwner = std::make_unique<runtime::GpuPreviewDisplayService>(
        scheduler, std::move(gpuStage),
        ui::makeCompositionPreviewCpuStage(compiler, evaluator, qualifiedProvider, planCache),
        ui::makeCompositionPreviewCpuDisplayFallback(referenceDisplayPreparer), serviceOptionsApp);
    auto& service = *serviceOwner;
    const ui::PreviewPreparationSubmitter submitter =
        [&service](runtime::TaskRequest request, const document::Snapshot& snapshot,
                   const runtime::PreviewRequestIdentity& identity, const std::size_t bytes,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides) {
            return service.submit(std::move(request), snapshot, identity, bytes, overrides);
        };
    ui::CompositionPreviewSettings settings;
    settings.colorIntent = session.colorIntent();
    settings.resolutionPolicy = gQuarter ? runtime::PreviewResolutionPolicy::Quarter
                                : half   ? runtime::PreviewResolutionPolicy::Half
                                         : runtime::PreviewResolutionPolicy::Full;
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline, settings,
                                                frameCache, nullptr, submitter, nullptr, policy);
    QObject::connect(&memoryController, &ui::MemoryBudgetController::sampleApplied, &controller,
                     &ui::CompositionPreviewController::notifyProcessingCapacityChanged);
    const auto widthProvider = [policy, deviceWorkingBytes](const std::uint64_t pixels) {
        return runtime::previewFramesThatFit(*policy, pixels, deviceWorkingBytes);
    };
    // Read-ahead also fills while idle, so only the playback measurement has it: a fill run
    // measures the RAM preview alone. The workspace mode always has it, as the application does.
    std::optional<ui::BackgroundPreviewController> background;
    if (gBenchPlayback || gBenchWorkspace) {
        background.emplace(session, controller, scheduler, bridge, pipeline, nullptr, submitter);
        background->setReadAheadWidthProvider(widthProvider);
    }
    ui::RamPreviewController ram(session, controller, scheduler, bridge, pipeline, nullptr,
                                 submitter);
    ram.setFillWidthProvider(widthProvider);
    // The workspace mode drives the controller's own transport, as the application does.
    std::optional<ui::PlaybackController> ownPlayback;
    if (!gBenchWorkspace)
        ownPlayback.emplace(session, controller);
    ui::PlaybackController& playback =
        gBenchWorkspace ? controller.playbackController() : *ownPlayback;
    const auto firstStarted = std::chrono::steady_clock::now();
    std::optional<ui::ViewerEditor> ownViewer;
    std::optional<ui::AssetController> assets;
    std::optional<ui::AudioPlaybackSession> audio;
    std::optional<scripting::LiveSessionHost> liveHost;
    std::optional<ui::LiveSessionAdapter> liveAdapter;
    std::optional<ui::EditorRegistry> registry;
    std::unique_ptr<ui::MainWindow> window;
    if (gBenchWorkspace) {
        // The application-lifetime controllers that follow session time (apps/bloom/main.cpp).
        if (gBenchWorkspaceAssets)
            assets.emplace(session, projectHost, scheduler, bridge, nullptr, nullptr,
                           ui::AudioProcessingAdmission{.policy = policy});
        audio.emplace(session, scheduler, bridge, compiler, planCache, evaluator);
        if (assets)
            audio->setAudioBufferSource(*assets);
        liveHost.emplace();
        liveAdapter.emplace(*liveHost, session, projectHost);
        // The application's window, default workspace and every foundation editor (Timeline,
        // Properties, Assets, ...), so per-tick work in panels is measured too. Offscreen, so the
        // Viewer presents through its non-surface path.
        registry.emplace();
        if (!ui::registerFoundationEditors(*registry, session, controller, &ram, &projectHost)) {
            std::cout << "BENCH_ERROR editors could not be registered\n";
            return 2;
        }
        window = std::make_unique<ui::MainWindow>(*registry, session, projectHost, &ram,
                                                  &controller, nullptr, &playback);
        window->resize(1920, 1080);
        window->show();
        static_cast<void>(waitUntil([&] { return window->displayedViewer() != nullptr; }, 30s));
        if (window->displayedViewer() == nullptr) {
            std::cout << "BENCH_ERROR the workspace has no Viewer\n";
            return 2;
        }
    } else {
        ownViewer.emplace(session, controller);
        ownViewer->resize(960, 540);
        ownViewer->show();
    }
    ui::ViewerEditor& viewer = gBenchWorkspace ? *window->displayedViewer() : *ownViewer;

    const auto shutdown = [&] {
        if (audio)
            audio->beginShutdown();
        if (assets)
            assets->setCachePurgeGate(true);
        ram.beginShutdown();
        if (background)
            background->beginShutdown();
        controller.beginShutdown();
        memoryController.beginShutdown();
        bridge.beginShutdown();
        service.beginShutdown();
        const bool stopped = waitUntil(
            [&] {
                return scheduler.isQuiescent() &&
                       service.status().state == runtime::GpuPreviewDisplayServiceState::Stopped;
            },
            30s);
        viewer.releasePackedFramesForShutdown();
        if (window) {
            window->completeShutdown();
            window.reset();
        }
        controller.releasePublishedFramesForShutdown();
        frameCache->clear();
        evaluator.operationCache()->closePreparedUploads();
        retirementOwner->beginShutdown();
        const bool retired = waitUntil([&] { return retirementOwner->exitAcknowledged(); }, 15s);
        if (!stopped || !retired) {
            std::cout << "BENCH_WARNING shutdown stopped=" << stopped << " retired=" << retired
                      << '\n'
                      << std::flush;
            std::_Exit(0); // the measurement is already reported
        }
    };

    if (!waitUntil(
            [&] {
                return service.status().state !=
                       runtime::GpuPreviewDisplayServiceState::Initializing;
            },
            90s) ||
        service.status().state != runtime::GpuPreviewDisplayServiceState::Ready) {
        std::cout << "Unsupported: " << service.status().residentDetail << '\n';
        shutdown();
        return options.require_device ? 1 : 77;
    }
    static_cast<void>(waitUntil([&] { return policy->snapshot().reservations.hostReady; }, 30s));
    const bool firstPainted = waitUntil(
        [&] { return viewer.displayedPreviewState().freshness == ui::FrameFreshness::Current; },
        300s);
    const auto firstFrameSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - firstStarted).count();
    const auto firstFrame = viewer.displayedFrameForTest();
    const int firstProvider =
        firstFrame ? static_cast<int>(firstFrame->provenance().provider) : -1;
    std::ostringstream json;
    json << std::setprecision(6) << "{\"project\":" << benchJson(projectPath.string())
         << ",\"mode\":\"" << (gBenchPlayback ? "playback" : "fill") << "\""
         << ",\"resolution\":\"" << resolution << "\""
         << ",\"workspace\":" << (gBenchWorkspace ? "true" : "false")
         << ",\"workspaceAssets\":" << (gBenchWorkspace && gBenchWorkspaceAssets ? "true" : "false")
         << ",\"firstFramePainted\":" << (firstPainted ? "true" : "false")
         << ",\"firstFrameSeconds\":" << firstFrameSeconds
         << ",\"firstFrameProvider\":" << firstProvider
         << ",\"previewCacheBudgetBytes\":" << frameCache->byteBudget();
    const auto before = service.status().counters;
    const auto cpuBefore = benchProcessCpuSeconds();
    const auto started = std::chrono::steady_clock::now();
    json << std::setprecision(15) << ",\"windowStartEpoch\":" << benchEpochSeconds()
         << std::setprecision(6);
    if (gBenchPlayback) {
        const auto played = benchPlay(session, playback, viewer);
        json << ",\"playSeconds\":" << played.seconds
             << ",\"transportFrames\":" << played.transportFrames
             << ",\"paintedFrames\":" << played.paintedFrames
             << ",\"paintedChanges\":" << played.paintedChanges
             << ",\"playUiBusySeconds\":" << played.uiBusySeconds
             << ",\"playMaxSliceMs\":" << played.maxSliceMs
             << ",\"playLongSlices\":" << played.longSlices << std::setprecision(15)
             << ",\"windowEndEpoch\":" << benchEpochSeconds() << std::setprecision(6)
             << ",\"playbackFps\":"
             << (played.seconds > 0 ? static_cast<double>(played.paintedFrames) / played.seconds
                                    : 0.0);
    } else {
        ram.start();
        const bool finished = waitUntil([&] { return !ram.isCaching(); }, gBenchTimeout);
        const auto wall =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const auto cpu = benchProcessCpuSeconds() - cpuBefore;
        const auto completion = ram.lastCompletion();
        json << ",\"finished\":" << (finished ? "true" : "false") << ",\"completion\":"
             << (completion ? static_cast<int>(*completion) : -1)
             << ",\"completionComplete\":"
             << (completion == ui::RamPreviewController::Completion::Complete ? "true" : "false")
             << ",\"totalFrames\":" << ram.totalFrameCount()
             << ",\"preparedFrames\":" << ram.preparedFrameCount()
             << ",\"cachedFrames\":" << ram.cachedFrameCount()
             << ",\"fillSeconds\":" << wall << ",\"fillFps\":"
             << (wall > 0 ? static_cast<double>(ram.preparedFrameCount()) / wall : 0.0)
             << ",\"fillCpuSeconds\":" << cpu
             << ",\"fillCoresBusy\":" << (wall > 0 ? cpu / wall : 0.0)
             << ",\"peakInFlight\":" << ram.peakInFlightFrames()
             << ",\"cacheResidentBytes\":" << frameCache->residentBytes() << std::setprecision(15)
             << ",\"windowEndEpoch\":" << benchEpochSeconds() << std::setprecision(6);
        if (finished && ram.canAutoPlay() && gBenchPlaySeconds > 0) {
            const auto played = benchPlay(session, playback, viewer);
            json << ",\"cachedPlaySeconds\":" << played.seconds
                 << ",\"cachedTransportFrames\":" << played.transportFrames
                 << ",\"cachedPaintedFrames\":" << played.paintedFrames
                 << ",\"cachedPaintedChanges\":" << played.paintedChanges
                 << ",\"cachedPresentedFps\":"
                 << (played.seconds > 0
                         ? static_cast<double>(played.paintedChanges) / played.seconds
                         : 0.0)
                 << ",\"cachedPlayUiBusySeconds\":" << played.uiBusySeconds
                 << ",\"cachedPlayMaxSliceMs\":" << played.maxSliceMs
                 << ",\"cachedPlayLongSlices\":" << played.longSlices << ",\"cachedPlaybackFps\":"
                 << (played.seconds > 0
                         ? static_cast<double>(played.paintedFrames) / played.seconds
                         : 0.0);
        }
    }
    const auto after = service.status().counters;
    const auto claims = policy->snapshot().reservations;
    json << ",\"cpuFallbackFrames\":" << after.cpuFallbacks - before.cpuFallbacks
         << ",\"cpuFallbacksTotal\":" << after.cpuFallbacks
         << ",\"nativeDispatches\":" << after.nativeDispatches - before.nativeDispatches
         << ",\"packedTransfers\":" << after.packedTransfers - before.packedTransfers
         << ",\"residentGraphJobs\":" << after.residentGraphJobs - before.residentGraphJobs
         << ",\"uploadSubmitSeconds\":"
         << 1e-9 * static_cast<double>(after.uploadSubmitNanoseconds - before.uploadSubmitNanoseconds)
         << ",\"uploadFenceSeconds\":"
         << 1e-9 * static_cast<double>(after.uploadFenceObservationNanoseconds -
                                       before.uploadFenceObservationNanoseconds)
         << ",\"processingSubmitSeconds\":"
         << 1e-9 * static_cast<double>(after.processingSubmitNanoseconds -
                                       before.processingSubmitNanoseconds)
         << ",\"processingFenceSeconds\":"
         << 1e-9 * static_cast<double>(after.processingFenceObservationNanoseconds -
                                       before.processingFenceObservationNanoseconds)
         << ",\"packedTransferSeconds\":"
         << 1e-9 * static_cast<double>(after.packedTransferNanoseconds -
                                       before.packedTransferNanoseconds)
         << ",\"admissionRefusals\":" << after.gpuAdmissionRefusals - before.gpuAdmissionRefusals
         << ",\"residentDetail\":" << benchJson(service.status().residentDetail)
         << ",\"hostCapacityBytes\":" << claims.hostCapacityBytes.value_or(0)
         << ",\"deviceCapacityBytes\":" << claims.deviceCapacityBytes.value_or(0)
         << ",\"peakRssBytes\":" << benchPeakRssBytes() << ",\"processSeconds\":"
         << std::chrono::duration<double>(std::chrono::steady_clock::now() - processStarted)
                .count()
         << '}';
    std::cout << "BENCH_RESULT " << json.str() << '\n' << std::flush;
    shutdown();
    return 0;
}
} // namespace
