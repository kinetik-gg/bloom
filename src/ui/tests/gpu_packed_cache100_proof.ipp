// A distinct 100-member 1080 EXR sequence through the real app stage, RAM cache and Viewer.
// This is a capacity/identity/replay proof, not SMB/EXR production-throughput qualification.
namespace {
int runPackedCache100(const TestOptions& options, QTemporaryDir& temporary, const bool half,
                      const bool partial) {
    Expectations checks;
    QSettings().setValue("viewer/resolution", half ? "Half" : "Full");
    Fixture fixture(temporary.path().toStdString());
    auto sequence = fixture.mediaAsset;
    sequence.kind = document::AssetKind::Sequence;
    sequence.width = 1920;
    sequence.height = 1080;
    sequence.frames = 100;
    sequence.rate = 25;
    sequence.manifest.pattern = "packed.####.exr";
    sequence.manifest.padding = 4;
    sequence.manifest.first = 0;
    sequence.manifest.last = 99;
    for (std::int64_t index = 0; index < 100; ++index) {
        const auto path = std::filesystem::path(temporary.path().toStdString()) /
                          ("packed." + std::to_string(index) + ".exr");
        writeExrRgba(
            path, 1920, 1080,
            std::vector<ExrPixel>(1920ULL * 1080, ExrPixel{static_cast<float>(index + 1) / 128.0F,
                                                           0.3F, 0.7F, 1.0F}));
        const auto member = imageAsset(path, "Distinct packed member", 901);
        sequence.manifest.members.push_back({index, member.locator, member.contentDigest});
    }
    sequence.locator = sequence.manifest.members.front().locator;
    sequence.contentDigest = sequence.manifest.members.front().contentDigest;
    fixture.evaluator.operationCache()->setByteBudget(256ULL << 20);
    const auto rate = document::FrameRate::create(25, 1);
    if (!rate)
        return 2;
    const auto format =
        document::CompositionFormat::create(1920, 1080, core::PixelAspectRatio::square(), *rate);
    if (!format)
        return 2;
    const auto duration = core::RationalTime::fromInteger(4);
    auto project = document::makeNewProject("Packed100", "Main", duration, *format);
    checks.expect(project.project.addAsset(sequence), "genuine 100-member sequence validates");
    document::Document document(std::move(project.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, project.initialCompositionId);
    commands::Transaction add("Add packed sequence");
    add.emplace<commands::AddImageLayer>(project.initialCompositionId, sequence.id);
    checks.expect(commands.execute(std::move(add)).succeeded(), "real sequence layer is authored");
    runtime::TaskScheduler scheduler(schedulerConfig());
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    auto resolver =
        host::makePackagedGpuOcioResolver(QCoreApplication::applicationFilePath().toStdString());
    checks.expect(resolver != nullptr, "packaged normal application resolver is available");
    auto stage = ui::makeSessionRefreshingGpuSceneStage(
        fixture.compiler, fixture.evaluator, fixture.provider,
        std::make_shared<runtime::GpuSceneCoverageCache>(),
        std::make_shared<runtime::GpuPreparedUploadCache>(), nullptr, resolver);
    auto configured = serviceOptions(options.loader_path);
    configured.product = runtime::GpuPreviewDisplayProductMode::GpuPreparedPacked;
    configured.presentation = runtime::GpuPreviewDisplayServicePresentationMode::Disabled;
    runtime::GpuPreviewDisplayService service(scheduler, stage, fixture.productionCpuStage(),
                                              fixture.productionCpuFallback(displayPreparer),
                                              configured);
    checks.expect(waitUntil(
                      [&] {
                          return service.status().state !=
                                 runtime::GpuPreviewDisplayServiceState::Initializing;
                      },
                      90s),
                  "genuine offscreen GPU service initializes");
    if (service.status().state != runtime::GpuPreviewDisplayServiceState::Ready) {
        std::cout << "Unsupported: " << service.status().residentDetail << '\n';
        return options.require_device ? 1 : 77;
    }
    ui::TaskUiBridge bridge(scheduler);
    bool insideCompletion = false;
    std::uint64_t completionSignals = 0, jobsPolls = 0;
    std::vector<std::uint64_t> measuredTerminals;
    std::vector<double> terminalDeliveryMicros;
    QObject::connect(&bridge, &ui::TaskUiBridge::resultsAvailable, &bridge, [&] {
        insideCompletion = true;
        ++completionSignals;
        for (const auto& task : scheduler.snapshots()) {
            if (task.name == "Cache RAM preview frame" && task.finishedAt &&
                std::ranges::find(measuredTerminals, task.id.value()) == measuredTerminals.end()) {
                measuredTerminals.push_back(task.id.value());
                terminalDeliveryMicros.push_back(
                    std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() -
                                                              *task.finishedAt)
                        .count());
            }
        }
    });
    QObject::connect(&bridge, &ui::TaskUiBridge::snapshotsPolled, &bridge, [&] { ++jobsPolls; });
    auto pipeline = ui::makeCompositionPreviewPipeline(fixture.compiler, fixture.evaluator,
                                                       displayPreparer, fixture.provider);
    const ui::PreviewPreparationSubmitter submitter =
        [&service](runtime::TaskRequest request, const document::Snapshot& snapshot,
                   const runtime::PreviewRequestIdentity& identity, std::size_t bytes,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides) {
            return service.submit(std::move(request), snapshot, identity, bytes, overrides);
        };
    const std::size_t hostBudget = partial ? 64ULL << 20 : 1ULL << 30;
    auto cache = std::make_shared<ui::PreviewFrameCache>(hostBudget);
    const auto ownerLimits = service.status().residentCapacityPlan;
    cache->setGpuResidentLimits(ownerLimits.cacheBytes, ownerLimits.cacheEntries);
    ui::CompositionPreviewSettings settings;
    settings.pixelStorageByteLimit = kBudget;
    settings.resolutionPolicy =
        half ? runtime::PreviewResolutionPolicy::Half : runtime::PreviewResolutionPolicy::Full;
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline, settings,
                                                cache, nullptr, submitter);
    ui::RamPreviewController ram(session, controller, scheduler, bridge, pipeline, nullptr,
                                 submitter);
    std::uint64_t observedPrepared = 0, fromCompletion = 0, outsideCompletion = 0;
    QObject::connect(&ram, &ui::RamPreviewController::stateChanged, &bridge, [&] {
        const auto prepared = ram.preparedFrameCount();
        if (prepared > observedPrepared) {
            (insideCompletion ? fromCompletion : outsideCompletion) += prepared - observedPrepared;
            observedPrepared = prepared;
        }
    });
    QObject::connect(&bridge, &ui::TaskUiBridge::resultsAvailable, &bridge,
                     [&] { insideCompletion = false; });
    ui::ViewerEditor viewer(session, controller);
    viewer.resize(600, 420);
    viewer.show();
    checks.expect(
        waitUntil(
            [&] { return viewer.displayedPreviewState().freshness == ui::FrameFreshness::Current; },
            30s),
        "foreground paints genuine GPU-packed sequence before RAM fill");
    checks.expect(viewer.displayedFrameForTest() &&
                      viewer.displayedFrameForTest()->provenance().provider ==
                          runtime::PreviewDisplayProvider::GpuPreparedPacked,
                  "actual normal Viewer foreground is GPU-prepared/CPU-painted");
    const auto before = service.status().counters;
    const auto fillStarted = std::chrono::steady_clock::now();
    ram.start();
    checks.expect(waitUntil([&] { return !ram.isCaching(); }, 110s), "100-frame range terminates");
    checks.expect(
        fromCompletion == ram.preparedFrameCount() && outsideCompletion == 0,
        "every actual RAM result/refill is driven by completion, not Jobs snapshot polling");
    std::ranges::sort(terminalDeliveryMicros);
    std::cout << "PACKED_COMPLETION_REFILL prepared=" << ram.preparedFrameCount()
              << " insideCompletion=" << fromCompletion
              << " outsideCompletion=" << outsideCompletion
              << " completionSignals=" << completionSignals
              << " bridgeWakes=" << bridge.completionWakeCount() << " jobsPolls=" << jobsPolls
              << " fillWallMs="
              << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                           fillStarted)
                     .count();
    if (!terminalDeliveryMicros.empty())
        std::cout << " measuredTerminals=" << terminalDeliveryMicros.size()
                  << " terminalToUiMedianMicros="
                  << terminalDeliveryMicros[terminalDeliveryMicros.size() / 2]
                  << " terminalToUiMaxMicros=" << terminalDeliveryMicros.back();
    std::cout << '\n';
    checks.expect(ram.totalFrameCount() == 100 &&
                      ram.lastCompletion() ==
                          (partial ? ui::RamPreviewController::Completion::PartialBudget
                                   : ui::RamPreviewController::Completion::Complete),
                  "actual budget yields truthful full/partial completion");
    checks.expect(partial ? !ram.canAutoPlay() : ram.canAutoPlay(),
                  "only actual full retention permits autoplay");
    checks.expect(partial ? ram.cachedFrameCount() < 100 : ram.cachedFrameCount() == 100,
                  "live retained exact-time count matches the measured capacity");
    checks.expect(cache->gpuResidentBytes() == 0 && cache->gpuResidentEntryCount() == 0 &&
                      cache->residentBytes() <= hostBudget,
                  "packed retained storage is host-only and fits its actual byte budget");
    std::cout << "PACKED100 resolution=" << (half ? "Half" : "Full") << " hostBudget=" << hostBudget
              << " requested=" << ram.totalFrameCount() << " prepared=" << ram.preparedFrameCount()
              << " retained=" << ram.cachedFrameCount() << " charged=" << cache->residentBytes()
              << " nativeRetained=" << cache->gpuResidentBytes()
              << " ownerCacheBytes=" << ownerLimits.cacheBytes
              << " ownerLeaseBytes=" << ownerLimits.leaseBytes
              << " ownerEntries=" << ownerLimits.cacheEntries << '\n';
    if (ram.playableSpan())
        std::cout << "PACKED_SPAN start=" << ram.playableSpan()->start.toSeconds()
                  << " endExclusive=" << ram.playableSpan()->end.toSeconds()
                  << " frames=" << ram.playableSpan()->frameCount
                  << " autoplay=" << ram.canAutoPlay() << '\n';
    std::vector<core::RationalTime> retainedTimes;
    std::size_t measured = 0;
    const auto retainedIndices = ram.retainedFrameIndices();
    for (const auto index : retainedIndices) {
        const auto time = core::RationalTime::create(static_cast<std::int64_t>(index), 25);
        if (!time) {
            checks.expect(false, "retained frame time validates");
            continue;
        }
        const auto key = controller.cacheKeyForTime(*time);
        if (!key) {
            checks.expect(false, "exact retained frame has an identity");
            continue;
        }
        // Transport requests the real accepted exact key; no fabricated prepared frame or time
        // stamp.
        checks.expect(session.currentTime() == *time || session.setCurrentTime(*time),
                      "retained exact time seeks");
        checks.expect(waitUntil(
                          [&] {
                              const auto actual = viewer.displayedPreviewState();
                              return actual.freshness == ui::FrameFreshness::Current &&
                                     actual.frame && actual.frame->desiredIdentity().time == *time;
                          },
                          5s),
                      "each retained exact time really paints in normal Viewer");
        const auto frame = viewer.displayedFrameForTest();
        if (!frame ||
            frame->provenance().provider != runtime::PreviewDisplayProvider::GpuPreparedPacked) {
            checks.expect(false, "retained time preserves genuine GPU-packed provenance");
            continue;
        }
        const auto view = frame->displayBufferView();
        if (!view) {
            checks.expect(false, "packed frame exposes display bytes");
            continue;
        }
        const auto extent = view->dataWindow.value_or(view->displayWindow).extent();
        const auto width = half ? 960U : 1920U;
        const auto height = half ? 540U : 1080U;
        const auto bytes = frame->displayOnlyFrame()->displayByteCost();
        checks.expect(extent.width() == width && extent.height() == height &&
                          frame->provenance().gpuPreparedPacked->transfer.hostStorageBytes ==
                              static_cast<std::uint64_t>(width) * height * 4 &&
                          key->resolutionPolicy == settings.resolutionPolicy,
                      "physical dimensions/capacity and exact Full/Half cache identity agree");
        measured += bytes;
        const auto snapshot = session.evaluationSnapshotForTime(*time);
        auto oracle = scheduler.submit<ui::PreparedPreviewFrameHandle>(
            runtime::TaskRequest("CPU sequence parity",
                                 {.kind = runtime::TaskOwnerKind::PanelRequest,
                                  .id = runtime::TaskOwnerId::fromRaw(999)}),
            [&fixture, snapshot, identity = frame->desiredIdentity()](runtime::TaskContext&) {
                return runtime::TaskResult<ui::PreparedPreviewFrameHandle>::succeeded(
                    snapshotCpuReferenceAtResolution(fixture, snapshot, identity));
            });
        const auto reference = awaitResult(oracle.handle, 15s);
        checks.expect(reference && reference->value() && samePixels(frame, *reference->value()),
                      "every actual retained GPU image equals its real CPU reference");
        std::cout << "PACKED_FRAME index=" << index << " width=" << width << " height=" << height
                  << " host=" << frame->provenance().gpuPreparedPacked->transfer.hostStorageBytes
                  << " actualCharge=" << bytes
                  << " red=" << static_cast<int>(view->pixels.front().red) << '\n';
        retainedTimes.push_back(*time);
    }
    checks.expect(measured == cache->residentBytes(),
                  "measured product charges equal the live cache ledger");
    const auto beforeWarm = service.status().counters;
    const auto cacheBeforeWarm = cache->residentBytes();
    std::size_t replayed = 0;
    for (int direction = 0; direction < 2; ++direction) {
        for (std::size_t step = 0; step < retainedTimes.size(); ++step) {
            const auto time = retainedTimes[direction ? retainedTimes.size() - 1 - step : step];
            checks.expect(session.currentTime() == time || session.setCurrentTime(time),
                          "warm transport seeks cached time");
            if (waitUntil(
                    [&] {
                        const auto actual = viewer.displayedPreviewState();
                        return actual.freshness == ui::FrameFreshness::Current && actual.frame &&
                               actual.frame->desiredIdentity().time == time;
                    },
                    5s))
                ++replayed;
        }
    }
    const auto afterWarm = service.status().counters;
    checks.expect(replayed == retainedTimes.size() * 2 &&
                      afterWarm.residentGraphJobs == beforeWarm.residentGraphJobs &&
                      afterWarm.nativeDispatches == beforeWarm.nativeDispatches &&
                      afterWarm.packedTransfers == beforeWarm.packedTransfers &&
                      cache->residentBytes() == cacheBeforeWarm,
                  "forward/reverse exact-time Current observations add no GPU "
                  "evaluation/dispatch/transfer or cache growth");
    checks.expect(afterWarm.cpuFallbacks == 0 && afterWarm.packedTransfers > before.packedTransfers,
                  "cold RAM path genuinely dispatches GPU and transfers, without CPU fallback");
    std::cout << "PACKED_WARM replayed=" << replayed
              << " evalDelta=" << afterWarm.residentGraphJobs - beforeWarm.residentGraphJobs
              << " dispatchDelta=" << afterWarm.nativeDispatches - beforeWarm.nativeDispatches
              << " transferDelta=" << afterWarm.packedTransfers - beforeWarm.packedTransfers
              << " transferSource=" << afterWarm.packedRetainedSourceBytes
              << " transferStaging=" << afterWarm.packedRetainedStagingBytes
              << " transferHost=" << afterWarm.packedRetainedHostBytes << '\n';
    ram.beginShutdown();
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
                  "RAM/Viewer/service retire asynchronously");
    std::cout << (checks.failures() ? "FAIL" : "PASS") << ": production packed cache100 "
              << (half ? "Half" : "Full") << '\n';
    return checks.failures() ? 1 : 0;
}
} // namespace
