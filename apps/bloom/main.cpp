#include "audio_device_process_teardown.hpp"
#include "deferred_editor_teardown.hpp"
#include "packed_host_process_teardown.hpp"
#include "sequence_export_process_teardown.hpp"

#include <algorithm>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/gpu_export_tool_package.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/media/cache/media_disk_cache.hpp>
#include <bloom/media/video/session.hpp>
#include <bloom/render/packed_host_retirement.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>
#include <bloom/runtime/gpu_prepared_upload_cache.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/acceleration_status.hpp>
#include <bloom/ui/application_shutdown_coordinator.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/audio_device_control.hpp>
#include <bloom/ui/audio_playback_session.hpp>
#include <bloom/ui/background_preview_controller.hpp>
#include <bloom/ui/cache_purge_controller.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_gpu_scene_stage.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/composition_thumbnail_controller.hpp>
#include <bloom/ui/cryptomatte_controller.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/frame_export_controller.hpp>
#include <bloom/ui/gpu_viewer_bootstrap.hpp>
#include <bloom/ui/h264_encoder_service.hpp>
#include <bloom/ui/jobs_editor.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/media_disk_cache_settings.hpp>
#include <bloom/ui/memory_budget_controller.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/preview_frame_trace.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/qualified_display_processor_bootstrap.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/interaction_sounds.hpp>
#include <bloom/ui/system_notifier.hpp>
#include <bloom/ui/ui_feedback.hpp>
#include <bloom/ui/ui_sound_engine.hpp>
#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/task_monitor_model.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <bloom/ui/window_status_bar.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QTimer>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>

