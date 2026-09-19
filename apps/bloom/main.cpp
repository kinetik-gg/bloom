#include <algorithm>
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/media/cache/media_disk_cache.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_prepared_upload_cache.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/application_shutdown_coordinator.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/audio_playback_session.hpp>
#include <bloom/ui/background_preview_controller.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_gpu_scene_stage.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/frame_export_controller.hpp>
#include <bloom/ui/gpu_viewer_bootstrap.hpp>
#include <bloom/ui/jobs_editor.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/media_disk_cache_settings.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/qualified_display_processor_bootstrap.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
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

#include <memory>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);

    QCoreApplication::setApplicationName("Bloom");
    QCoreApplication::setApplicationVersion("0.1.0");
    QCoreApplication::setOrganizationName("Kinetik");

    // The Kinetik visual language is installed once, application-wide, before any widget exists
    // (task U1, issue #117). It replaces MainWindow::applyFoundationTheme(): the palette, the
    // application stylesheet, and the interface font belong to the application, not to one window,
    // and installing them first means no surface is ever constructed under the default Qt theme.
    bloom::ui::kit::installKinetikTheme(application);
    // Mnemonic underlines only while Alt is held (task U2, issue #118, decision 2): a small
    // QProxyStyle installed application-wide, right after the theme's own Fusion style, so every
    // menu -- title-bar-embedded or classic -- shares the same behavior. QApplication::setStyle()
    // takes ownership of the style object (Qt's own documented contract); the static analyzer
    // cannot see through that ownership transfer and reports a path-sensitive "leak" anchored
    // wherever later in main() its analysis happens to conclude the object is unreachable, not at
    // this allocation -- hence the function-scoped suppression markers below rather than a
    // single-line one.
    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks) -- QApplication::setStyle() owns it.
    auto* mnemonicStyle = new bloom::ui::kit::AltUnderlineProxyStyle();
    QApplication::setStyle(mnemonicStyle);

    // ProjectHost (task U1, issue #72) replaces the hand-rolled document/command-stack pair: it
    // owns the application's single live bloom::host::ProjectSession and constructs an initial
    // createNew() project itself. taskScheduler is shared with CompositionPreviewController below
    // (both need a TaskExecutor::BlockingIo-capable scheduler; TaskSchedulerConfig::defaults()
    // already provisions one BlockingIo worker, so a single scheduler instance serves both).
    bloom::runtime::TaskScheduler taskScheduler;
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
    QObject::connect(
        &projectHost, &bloom::ui::ProjectHost::sessionReplaced, &compositionSession,
        [&projectHost, &compositionSession] {
            auto [document, commandStack] = projectHost.liveDocumentAndStack();
            if (document == nullptr || commandStack == nullptr) {
                return;
            }
            compositionSession.rebind(*document, *commandStack, projectHost.lowestCompositionId());
            if (const auto* settings = projectHost.colorSettings(); settings != nullptr) {
                compositionSession.setColorSettings(*settings);
            }
        });
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
    const QSettings playbackSettings;
    const auto cacheBudgets = bloom::ui::cacheMemoryBudgetsFromSettings(playbackSettings);
    {
        const auto videoBytes =
            std::min(std::size_t{256} * 1024U * 1024U, cacheBudgets.operationCacheByteBudget / 4U);
        cpuEvaluator.setVideoCacheByteBudget(videoBytes);
        cpuEvaluator.operationCache()->setByteBudget(cacheBudgets.operationCacheByteBudget -
                                                     videoBytes);
    }
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
    bloom::ui::TaskUiBridge taskUiBridge(taskScheduler);
    bloom::ui::AssetController assetController(compositionSession, projectHost, taskScheduler,
                                               taskUiBridge, mediaDiskCache.get());
    const auto updateMediaDirectory = [&] {
        const auto path = projectHost.displayPath();
        cpuEvaluator.setAssetBaseDirectory(path ? path->parent_path() : std::filesystem::path{});
    };
    QObject::connect(&projectHost, &bloom::ui::ProjectHost::sessionReplaced, &assetController,
                     updateMediaDirectory);
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
    // Presentation is requested only for the genuine Wayland session with a packaged loader. Every
    // other platform/session keeps the compute-only or CPU-only path; no blank activation is
    // claimed and the service still publishes an explicit Unavailable capability if the device
    // cannot enable a present-capable queue.
    if (bloom::ui::shouldRequestWaylandPresentation(
            bundledNativeLoader,
            QGuiApplication::platformName().contains(QStringLiteral("wayland")))) {
        gpuPreviewDisplayOptions.presentation =
            bloom::runtime::GpuPreviewDisplayServicePresentationMode::Wayland;
    }
    // Align the resident lease registry with the artist's UI frame-cache budget so the registry can
    // publish every lease the cache can reference, with bounded in-flight/visible headroom and a
    // hard VRAM ceiling. No live lease/pin is ever invalidated; pressure takes the CPU fallback.
    const auto gpuResidentBudgetPlan =
        bloom::ui::gpuResidentBudgetPlanFor(cacheBudgets.previewFrameCacheByteBudget);
    bloom::ui::applyGpuResidentBudgetPlan(gpuPreviewDisplayOptions, gpuResidentBudgetPlan);
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
    auto gpuPreparedUploadCache = std::make_shared<bloom::runtime::GpuPreparedUploadCache>();
    auto gpuPreviewGpuSceneStage = bloom::ui::makeSessionRefreshingGpuSceneStage(
        snapshotCompiler, cpuEvaluator, qualifiedDisplayProcessorProvider, gpuSceneCoverageCache,
        gpuPreparedUploadCache, compiledPlanCache);
    auto gpuPreviewCpuStage = bloom::ui::makeCompositionPreviewCpuStage(
        snapshotCompiler, cpuEvaluator, qualifiedDisplayProcessorProvider, compiledPlanCache);
    auto gpuPreviewCpuDisplayFallback =
        bloom::ui::makeCompositionPreviewCpuDisplayFallback(referenceDisplayPreparer);
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
        previewFrameCache, nullptr, gpuPreviewDisplaySubmitter);
    bloom::ui::BackgroundPreviewController backgroundPreviewController(
        compositionSession, previewController, taskScheduler, taskUiBridge, previewPipeline,
        nullptr, gpuPreviewDisplaySubmitter);
    bloom::ui::RamPreviewController ramPreviewController(
        compositionSession, previewController, taskScheduler, taskUiBridge, previewPipeline,
        nullptr, gpuPreviewDisplaySubmitter);
    bloom::ui::ApplicationShutdownCoordinator shutdownCoordinator(previewController, taskUiBridge);
    QObject::connect(&shutdownCoordinator,
                     &bloom::ui::ApplicationShutdownCoordinator::shutdownStarted,
                     &ramPreviewController, &bloom::ui::RamPreviewController::beginShutdown);
    // Non-blocking: the service closes its own admission, cancels the tasks it submitted, and wakes
    // its thread. Its destructor (which runs before the scheduler's) joins that thread and drains
    // child/native ownership, so nothing joins the UI thread while GPU work is in flight.
    //
    // HOST-RETIREMENT: this is intentionally connected to shutdownQuiescent (both task quiescence
    // AND native-surface retirement), not shutdownStarted. Beginning service shutdown while a
    // presenter's native target is still live would cancel the very work whose retirement proof the
    // shutdown gate waits for; the service must keep pumping until every surface is genuinely
    // retired. If a future service API exposes a separate "stop admitting, keep pumping" call, add
    // it on shutdownStarted and keep the destruction hook here.
    QObject::connect(&shutdownCoordinator,
                     &bloom::ui::ApplicationShutdownCoordinator::shutdownQuiescent,
                     &shutdownCoordinator,
                     [&gpuPreviewDisplayService] { gpuPreviewDisplayService.beginShutdown(); });
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
        &qualifiedDisplayProcessorProvider);

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
    bloom::ui::ViewerGpuDependencies viewerGpuDependencies = gpuViewerBootstrap.dependencies();
    bloom::ui::EditorRegistry editorRegistry;
    // Jobs is deliberately NOT registered (task F1, item F6). An editor in this registry is an
    // editor the panel switcher offers and a workspace can place, and Jobs is wanted in neither
    // for now. The JobsEditor class and registerJobsEditor() both survive untouched -- the single
    // `&& bloom::ui::registerJobsEditor(editorRegistry, taskMonitor)` this line used to carry is
    // all it takes to offer the panel again -- so Jobs is reachable programmatically and simply
    // not on offer in the interface.
    const bool editorsRegistered = bloom::ui::registerFoundationEditors(
        editorRegistry, compositionSession, previewController, &ramPreviewController, &projectHost,
        &viewerGpuDependencies);
    if (!editorsRegistered) {
        QEventLoop shutdownLoop;
        QObject::connect(&shutdownCoordinator,
                         &bloom::ui::ApplicationShutdownCoordinator::shutdownQuiescent,
                         &shutdownLoop, &QEventLoop::quit);
        shutdownCoordinator.beginShutdown();
        if (!taskScheduler.isQuiescent()) {
            shutdownLoop.exec();
        }
        return 1;
    }

    application.setQuitOnLastWindowClosed(false);
    QSettings settings;
    auto& playback = previewController.playbackController();
    bloom::ui::AudioPlaybackSession audioPlaybackSession(compositionSession, snapshotCompiler,
                                                         compiledPlanCache, cpuEvaluator);
    playback.setAudioEngine(std::make_unique<bloom::media::audio::playback::AudioEngine>(
        bloom::media::audio::playback::makeMiniaudioBackend()));
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
            const auto buffer = assetController.audioBuffer(description.assetId);
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
    QObject::connect(&assetController, &bloom::ui::AssetController::changed, &playback,
                     applyAudioMix);
    (void)audioPlaybackSession.refresh();
    applyAudioMix();
    // Native (server-side) window chrome only (task C1): MainWindow no longer takes a chrome mode
    // at all -- there is nothing left for main() to read from settings before constructing it.
    bloom::ui::MainWindow window(editorRegistry, compositionSession, projectHost,
                                 frameExportController, &ramPreviewController, &previewController,
                                 nullptr, &playback, cpuEvaluator.operationCache().get(),
                                 mediaDiskCache.get());
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
                     &playback, [&playback](const bool completed) {
                         if (completed) {
                             playback.play();
                         }
                     });
    (void)window.restoreApplicationState(settings);
    QObject::connect(&shutdownCoordinator,
                     &bloom::ui::ApplicationShutdownCoordinator::shutdownStarted, &window,
                     [&window, &settings] { window.saveApplicationState(settings); });
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
    window.show();

    // The EXISTING TaskUiBridge poll drives the cached capability refresh and keeps running until
    // shutdown, so a capability that is later lost is observed and published as a null client --
    // the stop-on-Ready timer it replaces could never see a loss. This is a locked status read
    // only: it never probes the device, blocks, or renders on the UI thread.
    QObject::connect(&taskUiBridge, &bloom::ui::TaskUiBridge::snapshotsPolled, &application,
                     [&gpuViewerBootstrap, &gpuPreviewDisplayService] {
                         gpuViewerBootstrap.refreshFromStatus(gpuPreviewDisplayService.status());
                     });
    taskUiBridge.wake();

    // SAVEFIX-1: the launch after an abnormal exit. Queued, not called inline, so the offer is
    // presented over a window that is already on screen rather than in front of one.
    QTimer::singleShot(0, &projectHost, &bloom::ui::ProjectHost::offerRecoveryOnStartup);

    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
    return application.exec();
}