int main(int argc, char* argv[]) try {
    QApplication application(argc, argv);

    QCoreApplication::setApplicationName("Bloom");
    QCoreApplication::setApplicationVersion("0.1.0");
    QCoreApplication::setOrganizationName("Kinetik");

    // The Kinetik visual language is installed once, application-wide, before any widget exists
    // (task U1, issue #117). It replaces MainWindow::applyFoundationTheme(): the palette, the
    // application stylesheet, and the interface font belong to the application, not to one window,
    // and installing them first means no surface is ever constructed under the default Qt theme.
    bloom::ui::kit::installKinetikTheme(application);
    // The theme installs kit::AltUnderlineProxyStyle (Alt-held mnemonic underlines, menu and
    // item-view painting) itself, so every window built after this line -- and every test window
    // built under the same theme -- shares one style.
    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks) -- Qt parent ownership below.

    // ProjectHost (task U1, issue #72) replaces the hand-rolled document/command-stack pair: it
    // owns the application's single live bloom::host::ProjectSession and constructs an initial
    // createNew() project itself. taskScheduler is shared with CompositionPreviewController below
    // (both need a TaskExecutor::BlockingIo-capable scheduler; TaskSchedulerConfig::defaults()
    // already provisions one BlockingIo worker, so a single scheduler instance serves both).
    bloom::runtime::processMemoryBudgetLedger().beginProcessSampling();
    const auto taskBudgetView = std::make_shared<bloom::runtime::TaskBudgetView>();
    auto taskConfig = bloom::runtime::TaskSchedulerConfig::defaults();
    taskConfig.budgetView = taskBudgetView;
    bloom::runtime::TaskScheduler taskScheduler(taskConfig);
    bloom::ui::ProjectHost projectHost(taskScheduler);

    auto [initialDocument, initialCommandStack] = projectHost.liveDocumentAndStack();
    if (initialDocument == nullptr || initialCommandStack == nullptr) {
        return 1;
    }
    bloom::ui::CompositionSession compositionSession(*initialDocument, *initialCommandStack,
                                                     projectHost.lowestCompositionId());
    if (const auto* settings = projectHost.colorSettings(); settings != nullptr) {
        compositionSession.setColorSettings(*settings);
    }

    // Projection rebinding (decision 2): every time ProjectHost replaces the live session content
    // (New or a successful Open install), rebind CompositionSession to whatever document/command-
    // stack pair is now live. A preserved-read-only install has no document/command-stack at all
    // (liveDocumentAndStack() returns null), so this lambda intentionally skips rebinding and
    // leaves CompositionSession bound to whatever it projected before. That skip is now
    // intentional and safe rather than a known limitation (task R1, issue #74): MainWindow hides
    // the entire editor workspace behind a presentation-level read-only placeholder page whenever
    // ProjectHost's content kind is PreservedReadOnly (see MainWindow::updateContentSurface()), so
    // the stale CompositionSession this lambda leaves bound is never shown to the artist. Returning
    // to decoded content (a New or an editable Open) switches the workspace back into view and this
    // lambda rebinds normally.
    QObject::connect(&projectHost, &bloom::ui::ProjectHost::colorSettingsChanged,
                     &compositionSession, [&projectHost, &compositionSession] {
                         if (const auto* settings = projectHost.colorSettings();
                             settings != nullptr)
                             compositionSession.setColorSettings(*settings);
                     });

    bloom::runtime::NodeDefinitionRegistry nodeDefinitions;
    if (!bloom::runtime::registerBuiltInNodeDefinitions(nodeDefinitions)) {
        return 1;
    }
    nodeDefinitions.freeze();
    bloom::runtime::SnapshotCompiler snapshotCompiler(nodeDefinitions);
    bloom::runtime::CpuCompositionEvaluator cpuEvaluator;
    QObject::connect(
        &projectHost, &bloom::ui::ProjectHost::sessionReplaced, &compositionSession,
        [&projectHost, &compositionSession, &cpuEvaluator] {
            const auto path = projectHost.displayPath();
            cpuEvaluator.setAssetBaseDirectory(path ? path->parent_path()
                                                    : std::filesystem::path{});
            // Invalidate source authority before rebind emits any new preview request. Even an
            // identical reopened document must verify its sources in the new session. Rooted
            // tickets/sessions retire on their worker; this boundary performs no source I/O.
            cpuEvaluator.operationCache()->invalidateSourceRevisions();
            auto [document, commandStack] = projectHost.liveDocumentAndStack();
            if (document == nullptr || commandStack == nullptr) {
                return;
            }
            compositionSession.rebind(*document, *commandStack, projectHost.lowestCompositionId());
            if (const auto* settings = projectHost.colorSettings(); settings != nullptr) {
                compositionSession.setColorSettings(*settings);
            }
        });
    const QSettings playbackSettings;
    const auto cacheBudgets = bloom::ui::cacheMemoryBudgetsFromSettings(playbackSettings);
    // Decoded-video retention: `playback/decoded-video-cache-bytes` selects an exact target; a
    // missing, zero or unreadable value selects the automatic share of physical memory. The
    // cache is a reusable pool on the process memory ledger, so observed OS limits still apply.
    const auto decodedVideoCacheBytes =
        playbackSettings.value(QStringLiteral("playback/decoded-video-cache-bytes"), 0)
            .toULongLong();
    cpuEvaluator.setVideoCacheByteBudget(decodedVideoCacheBytes != 0
                                             ? static_cast<std::size_t>(decodedVideoCacheBytes)
                                             : bloom::media::video::automaticDecodedVideoCacheBytes(
                                                   bloom::runtime::physicalMemoryBytes()));
    cpuEvaluator.operationCache()->setByteBudget(cacheBudgets.operationCacheByteBudget);
    // CACHE-2 (docs/architecture/media-io.md "Disk cache"): one instance shared by the evaluator
    // and the Asset Controller's proxy/thumbnail decodes below -- "the same store". Built from
    // settings alongside the operation cache's own settings-derived budget just above; null when
    // disabled in settings or no cache directory can be resolved at all (never a startup failure).
    const std::shared_ptr<bloom::media::cache::MediaDiskCache> mediaDiskCache = [] {
        const QSettings mediaDiskCacheSettings;
        return bloom::ui::makeMediaDiskCacheFromSettings(mediaDiskCacheSettings);
    }();
    cpuEvaluator.setMediaDiskCache(mediaDiskCache);
    bloom::runtime::CpuReferenceDisplayPreparer referenceDisplayPreparer;
    // Issue #97 (task C3): resolved and built once, on the shared TaskScheduler's blocking-I/O
    // lane, at this same session/pipeline-construction point (design decision 3). Declared before
    // taskUiBridge/qualifiedDisplayProcessorBootstrap/previewController so it outlives every
    // reference into it those objects hold (locals destruct in reverse declaration order).
    bloom::runtime::QualifiedDisplayProcessorProvider qualifiedDisplayProcessorProvider;
    const auto packedHostRetirement = bloom::render::PackedHostRetirementOwner::create();
    bloom::app::PackedHostProcessTeardown packedHostTeardown(taskScheduler, packedHostRetirement,
                                                             cpuEvaluator.operationCache());
    const auto sequenceExportRetirement = bloom::host::SequenceExportRetirementOwnerV1::create();
    bloom::app::SequenceExportProcessTeardown sequenceExportTeardown(taskScheduler,
                                                                     sequenceExportRetirement);
    std::unique_ptr<bloom::ui::TaskUiBridge> taskUiBridgeOwner;
    try {
        taskUiBridgeOwner = std::make_unique<bloom::ui::TaskUiBridge>(taskScheduler);
    } catch (const std::exception& error) {
        std::cerr << "Bloom could not initialize task completion delivery: " << error.what()
                  << ". Close other applications and retry.\n";
        taskScheduler.beginShutdown();
        return 1;
    }
    auto& taskUiBridge = *taskUiBridgeOwner;
    bloom::ui::AudioDeviceControl audioDeviceControl(
        taskScheduler, taskUiBridge,
        std::make_unique<bloom::media::audio::playback::AudioEngine>(
            bloom::media::audio::playback::makeMiniaudioBackend()));
    bloom::app::AudioDeviceProcessTeardown audioDeviceTeardown(audioDeviceControl);
    const auto processingPhasePolicy = std::make_shared<bloom::runtime::GpuProcessingPhasePolicy>(
        32, packedHostRetirement, taskBudgetView);
    bloom::ui::MemoryBudgetController memoryBudgetController(
        taskScheduler, taskUiBridge, bloom::runtime::processMemoryBudgetLedger(),
        [processingPhasePolicy](const bloom::runtime::CancellationToken& cancellation) {
            return processingPhasePolicy->sampleHostMemory(cancellation);
        });
    QObject::connect(
        &memoryBudgetController, &bloom::ui::MemoryBudgetController::sampleApplied, &application,
        [processingPhasePolicy, &playbackSettings] {
            auto& ledger = bloom::runtime::processMemoryBudgetLedger();
            const auto physicalBytes = ledger.physicalByteBudget();
            const auto allocation =
                bloom::ui::cacheMemoryBudgetAllocationFromSettings(playbackSettings, ledger);
            const auto delivered = ledger.state();
            processingPhasePolicy->applyHostSample(delivered, physicalBytes, allocation);
        });
    // Explicit opt-in, one bounded numeric-only buffer. The path is only an export destination;
    // it and media locators never enter trace records. No formatting or I/O runs in UI callbacks.
    const QString previewTraceDestination = qEnvironmentVariable("BLOOM_PREVIEW_TRACE");
    const auto previewTrace = previewTraceDestination.isEmpty()
                                  ? nullptr
                                  : std::make_shared<bloom::ui::PreviewFrameTrace>();
    bloom::ui::AssetController assetController(compositionSession, projectHost, taskScheduler,
                                               taskUiBridge, mediaDiskCache.get(), nullptr,
                                               {.policy = processingPhasePolicy});
    QObject::connect(&memoryBudgetController, &bloom::ui::MemoryBudgetController::sampleApplied,
                     &assetController, &bloom::ui::AssetController::retryAudioAfterCapacitySample);
    const auto updateMediaDirectory = [&] {
        const auto path = projectHost.displayPath();
        cpuEvaluator.setAssetBaseDirectory(path ? path->parent_path() : std::filesystem::path{});
    };
    QObject::connect(&projectHost, &bloom::ui::ProjectHost::saveFinished, &assetController,
                     updateMediaDirectory);
    updateMediaDirectory();
    bloom::ui::QualifiedDisplayProcessorBootstrap qualifiedDisplayProcessorBootstrap(
        taskScheduler, taskUiBridge, qualifiedDisplayProcessorProvider);
    // The RAM preview cache and the preview pipeline are built HERE, once, because two surfaces
    // share each of them: the preview controller and the RAM preview controller both render through
    // the one pipeline (and so through the one compiled-plan cache inside it), and both put frames
    // into the one frame cache. The cache's budget is the artist's setting (task PERF1, item 2);
    // `settings` is declared below for window state, so this read opens its own short-lived
    // QSettings over the same organization/application keys rather than moving that declaration up
    // here.
    auto previewFrameCache =
        std::make_shared<bloom::ui::PreviewFrameCache>(cacheBudgets.previewFrameCacheByteBudget);
    // Resolve Automatic after the first worker-delivered physical capacity. Later preference
    // changes use the same helper immediately; free-memory fluctuations do not rewrite targets.
    const auto applyMemoryPreferences = [&] {
        const QSettings currentSettings;
        return bloom::ui::applyCacheMemoryBudgetsFromSettings(
            currentSettings, *cpuEvaluator.operationCache(), *previewFrameCache);
    };
    bool cacheCapacityInitialized = false;
    QObject::connect(&memoryBudgetController, &bloom::ui::MemoryBudgetController::sampleApplied,
                     &application, [&] {
                         if (cacheCapacityInitialized ||
                             bloom::runtime::processMemoryBudgetLedger().physicalByteBudget() == 0)
                             return;
                         cacheCapacityInitialized = true;
                         static_cast<void>(applyMemoryPreferences());
                     });
    // One compiled-plan cache for every consumer of the live revision: the preview surfaces below
    // and the audio mix, which derives from the document rather than from a shown frame.
    auto compiledPlanCache = std::make_shared<bloom::ui::CompiledPlanCache>();
    const auto previewPipeline = bloom::ui::makeCompositionPreviewPipeline(
        snapshotCompiler, cpuEvaluator, referenceDisplayPreparer, qualifiedDisplayProcessorProvider,
        compiledPlanCache);
    // The GPU display half of the preview pipeline. It owns a dedicated service thread and never
    // exposes a native or Vulkan handle. It is constructed here, after the compiler/evaluator/
    // qualified provider/frame cache it depends on and before the controllers that submit through
    // it, so every dependency outlives it and it outlives every controller that captures its
    // submitter. When no bundled native loader was packaged, `enabled` stays false and the service
    // never touches a device: every request takes the unchanged CPU stage + display-fallback path.
    bloom::runtime::GpuPreviewDisplayServiceOptions gpuPreviewDisplayOptions;
    bool bundledNativeLoader = false;
#ifdef BLOOM_BUNDLED_VULKAN_LOADER
    bundledNativeLoader = true;
    gpuPreviewDisplayOptions.enabled = true;
    gpuPreviewDisplayOptions.loaderPath =
        std::filesystem::path(QDir(QCoreApplication::applicationDirPath())
                                  .filePath(QStringLiteral(BLOOM_BUNDLED_VULKAN_LOADER_SUBDIR
                                                           "/" BLOOM_BUNDLED_VULKAN_LOADER_NAME))
                                  .toStdString());
#endif
    // Ordinary widget composition provides panel/window clipping. Scene/display preparation
    // remains genuinely GPU; one asynchronous final transfer supplies immutable painted pixels.
    // The independently qualified NativeResident route remains available to its required fixtures.
    gpuPreviewDisplayOptions.product =
        bloom::runtime::GpuPreviewDisplayProductMode::GpuPreparedPacked;
    gpuPreviewDisplayOptions.presentation =
        bloom::runtime::GpuPreviewDisplayServicePresentationMode::Disabled;
    // Align the resident lease registry with the artist's UI frame-cache budget so the registry can
    // publish every lease the cache can reference, with bounded in-flight/visible headroom and a
    // hard VRAM ceiling. No live lease/pin is ever invalidated; pressure takes the CPU fallback.
    const auto gpuResidentBudgetPlan =
        bloom::ui::gpuResidentBudgetPlanFor(cacheBudgets.previewFrameCacheByteBudget);
    bloom::ui::applyGpuResidentBudgetPlan(gpuPreviewDisplayOptions, gpuResidentBudgetPlan);
    // Omit the request override: the shared delivered root view, not this cold cache partition,
    // selects each request. The native owner separately resolves its finite physical pool.
    gpuPreviewDisplayOptions.previewByteAllowance.reset();
    // The shared frame cache gets the matching additive GPU-resident sublimits. Its overall CPU
    // cache budget is unchanged; only the bounded resident subset is capped, and it evicts LRU
    // resident entries (dropping the cache's reference only) before the registry can refuse a
    // publishable lease. No live lease is invalidated.
    bloom::ui::applyGpuResidentCacheLimits(*previewFrameCache, gpuResidentBudgetPlan);
    // The coverage cache and the prepared-upload cache are shared across every GPU-scene request.
    // The media context is NOT captured once here: makeSessionRefreshingGpuSceneStage copies the
    // evaluator's current assetBaseDirectory per request, so a relative media path follows an
    // Open/SaveAs that changes the session base directory.
    auto gpuSceneCoverageCache = std::make_shared<bloom::runtime::GpuSceneCoverageCache>();
    auto gpuPreparedUploadCache = std::make_shared<bloom::runtime::GpuPreparedUploadCache>(
        cpuEvaluator.operationCache(), packedHostRetirement);
    // The ONE shared runtime OCIO context resolver. It is INERT to construct (no hash/fs/OCIO/
    // process work); the packaged glslangValidator/spirv-val paths come from the application's OWN
    // packaging macros (never PATH, never a manual setup) and are qualified lazily on the GPU-scene
    // CPU worker. The same resolver supplies the builder's effect/media/ACES context AND the
    // general display program's preparer, so there is exactly one tool qualification and one
    // program cache.
    // Compose the ONE shared resolver from this target's packaged tools via the shared root helper,
    // exactly like bloom-cli/bloom-mcp: non-relocated pins staged digests, relocated uses
    // inventory.
    std::shared_ptr<bloom::runtime::GpuOcioContextResolver> gpuOcioContextResolver =
        bloom::host::makePackagedGpuOcioResolver(
            std::filesystem::path(QCoreApplication::applicationFilePath().toStdString()));
    if (gpuOcioContextResolver == nullptr) {
        // No packaged tools: keep the inert fail-closed resolver so every affected request takes
        // the CPU path, exactly as before.
        gpuOcioContextResolver = std::make_shared<bloom::runtime::GpuOcioContextResolver>();
    }
    auto gpuPreviewGpuSceneStage = bloom::ui::makeSessionRefreshingGpuSceneStage(
        snapshotCompiler, cpuEvaluator, qualifiedDisplayProcessorProvider, gpuSceneCoverageCache,
        gpuPreparedUploadCache, compiledPlanCache, gpuOcioContextResolver,
        {.policy = processingPhasePolicy,
         .envelope = {.metadataBytes = 16ULL * 1024 * 1024,
                      .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                      .requestedExecutorBytes = std::nullopt,
                      // The general OCIO arm's 512 MiB ceiling plus concurrent transfer storage.
                      .deviceWorkingBytes = 512ULL * 1024 * 1024 +
                                            gpuPreviewDisplayOptions.packedTransferByteAllowance}});
    auto gpuPreviewCpuStage = bloom::ui::makeCompositionPreviewCpuStage(
        snapshotCompiler, cpuEvaluator, qualifiedDisplayProcessorProvider, compiledPlanCache);
    auto gpuPreviewCpuDisplayFallback =
        bloom::ui::makeCompositionPreviewCpuDisplayFallback(referenceDisplayPreparer);
    gpuPreviewDisplayOptions.processingPhasePolicy = processingPhasePolicy;
    gpuPreviewDisplayOptions.cpuStoragePlanner = bloom::ui::makeCompositionPreviewCpuStoragePlanner(
        snapshotCompiler, cpuEvaluator, compiledPlanCache);
    bloom::runtime::GpuPreviewDisplayService gpuPreviewDisplayService(
        taskScheduler, std::move(gpuPreviewGpuSceneStage), std::move(gpuPreviewCpuStage),
        std::move(gpuPreviewCpuDisplayFallback), gpuPreviewDisplayOptions);
    // The one submit seam: each controller hands the request it would otherwise submit to the
    // scheduler straight to the service, which owns GPU submission and the CPU fallback. The
    // controller's own preparation function stays live for viewer analysis and probes only.
    bloom::ui::PreviewPreparationSubmitter gpuPreviewDisplaySubmitter =
        [&gpuPreviewDisplayService](
            bloom::runtime::TaskRequest request, const bloom::document::Snapshot& snapshot,
            const bloom::runtime::PreviewRequestIdentity& identity,
            const std::size_t pixelStorageByteLimit,
            const std::vector<bloom::runtime::SnapshotParameterOverride>& overrides) {
            return gpuPreviewDisplayService.submit(std::move(request), snapshot, identity,
                                                   pixelStorageByteLimit, overrides);
        };
    bloom::ui::CompositionPreviewController previewController(
        compositionSession, taskScheduler, taskUiBridge, previewPipeline,
        {.colorIntent = compositionSession.colorIntent(),
         .displayName = {},
         .viewName = {},
         .showLook = true},
        previewFrameCache, nullptr, gpuPreviewDisplaySubmitter, previewTrace,
        processingPhasePolicy);
    QObject::connect(&memoryBudgetController, &bloom::ui::MemoryBudgetController::sampleApplied,
                     &previewController,
                     &bloom::ui::CompositionPreviewController::notifyProcessingCapacityChanged);
    // ADR 0026 Viewer channel inspection: the channel menu's worker listing reads the same
    // compiler and media base directory as evaluation.
    previewController.setChannelListingProvider(
        [&snapshotCompiler, &cpuEvaluator](
            const bloom::document::Snapshot& snapshot,
            const bloom::document::CompositionId composition, const bloom::document::NodeId node,
            const std::string& port, const bloom::core::RationalTime time,
            const bloom::runtime::EvaluationColorIntent& colorIntent,
            const bloom::runtime::CancellationToken& cancellation) {
            return bloom::runtime::listChannelInspectionChoices(
                snapshotCompiler, snapshot, composition, node, port,
                cpuEvaluator.assetBaseDirectory(), time, colorIntent, cancellation);
        });
    // #364 Cryptomatte: the Properties layer picker and the Viewer eyedropper read the channel set
    // through the same compiler and media base directory on a worker.
    bloom::ui::CryptomatteController cryptomatteController(
        compositionSession, taskScheduler, taskUiBridge,
        [&snapshotCompiler, &cpuEvaluator](const bloom::document::Snapshot& snapshot,
                                           const bloom::document::CompositionId composition,
                                           const bloom::document::NodeId node,
                                           const bloom::core::RationalTime time,
                                           const bloom::runtime::EvaluationColorIntent& colorIntent,
                                           const bloom::runtime::CancellationToken& cancellation) {
            return bloom::runtime::listCryptomatteLayers(snapshotCompiler, snapshot, composition,
                                                         node, cpuEvaluator.assetBaseDirectory(),
                                                         time, colorIntent, cancellation);
        },
        [&snapshotCompiler, &cpuEvaluator](
            const bloom::document::Snapshot& snapshot,
            const bloom::document::CompositionId composition, const bloom::document::NodeId node,
            const bloom::core::RationalTime time,
            const bloom::runtime::EvaluationColorIntent& colorIntent,
            const std::optional<bloom::runtime::ChannelInspection>& inspection, const std::int64_t x,
            const std::int64_t y, const bloom::runtime::CancellationToken& cancellation) {
            return bloom::runtime::pickCryptomatte(snapshotCompiler, snapshot, composition, node,
                                                   cpuEvaluator.assetBaseDirectory(), time,
                                                   colorIntent, inspection, x, y, cancellation);
        });
    bloom::ui::BackgroundPreviewController backgroundPreviewController(
        compositionSession, previewController, taskScheduler, taskUiBridge, previewPipeline,
        nullptr, gpuPreviewDisplaySubmitter);
    bloom::ui::RamPreviewController ramPreviewController(
        compositionSession, previewController, taskScheduler, taskUiBridge, previewPipeline,
        nullptr, gpuPreviewDisplaySubmitter);
    // #380: the Assets header's composition frames, prepared on a worker by the same pipeline.
    bloom::ui::CompositionThumbnailController compositionThumbnails(
        compositionSession, taskScheduler, taskUiBridge, previewPipeline,
        previewController.effectivePixelStorageByteLimit());
    // The application-lifetime GPU final-render provider is created HERE, once, before the
    // shutdown coordinator that will own its retirement ordering, so it outlives every coordinator
    // callback and every export attempt. It reuses the same app-relative bundled loader the
    // preview service uses. The coordinator signals the evaluator owner (non-blocking) at
    // beginShutdown and polls genuine retirement completion from the UI event loop, so the UI
    // never waits on the native owner even after task admission closes.
    bloom::runtime::GpuProcessFrameEvaluatorOptions gpuExportOptions;
    gpuExportOptions.processingPhasePolicy = processingPhasePolicy;
    gpuExportOptions.enabled = bundledNativeLoader;
    if (bundledNativeLoader) {
        gpuExportOptions.loaderPath = gpuPreviewDisplayOptions.loaderPath;
    }
    // Reuse the ONE shared app OCIO context resolver built above from THIS target's own packaging
    // definitions: the provider's CPU-worker bootstrap qualifies the tools once and publishes the
    // same shared context for the export evaluator's media/effect transforms and output display, so
    // preview and export share one tool qualification and one program cache. The media context is
    // refreshed per request on the calling CPU worker. Final requests override the live base with
    // the source directory captured beside their own immutable snapshot, including across Save As.
    gpuExportOptions.ocioResolver = gpuOcioContextResolver;
    gpuExportOptions.mediaContextProvider = [&cpuEvaluator, gpuPreparedUploadCache] {
        auto context = bloom::runtime::GpuSceneMediaContext::fromEvaluator(cpuEvaluator);
        context.preparedUploadCache = gpuPreparedUploadCache;
        return context;
    };
    auto gpuExportProvider = bloom::host::GpuExportProvider::create(gpuExportOptions);
    gpuExportProvider->prepare(taskScheduler);
    bool shutdownQuiescent = false;
    bloom::ui::ApplicationShutdownCoordinator shutdownCoordinator(previewController, taskUiBridge);
    shutdownCoordinator.setAudioRetirement([&] { audioDeviceControl.beginShutdown(); },
                                           [&] { return audioDeviceControl.retirementComplete(); });
    QObject::connect(&shutdownCoordinator,
                     &bloom::ui::ApplicationShutdownCoordinator::shutdownStarted,
                     &memoryBudgetController, &bloom::ui::MemoryBudgetController::beginShutdown);
    shutdownCoordinator.setGpuExportRetirement(
        [&gpuExportProvider, &sequenceExportRetirement] {
            sequenceExportRetirement->beginShutdown();
            gpuExportProvider->beginShutdown();
        },
        [&gpuExportProvider, &sequenceExportRetirement] {
            if (!sequenceExportRetirement->retirementComplete() ||
                !gpuExportProvider->retirementComplete()) {
                return false;
            }
            gpuExportProvider->collectRetired();
            return true;
        });
    QObject::connect(&shutdownCoordinator,
                     &bloom::ui::ApplicationShutdownCoordinator::shutdownStarted,
                     &ramPreviewController, &bloom::ui::RamPreviewController::beginShutdown);
    QObject::connect(&shutdownCoordinator,
                     &bloom::ui::ApplicationShutdownCoordinator::shutdownStarted,
                     &cryptomatteController, &bloom::ui::CryptomatteController::cancel);
    QObject::connect(&shutdownCoordinator,
                     &bloom::ui::ApplicationShutdownCoordinator::shutdownStarted,
                     &compositionThumbnails, &bloom::ui::CompositionThumbnailController::cancel);
    // Pre-host retirement is installed below once the window exists. The early setup path
    // has no Viewer pins, but still stops the producer before closing the host owner.
    shutdownCoordinator.setPackedHostRetirement(
        [&] {
            gpuPreviewDisplayService.beginShutdown();
            previewController.releasePublishedFramesForShutdown();
            cpuEvaluator.operationCache()->closePreparedUploads();
            packedHostRetirement->beginShutdown();
        },
        [&] { return packedHostRetirement->exitAcknowledged(); });
    QObject::connect(&shutdownCoordinator,
                     &bloom::ui::ApplicationShutdownCoordinator::shutdownQuiescent,
                     &shutdownCoordinator, [&] { shutdownQuiescent = true; });
    application.installEventFilter(&shutdownCoordinator);
    // Kept live even though no editor shows it (task F1, item F6 removed Jobs from the registry
    // below): this is the model a JobsEditor takes, and it is the bridge's own consumer. Dropping
    // it would change what happens to task-bridge state, which item F6 does not ask for.
    bloom::ui::TaskMonitorModel taskMonitor(taskUiBridge);

    // "File -> Export Frame..." (task F3, issue #103): binds to the SAME application-wide
    // PublicationCoordinator/StagedArtifactCoordinator ProjectHost already owns (docs/architecture/
    // frame-output.md, "Capability Boundary": saves and exports share one coordinator pair for
    // correct same-target ordering/supersession), and to the SAME SnapshotCompiler/TaskScheduler/
    // TaskUiBridge the preview pipeline above already uses.
    bloom::ui::FrameExportController frameExportController(
        compositionSession, taskScheduler, taskUiBridge, snapshotCompiler,
        projectHost.publicationCoordinator(), projectHost.artifactCoordinator(), {},
        &qualifiedDisplayProcessorProvider, gpuExportProvider);
    frameExportController.setSequenceExportRetirementOwner(sequenceExportRetirement);
    // Which H.264 encoder this platform uses, and whether OpenH264 is verified, checked on a
    // worker for the Export dialog (never hashed on the UI thread).
    bloom::ui::H264EncoderService h264Encoder(taskScheduler, taskUiBridge);
    frameExportController.setH264EncoderService(&h264Encoder);
    frameExportController.setProjectPathProvider(
        [&projectHost] { return projectHost.displayPath(); });
    QObject::connect(&shutdownCoordinator,
                     &bloom::ui::ApplicationShutdownCoordinator::shutdownStarted,
                     &frameExportController, &bloom::ui::FrameExportController::retireForShutdown);

    // The typed viewer GPU dependency context. Its presentation-client getter reads the cached
    // service capability, so an editor created before the async startup qualification finishes is
    // handed the client as soon as it is published (the bridge poll below refreshes the cache), and
    // every later editor -- including a workspace replacement -- reads the same cache. No native
    // handle ever crosses this seam; a null client leaves the unchanged CPU paint path.
    //
    // Declaration order is the lifetime contract: the bootstrap outlives the dependency context,
    // which outlives the registry whose factories capture a pointer to it, and the window (and its
    // editors) is declared last so it is destroyed first. No factory is invoked after the context
    // or the bootstrap is gone.
    const auto* const primaryScreen = QGuiApplication::primaryScreen();
    bloom::ui::GpuViewerBootstrap gpuViewerBootstrap(
        taskScheduler, gpuPreviewDisplayOptions.loaderPath.string(),
        primaryScreen != nullptr ? primaryScreen->devicePixelRatio() : 1.0);
    // The bootstrap installs the owner-resolved capacity plan through the poll below. Binding the
    // shared frame cache is inert (no device query); until the owner resolves the device budget the
    // configured sublimits installed above remain in force.
    gpuViewerBootstrap.bindResidentCache(*previewFrameCache);
    bloom::ui::ViewerGpuDependencies viewerGpuDependencies = gpuViewerBootstrap.dependencies();
    bloom::ui::EditorRegistry editorRegistry;
    bloom::app::DeferredEditorTeardown deferredEditorTeardown;
    // Jobs is deliberately NOT registered (task F1, item F6). An editor in this registry is an
    // editor the panel switcher offers and a workspace can place, and Jobs is wanted in neither
    // for now. The JobsEditor class and registerJobsEditor() both survive untouched -- the single
    // `&& bloom::ui::registerJobsEditor(editorRegistry, taskMonitor)` this line used to carry is
    // all it takes to offer the panel again -- so Jobs is reachable programmatically and simply
    // not on offer in the interface.
    const bool editorsRegistered = bloom::ui::registerFoundationEditors(
        editorRegistry, compositionSession, previewController, &ramPreviewController, &projectHost,
        &viewerGpuDependencies, gpuExportProvider);
    if (!editorsRegistered) {
        QEventLoop shutdownLoop;
        QObject::connect(&shutdownCoordinator,
                         &bloom::ui::ApplicationShutdownCoordinator::shutdownQuiescent,
                         &shutdownLoop, &QEventLoop::quit);
        shutdownCoordinator.beginShutdown();
        if (!shutdownQuiescent) {
            shutdownLoop.exec();
        }
        while (!packedHostRetirement->collect())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        while (!sequenceExportRetirement->collect())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return 1;
    }

    application.setQuitOnLastWindowClosed(false);
    QSettings settings;
    auto& playback = previewController.playbackController();
    bloom::ui::AudioPlaybackSession audioPlaybackSession(compositionSession, taskScheduler,
                                                         taskUiBridge, snapshotCompiler,
                                                         compiledPlanCache, cpuEvaluator);
    audioPlaybackSession.setAudioBufferSource(assetController);
    playback.setAudioDeviceControl(&audioDeviceControl);
    QObject::connect(&shutdownCoordinator,
                     &bloom::ui::ApplicationShutdownCoordinator::shutdownStarted,
                     &audioPlaybackSession, &bloom::ui::AudioPlaybackSession::beginShutdown);
    playback.setAudioEnabled(
        settings.value(QStringLiteral("playback/audio-enabled"), true).toBool());
    const auto applyAudioMix = [&] {
        const auto& mix = audioPlaybackSession.mix();
        if (!mix.has_value()) {
            playback.setAudioMix({}, {});
            return;
        }
        std::vector<bloom::media::audio::playback::AudioClip> clips;
        clips.reserve(mix->clips.size());
        for (const auto& description : mix->clips) {
            const auto buffer =
                audioPlaybackSession.audioBuffer(description.assetId, description.audioStream);
            if (buffer == nullptr)
                continue;
            clips.push_back({.buffer = buffer,
                             .startTime = description.startTime,
                             .level = static_cast<float>(description.level),
                             .muted = description.muted,
                             .solo = description.solo,
                             .endTime = description.endTime});
            if (!description.timeMappings.empty())
                clips.back().mapTime = [description](const bloom::core::RationalTime time) {
                    return bloom::runtime::mapAudioClipTime(description, time);
                };
        }
        playback.setAudioMix(*mix, std::move(clips));
    };
    QObject::connect(&audioPlaybackSession, &bloom::ui::AudioPlaybackSession::mixChanged, &playback,
                     applyAudioMix);
    (void)audioPlaybackSession.refresh();
    applyAudioMix();
    // The Preferences window's read-only Performance page reads this cached provider. The existing
    // status poll below hands it one already-published `service.status()` read per tick, so the
    // page follows Initializing -> Ready -> a later capability loss without any UI-thread device
    // probe. Declared before the window so it outlives the borrowed pointer the window holds.
    bloom::ui::CachedAccelerationStatusProvider accelerationStatus;
    // Edit | Purge…: the async, off-UI purge of derived caches. It shares the one scheduler and
    // TaskUiBridge the rest of the application already polls, clears the GPU-scene and prepared-
    // upload stores the preview and media paths share, and reaches the evaluator's decoded-video
    // cache through its narrow owning API. Declared before the window so it outlives it.
    bloom::ui::CachePurgeController cachePurgeController(
        taskScheduler, taskUiBridge, mediaDiskCache.get(), cpuEvaluator.operationCache().get(),
        gpuPreparedUploadCache.get(), gpuSceneCoverageCache.get(),
        [&cpuEvaluator] { cpuEvaluator.clearDecodedVideoCache(); },
        [&previewController, &assetController, &frameExportController](const bool gated) {
            assetController.setCachePurgeGate(gated);
            frameExportController.setCachePurgeGate(gated);
            previewController.setCachePurgeGate(gated);
        },
        [&gpuPreviewDisplayService](const std::chrono::milliseconds timeout) {
            return gpuPreviewDisplayService.purgeRetainedCaches(timeout);
        });
    // Native (server-side) window chrome only (task C1): MainWindow no longer takes a chrome mode
    // at all -- there is nothing left for main() to read from settings before constructing it.
    bloom::ui::MainWindow window(editorRegistry, compositionSession, projectHost,
                                 frameExportController, &ramPreviewController, &previewController,
                                 nullptr, &playback, cpuEvaluator.operationCache().get(),
                                 mediaDiskCache.get(), &accelerationStatus, &cachePurgeController);
    // Every default dialog of the project host and the export controller opens over the window.
    projectHost.setDialogOwner(&window);
    frameExportController.setDialogOwner(&window);
    // UI sound feedback and render notifications (docs/architecture/ui-feedback.md). The sound
    // engine decodes and opens its own output device on a worker thread; nothing here blocks.
    auto uiSoundEngine = std::make_unique<bloom::ui::UiSoundEngine>(
        bloom::media::audio::playback::makeMiniaudioBackend(),
        bloom::ui::bundledFeedbackSoundBytes, [](const std::string& message) {
            std::cerr << "Bloom: " << message << '\n';
        });
    uiSoundEngine->start();
    bloom::ui::UiFeedback uiFeedback(std::move(uiSoundEngine),
                                     bloom::ui::makeSystemNotifier(&application));
    const auto applyFeedbackPreferences = [&uiFeedback] {
        const QSettings feedbackSettings;
        uiFeedback.setSettings(bloom::ui::feedbackSettingsFrom(
            bloom::ui::loadApplicationPreferences(feedbackSettings)));
    };
    applyFeedbackPreferences();
    uiFeedback.setNotificationActivationHandler([&window] {
        window.setWindowState(window.windowState() & ~Qt::WindowMinimized);
        window.show();
        window.raise();
        window.activateWindow();
    });
    bloom::ui::InteractionSounds interactionSounds(uiFeedback, &application);
    application.installEventFilter(&interactionSounds);
    QObject::connect(&window, &bloom::ui::MainWindow::preferencesChanged, &window,
                     applyFeedbackPreferences);
    QObject::connect(&window, &bloom::ui::MainWindow::soundTestRequested, &window,
                     [&uiFeedback](const bloom::ui::ApplicationPreferences& draft) {
                         uiFeedback.playTest(bloom::ui::feedbackSettingsFrom(draft));
                     });
    QObject::connect(&frameExportController, &bloom::ui::FrameExportController::exportFinished,
                     &window, [&uiFeedback](const bloom::ui::FrameExportOutcome outcome,
                                            const QString& message) {
                         uiFeedback.announceExport(outcome, message);
                     });
    QObject::connect(&assetController, &bloom::ui::AssetController::importFinished, &window,
                     [&uiFeedback](const bloom::ui::AssetController::ImportReport& report) {
                         if (const auto event = bloom::ui::importFeedbackEvent(
                                 report.cancelled, report.discarded, report.imported,
                                 report.skipped.size(), !report.failure.isEmpty())) {
                             uiFeedback.play(*event);
                         }
                     });
    QObject::connect(&compositionSession, &bloom::ui::CompositionSession::commandRejected, &window,
                     [&uiFeedback](const QString&) {
                         uiFeedback.play(bloom::ui::FeedbackEvent::Refused);
                     });
    QObject::connect(&playback, &bloom::ui::PlaybackController::stateChanged, &window,
                     [&uiFeedback](const bloom::ui::PlaybackState state) {
                         uiFeedback.setInteractionSuppressed(
                             bloom::ui::UiFeedback::Suppression::Playback,
                             state == bloom::ui::PlaybackState::Playing);
                     });
    // Apply memory preferences to their owning caches now. Disk cache configuration alone
    // remains restart-required; live processing admission is independent of retention targets.
    QObject::connect(&window, &bloom::ui::MainWindow::preferencesChanged, &playback, [&] {
        const QSettings settings;
        playback.setAudioEnabled(
            settings.value(QStringLiteral("playback/audio-enabled"), true).toBool());
        playback.setLooping(settings.value(QStringLiteral("playback/loop"), true).toBool());
        static_cast<void>(applyMemoryPreferences());
        memoryBudgetController.requestSample();
    });
    playback.installWindowShortcut(window);
    // The shutdown contract's second half: the workspace enumerates every live editor native
    // surface so the coordinator can require genuine retirement before Qt teardown. A CPU-only
    // workspace yields no surfaces and shutdown behavior is exactly as before.
    shutdownCoordinator.setNativeSurfaceSource(
        [&window] { return window.workspaceHost()->liveNativeSurfaces(); });
    // Re-publish a changed presentation capability into already-live ViewerEditors: a newly Ready
    // client is handed over, and a later loss is handed over as a null client so those viewers drop
    // back to their own CPU paint path. The factory seam covers every editor created later; this
    // covers the ones the initial workspace restore already built. The viewer owns its own
    // same-request CPU fallback, and a null client simply leaves that CPU path in place.
    gpuViewerBootstrap.setPublicationSink([&window](const bloom::ui::ViewerGpuDependencies& deps) {
        for (auto* surface : window.workspaceHost()->liveNativeSurfaces()) {
            if (auto* viewer = dynamic_cast<bloom::ui::ViewerEditor*>(surface); viewer != nullptr) {
                viewer->setGpuPresentationDependencies(deps.presentationClient(), deps.scheduler,
                                                       deps.vulkanLoaderPath,
                                                       deps.devicePixelRatio);
            }
        }
    });
    QObject::connect(&ramPreviewController, &bloom::ui::RamPreviewController::stateChanged,
                     &playback, [&] {
                         if (ramPreviewController.isCaching()) {
                             playback.pause();
                         }
                     });
    QObject::connect(&ramPreviewController, &bloom::ui::RamPreviewController::cachingFinished,
                     &playback, [&](const bloom::ui::RamPreviewController::Completion outcome) {
                         // Partial retention has no ephemeral transport range: manual playback
                         // remains available, but only a still-current fully retained range
                         // autoplays.
                         if (outcome == bloom::ui::RamPreviewController::Completion::Complete &&
                             ramPreviewController.canAutoPlay()) {
                             static_cast<void>(compositionSession.setCurrentTime(
                                 ramPreviewController.playableSpan()->start));
                             playback.play();
                         }
                     });
    (void)window.restoreApplicationState(settings);
    QObject::connect(&shutdownCoordinator,
                     &bloom::ui::ApplicationShutdownCoordinator::shutdownStarted, &window,
                     [&window, &settings] { window.saveApplicationState(settings); });
    // UI sounds stop with shutdown; the destructor then joins the sound worker.
    QObject::connect(&shutdownCoordinator,
                     &bloom::ui::ApplicationShutdownCoordinator::shutdownStarted, &window,
                     [&uiFeedback] { uiFeedback.shutdown(); });
    QObject::connect(&window, &bloom::ui::MainWindow::shutdownRequested, &shutdownCoordinator,
                     &bloom::ui::ApplicationShutdownCoordinator::beginShutdown);
    // QApplication::quit() asks visible top-level windows to close. Release MainWindow's
    // asynchronous-close guard only now, or that final request is vetoed forever after Discard.
    QObject::connect(&shutdownCoordinator,
                     &bloom::ui::ApplicationShutdownCoordinator::shutdownQuiescent, &window,
                     &bloom::ui::MainWindow::completeShutdown);
    QObject::connect(&shutdownCoordinator,
                     &bloom::ui::ApplicationShutdownCoordinator::shutdownQuiescent, &application,
                     &QApplication::quit);
    shutdownCoordinator.setPackedHostRetirement(
        [&] {
            // Tasks, native surfaces and export owners have retired before this callback.
            // Release every live CPU or native Viewer, not just surfaces with native targets.
            gpuPreviewDisplayService.beginShutdown();
            window.workspaceHost()->releasePackedFramesForShutdown();
            previewController.releasePublishedFramesForShutdown();
            cpuEvaluator.operationCache()->closePreparedUploads();
            packedHostRetirement->beginShutdown();
        },
        [&] { return packedHostRetirement->exitAcknowledged(); });
    window.show();

    // The EXISTING TaskUiBridge poll drives the cached capability refresh and keeps running until
    // shutdown, so a capability that is later lost is observed and published as a null client --
    // the stop-on-Ready timer it replaces could never see a loss. This is a locked status read
    // only: it never probes the device, blocks, or renders on the UI thread. One read feeds both
    // the bootstrap (which republishes the presentation client) and the cached Preferences status.
    QObject::connect(&taskUiBridge, &bloom::ui::TaskUiBridge::snapshotsPolled, &application,
                     [&gpuViewerBootstrap, &gpuPreviewDisplayService, &accelerationStatus] {
                         const auto status = gpuPreviewDisplayService.status();
                         gpuViewerBootstrap.refreshFromStatus(status);
                         accelerationStatus.setServiceStatus(status);
                     });
    taskUiBridge.wake();

    // SAVEFIX-1: the launch after an abnormal exit. Queued, not called inline, so the offer is
    // presented over a window that is already on screen rather than in front of one.
    QTimer::singleShot(0, &projectHost, &bloom::ui::ProjectHost::offerRecoveryOnStartup);

    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
    const int result = application.exec();
    if (!shutdownQuiescent) {
        // Also cover a programmatic quit that bypassed the window close gate. Continue pumping
        // retirement, never joining an active event loop or treating a timeout as quiescence.
        QEventLoop shutdownLoop;
        QObject::connect(&shutdownCoordinator,
                         &bloom::ui::ApplicationShutdownCoordinator::shutdownQuiescent,
                         &shutdownLoop, &QEventLoop::quit);
        shutdownCoordinator.beginShutdown();
        if (!shutdownQuiescent)
            shutdownLoop.exec();
    }
    while (!packedHostRetirement->collect())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (!sequenceExportRetirement->collect())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (previewTrace) {
        // Recording has stopped with the event loop. The worker owns formatting and file I/O;
        // joining here cannot stall an active viewer or hold up an event-loop publication.
        bool exported = false;
        std::thread writer([&] {
            try {
                std::ofstream output(previewTraceDestination.toStdString());
                previewTrace->writeJson(output);
                exported = output.good();
            } catch (...) {
                exported = false;
            }
        });
        writer.join();
        if (!exported)
            std::cerr << "Preview trace export failed\n";
    }
    return result;
} catch (const std::exception& error) {
    std::cerr << "Bloom could not complete application setup or teardown: " << error.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "Bloom could not complete application setup or teardown: unknown exception\n";
    return 1;
}
