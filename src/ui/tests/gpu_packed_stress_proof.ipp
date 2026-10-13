// Opt-in sustained qualification of the ordinary packed Viewer, on a private compositor only.
namespace {
using PackedStressClock = std::chrono::steady_clock;

template <typename Value>
Value packedStressWorkerResult(std::future<Value>& future, const std::chrono::seconds timeout) {
    if (!waitUntil([&] { return future.wait_for(0s) == std::future_status::ready; }, timeout)) {
        // A fixture failure must neither join a live worker on the UI nor let its borrowed
        // references outlive the fixture. Terminate this isolated test process on that failure.
        std::cerr << "FAIL: sustained worker exceeded its deadline\n" << std::flush;
        std::quick_exit(EXIT_FAILURE);
    }
    return future.get();
}

bool packedStressCurrent(ui::ViewerEditor& viewer, ui::CompositionPreviewController& controller,
                         ui::CompositionSession& session) {
    const auto shown = viewer.displayedPreviewState();
    const auto desired = controller.state().desiredIdentity;
    return shown.freshness == ui::FrameFreshness::Current && shown.frame && desired &&
           shown.frame->desiredIdentity() == *desired && desired->time == session.currentTime() &&
           shown.frame->provenance().provider == runtime::PreviewDisplayProvider::GpuPreparedPacked;
}

void packedStressLatency(Expectations& checks, const char* operation,
                         const PackedStressClock::time_point started, ui::ViewerEditor& viewer,
                         ui::CompositionPreviewController& controller,
                         ui::CompositionSession& session) {
    const bool current =
        waitUntil([&] { return packedStressCurrent(viewer, controller, session); }, 15s);
    const auto elapsed =
        std::chrono::duration<double, std::milli>(PackedStressClock::now() - started);
    checks.expect(current, "loaded interaction reaches the exact genuine GPU-packed paint");
    std::cout << "PACKED_LATENCY operation=" << operation
              << " inputToCpuPaintMs=" << elapsed.count() << " exact=" << current
              << " endpoint=widgetPaint scanout=unmeasured\n";
}

void packedStressInput(Expectations& checks, Fixture& fixture, runtime::TaskScheduler& scheduler,
                       ui::ViewerEditor& viewer, ui::CompositionPreviewController& controller,
                       ui::CompositionSession& session, ui::RamPreviewController& ram,
                       const std::uint32_t rate) {
    checks.expect(ram.isCaching(), "interaction begins during actual RAM fill");
    // A scrub keeps the fill running. Selection uses real widget events and the actually painted
    // mapping. A document-changing drag is expected to cancel the old range, then start fresh.
    auto started = PackedStressClock::now();
    const auto target = core::RationalTime::create(7, rate);
    checks.expect(target && session.setCurrentTime(*target), "loaded scrub requests real time");
    packedStressLatency(checks, "scrubDuringFill", started, viewer, controller, session);
    const auto mapping = viewer.mappingForTest();
    if (!mapping) {
        checks.expect(false, "loaded gesture has a painted mapping");
        return;
    }
    const auto point = mapping->displayRect.center();
    const auto send = [&](QEvent::Type type, const QPointF local) {
        QMouseEvent event(type, local, viewer.mapToGlobal(local),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(&viewer, &event);
    };
    started = PackedStressClock::now();
    send(QEvent::MouseButtonPress, point);
    checks.expect(session.transformInteractionActive(), "loaded press selects real image geometry");
    std::cout
        << "PACKED_LATENCY operation=selectionDuringFill eventFeedbackMicros="
        << std::chrono::duration<double, std::micro>(PackedStressClock::now() - started).count()
        << " fillStillActive=" << ram.isCaching() << '\n';
    started = PackedStressClock::now();
    send(QEvent::MouseMove, point + QPointF{12, 8});
    packedStressLatency(checks, "heldDragDuringFill", started, viewer, controller, session);
    const auto shown = viewer.displayedFrameForTest();
    checks.expect(shown && !controller.selectedLayerBounds(shown).empty(),
                  "loaded held drag publishes evaluated outline with the painted image");
    packedAnimationOracle(checks, fixture, scheduler, session, viewer);
    started = PackedStressClock::now();
    send(QEvent::MouseButtonRelease, point + QPointF{12, 8});
    packedStressLatency(checks, "committedDrag", started, viewer, controller, session);
    checks.expect(!session.transformInteractionActive() && !ram.isCaching(),
                  "committed edit cancels old RAM range instead of mixing revisions");
    checks.expect(session.undo(), "loaded drag is one undoable transaction");
    checks.expect(waitUntil([&] { return packedStressCurrent(viewer, controller, session); }, 15s),
                  "loaded undo restores coherent pixels and outline");
}

struct PackedStressExportResult final {
    std::uint64_t completed = 0, nativeDispatches = 0, readbacks = 0;
    double evaluationMs = 0, encodingMs = 0;
    bool failed = false;
    std::optional<runtime::GpuProcessFrameStageTimes> firstStages;
};

void exercisePackedStress(Expectations& checks, Fixture& fixture, const TestOptions& options,
                          QTemporaryDir& temporary, ui::ViewerEditor& viewer,
                          ui::CompositionSession& session,
                          ui::CompositionPreviewController& controller,
                          ui::RamPreviewController& ram, runtime::TaskScheduler& scheduler,
                          runtime::GpuPreviewDisplayService& service,
                          const std::shared_ptr<runtime::GpuProcessingPhasePolicy>& policy,
                          const std::shared_ptr<runtime::GpuSceneCoverageCache>& coverage,
                          const std::shared_ptr<runtime::GpuPreparedUploadCache>& prepared,
                          const std::vector<core::RationalTime>& retainedTimes,
                          const std::uint32_t rate, const std::size_t processingBudget,
                          const std::size_t preparationWidth, document::Document& originalDocument,
                          commands::CommandStack& originalCommands) {
    checks.expect(!retainedTimes.empty(), "sustained proof starts with actual retained frames");
    if (retainedTimes.empty())
        return;
    const auto purge = [&] {
        controller.setCachePurgeGate(true);
        controller.purgePreviewCache();
        checks.expect(waitUntil([&] { return scheduler.isQuiescent(); }, 30s),
                      "purge gates producers and awaits real task retirement");
        auto cleared = std::async(std::launch::async, [&] {
            fixture.evaluator.operationCache()->clearOperations();
            fixture.evaluator.operationCache()->clearDecodedMedia();
            fixture.evaluator.operationCache()->clearPreparedUploads();
            coverage->clear();
            prepared->clear();
            return service.purgeRetainedCaches(5s);
        });
        checks.expect(packedStressWorkerResult(cleared, 10s),
                      "media/native purge and large frees run off the UI thread");
        const auto purgedAt = PackedStressClock::now();
        checks.expect(
            waitUntil([&] { return service.status().allocatorMemorySampledAt >= purgedAt; }, 3s),
            "settled allocator/driver report is actually sampled after purge");
        checks.expect(waitUntil(
                          [&] {
                              const auto claims = policy->snapshot().reservations;
                              return claims.admittedPhases == 0 && claims.unprovenPhases == 0 &&
                                     claims.ownedPackedHostAllocations <= 1 &&
                                     service.status().counters.packedRetainedStagingBytes == 0;
                          },
                          30s),
                      "purge settles actual retirement while preserving one displayed picture");
    };
    purge();
    controller.setCachePurgeGate(false);
    checks.expect(waitUntil([&] { return packedStressCurrent(viewer, controller, session); }, 15s),
                  "fresh foreground recovers after explicit source refresh");

    // A real final-render owner and actual EXR writer compete with preview, with one final readback
    // per frame. Snapshot/compile/source access/encoding all stay on this worker.
    const auto snapshot = session.snapshot();
    const auto composition = session.compositionId();
    std::atomic_bool exportBusy{false}, stopExport{false};
    auto exporting = std::async(std::launch::async, [&] {
        PackedStressExportResult result;
        runtime::GpuProcessFrameEvaluatorOptions finalOptions;
        finalOptions.loaderPath = options.loader_path;
        finalOptions.processingPhasePolicy = policy;
        finalOptions.collectQueueTimestamps = true;
        finalOptions.requestByteBudget = processingBudget;
        finalOptions.readbackByteBudget = processingBudget;
        finalOptions.ocioResolver = host::makePackagedGpuOcioResolver(
            QCoreApplication::applicationFilePath().toStdString());
        if (finalOptions.ocioResolver)
            finalOptions.ocioContext = finalOptions.ocioResolver->resolve().context;
        finalOptions.mediaContext = runtime::GpuSceneMediaContext::fromEvaluator(fixture.evaluator);
        auto evaluator = runtime::GpuProcessFrameEvaluator::create(finalOptions);
        if (!evaluator || !evaluator->gpuAvailable()) {
            result.failed = true;
            return result;
        }
        const auto compiled =
            fixture.compiler.compile({.snapshot = snapshot, .compositionId = composition}, {});
        if (!compiled.plan) {
            result.failed = true;
            return result;
        }
        exportBusy.store(true);
        const auto deadline = PackedStressClock::now() + 90s;
        do {
            const auto time = retainedTimes[result.completed % retainedTimes.size()];
            const auto started = PackedStressClock::now();
            auto frame =
                evaluator->evaluate(compiled.plan, {.time = time,
                                                    .output = compiled.plan->output(),
                                                    .resolution = {},
                                                    .pixelStorageByteLimit = processingBudget});
            const auto encoded = PackedStressClock::now();
            if (!result.firstStages)
                result.firstStages = frame.stageTimes;
            if (!frame.frame || frame.status != runtime::GpuProcessFrameStatus::Evaluated ||
                frame.counters.readbacks != 1) {
                result.failed = true;
                break;
            }
            const output::FlatExrRgba32fLinRec709SceneWriterV1 writer;
            const auto written = writer.write(
                *frame.frame, std::filesystem::path(temporary.path().toStdString()) / "stress.exr",
                {});
            result.evaluationMs +=
                std::chrono::duration<double, std::milli>(encoded - started).count();
            result.encodingMs +=
                std::chrono::duration<double, std::milli>(PackedStressClock::now() - encoded)
                    .count();
            // Executor telemetry is cumulative for this dedicated owner; don't sum snapshots.
            result.nativeDispatches = frame.counters.nativeDispatches;
            result.readbacks += frame.counters.readbacks;
            if (written.status() != output::FlatExrWriteStatusV1::Written) {
                result.failed = true;
                break;
            }
            ++result.completed;
        } while (!stopExport.load() && PackedStressClock::now() < deadline);
        exportBusy.store(false);
        // Destruction joins here on the CPU worker, never on the active UI event loop.
        evaluator->beginShutdown();
        return result;
    });
    checks.expect(waitUntil(
                      [&] {
                          return exportBusy.load() ||
                                 exporting.wait_for(0s) == std::future_status::ready;
                      },
                      30s) &&
                      exportBusy.load(),
                  "real final-render/export competition starts");
    ram.start();
    packedStressInput(checks, fixture, scheduler, viewer, controller, session, ram, rate);
    viewer.resize(880, 560);
    QApplication::processEvents();
    viewer.resize(480, 360);
    checks.expect(waitUntil([&] { return packedStressCurrent(viewer, controller, session); }, 15s),
                  "loaded resize preserves exact painted identity");
    stopExport.store(true);
    const auto exportResult = packedStressWorkerResult(exporting, 40s);
    checks.expect(!exportResult.failed && exportResult.completed > 0 &&
                      exportResult.nativeDispatches > 0 &&
                      exportResult.readbacks == exportResult.completed,
                  "competing export uses real GPU dispatch, one final transfer and actual codec");
    if (exportResult.firstStages) {
        const auto& times = *exportResult.firstStages;
        std::cout << "PACKED_FINAL_STAGES ioNs=" << times.media.sourceIoNanoseconds
                  << " hashNs=" << times.media.sourceHashNanoseconds
                  << " probeNs=" << times.media.sourceProbeNanoseconds
                  << " decodeNs=" << times.media.imageDecodeNanoseconds
                  << " uploadSubmitNs=" << times.uploadSubmitNanoseconds
                  << " uploadFenceObservationNs=" << times.uploadFenceObservationNanoseconds
                  << " processingSubmitNs=" << times.processingSubmitNanoseconds
                  << " processingFenceObservationNs=" << times.processingFenceObservationNanoseconds
                  << " finalTransferNs=" << times.finalTransferNanoseconds
                  << " finalHostPublicationNs=" << times.finalHostPublicationNanoseconds
                  << " gpuGraphTimingStatus="
                  << static_cast<unsigned>(times.graphQueueTiming.status)
                  << " gpuGraphQueueSpanNs=" << times.graphQueueTiming.nanoseconds
                  << " gpuTimestampBits=" << times.graphQueueTiming.timestampValidBits
                  << " gpuTimestampPeriodNs=" << times.graphQueueTiming.timestampPeriodNanoseconds
                  << " queueIdleAndSubmitGapsIncluded=1\n";
    }
    std::cout << "PACKED_EXPORT completed=" << exportResult.completed
              << " nativeDispatches=" << exportResult.nativeDispatches
              << " readbacks=" << exportResult.readbacks
              << " evaluationMs=" << exportResult.evaluationMs
              << " encodingAndFileIoMs=" << exportResult.encodingMs << '\n';

    std::optional<render::GpuProcessingPhaseSnapshot> settledBaseline;
    for (int cycle = 0; cycle < 10; ++cycle) {
        purge();
        const auto settled = policy->snapshot().reservations;
        const auto settledAllocator = service.status().allocatorMemory;
        if (!settledBaseline)
            settledBaseline = settled;
        checks.expect(settled.totalHostClaimBytes <= settledBaseline->totalHostClaimBytes &&
                          settled.totalDeviceClaimBytes <= settledBaseline->totalDeviceClaimBytes &&
                          settled.ownedNativeBufferBytes <= settledBaseline->ownedNativeBufferBytes,
                      "equivalent settled purge claims never grow across ten cycles");
        controller.setCachePurgeGate(false);
        checks.expect(
            waitUntil([&] { return packedStressCurrent(viewer, controller, session); }, 20s),
            "cycle foreground reaches genuine Current before fill");
        const auto before = service.status().counters;
        const auto started = PackedStressClock::now();
        std::uint64_t peakHostClaims = 0, peakDeviceClaims = 0;
        std::size_t peakPhases = 0;
        ram.start();
        checks.expect(waitUntil(
                          [&] {
                              const auto active = policy->snapshot().reservations;
                              peakHostClaims = std::max(peakHostClaims, active.totalHostClaimBytes);
                              peakDeviceClaims =
                                  std::max(peakDeviceClaims, active.totalDeviceClaimBytes);
                              peakPhases = std::max(peakPhases, active.admittedPhases);
                              return !ram.isCaching();
                          },
                          110s) &&
                          ram.canAutoPlay(),
                      "equivalent fresh cycle fills the whole retained range");
        const auto filledAt = PackedStressClock::now();
        checks.expect(waitUntil(
                          [&] {
                              const auto timing = service.status().packedTransferQueueTiming.status;
                              return timing != render::GpuQueueTimingStatus::Pending &&
                                     timing != render::GpuQueueTimingStatus::Active;
                          },
                          1s),
                      "diagnostic transfer sample resolves without gating painted pixels");
        checks.expect(
            waitUntil([&] { return service.status().allocatorMemorySampledAt >= filledAt; }, 3s),
            "post-fill allocator/driver report follows completed native work");
        const auto afterStatus = service.status();
        const auto after = afterStatus.counters;
        checks.expect(after.cpuFallbacks == 0 && after.packedTransfers > before.packedTransfers,
                      "each purged cycle executes native processing and actual transfer");
        // RSS is independent observed process memory, never added to or substituted for allocation
        // claims. Linux /proc is read only on this worker. Other hosts report unavailable.
        auto rss = std::async(std::launch::async, [] {
            QFile status("/proc/self/status");
            if (!status.open(QIODevice::ReadOnly))
                return QByteArray("unavailable");
            for (const auto& line : status.readAll().split('\n')) {
                if (line.startsWith("VmRSS:"))
                    return line.trimmed();
            }
            return QByteArray("unavailable");
        });
        const auto claims = policy->snapshot().reservations;
        std::cout
            << "PACKED_CYCLE index=" << cycle + 1 << " width=" << preparationWidth
            << " retained=" << ram.cachedFrameCount()
            << " fillMs=" << std::chrono::duration<double, std::milli>(filledAt - started).count()
            << " settledHostClaims=" << settled.totalHostClaimBytes
            << " settledDeviceClaims=" << settled.totalDeviceClaimBytes
            << " peakHostClaims=" << peakHostClaims << " peakDeviceClaims=" << peakDeviceClaims
            << " peakPhases=" << peakPhases << " packedPayload=" << claims.ownedPackedHostBytes
            << " uploadSubmitNs=" << after.uploadSubmitNanoseconds - before.uploadSubmitNanoseconds
            << " uploadFenceObservationNs="
            << after.uploadFenceObservationNanoseconds - before.uploadFenceObservationNanoseconds
            << " processingSubmitNs="
            << after.processingSubmitNanoseconds - before.processingSubmitNanoseconds
            << " processingFenceObservationNs="
            << after.processingFenceObservationNanoseconds -
                   before.processingFenceObservationNanoseconds
            << " packedTransferNs="
            << after.packedTransferNanoseconds - before.packedTransferNanoseconds
            << " nativeImages=" << claims.ownedNativeImageBytes
            << " graphQueueSpanNs=" << afterStatus.graphQueueTiming.nanoseconds
            << " graphTimingStatus=" << static_cast<unsigned>(afterStatus.graphQueueTiming.status)
            << " transferQueueSpanNs=" << afterStatus.packedTransferQueueTiming.nanoseconds
            << " transferTimingStatus="
            << static_cast<unsigned>(afterStatus.packedTransferQueueTiming.status)
            << " nativeBuffers=" << claims.ownedNativeBufferBytes
            << " rss=" << packedStressWorkerResult(rss, 2s).toStdString()
            << " settledAllocatorPayload=" << settledAllocator.allocationBytes
            << " settledAllocatorBlocks=" << settledAllocator.blockBytes
            << " allocatorPayload=" << afterStatus.allocatorMemory.allocationBytes
            << " allocatorBlocks=" << afterStatus.allocatorMemory.blockBytes
            << " driverBudgetStatus="
            << static_cast<unsigned>(afterStatus.allocatorMemory.driverBudgetStatus)
            << " driverHeaps=" << afterStatus.allocatorMemory.deviceLocalHeapCount << '\n';
        for (std::size_t heap = 0; heap < afterStatus.allocatorMemory.deviceLocalHeapCount;
             ++heap) {
            const auto& value = afterStatus.allocatorMemory.deviceLocalHeaps[heap];
            std::cout << "PACKED_DRIVER_HEAP cycle=" << cycle + 1 << " heap=" << value.heapIndex
                      << " usage=" << value.usageBytes << " budget=" << value.budgetBytes
                      << " ownership=notAttributed\n";
        }
    }
    const auto beforePlayback = service.status().counters;
    const auto beforeClaims = policy->snapshot().reservations;
    auto& playback = controller.playbackController();
    playback.setLooping(true);
    playback.play();
    const auto started = PackedStressClock::now();
    auto lastAdvance = started;
    bool continuouslyPlaying = true;
    std::uint64_t observed = 0;
    std::optional<core::RationalTime> previous;
    checks.expect(waitUntil(
                      [&] {
                          const auto shown = viewer.displayedFrameForTest();
                          if (shown && (!previous || *previous != shown->desiredIdentity().time)) {
                              ++observed;
                              lastAdvance = PackedStressClock::now();
                              previous = shown->desiredIdentity().time;
                              checks.expect(
                                  shown->provenance().provider ==
                                          runtime::PreviewDisplayProvider::GpuPreparedPacked &&
                                      std::ranges::find(retainedTimes, *previous) !=
                                          retainedTimes.end(),
                                  "sustained playback paints retained exact GPU products");
                          }
                          continuouslyPlaying = continuouslyPlaying &&
                                                playback.state() == ui::PlaybackState::Playing &&
                                                PackedStressClock::now() - lastAdvance < 1s;
                          return PackedStressClock::now() - started >= 60s;
                      },
                      65s),
                  "real retained transport remains active for sixty seconds");
    playback.pause();
    checks.expect(waitUntil([&] { return packedStressCurrent(viewer, controller, session); }, 5s),
                  "sustained playback settles at exact requested paint");
    const auto afterPlayback = service.status().counters;
    const auto afterClaims = policy->snapshot().reservations;
    checks.expect(continuouslyPlaying && observed >= 30ULL * rate &&
                      afterPlayback.nativeDispatches == beforePlayback.nativeDispatches &&
                      afterPlayback.packedTransfers == beforePlayback.packedTransfers &&
                      afterClaims.ownedPackedHostBytes == beforeClaims.ownedPackedHostBytes &&
                      afterClaims.ownedPackedHostAllocations ==
                          beforeClaims.ownedPackedHostAllocations,
                  "sixty-second warm playback performs no render, transfer or retained growth");
    std::cout << "PACKED_SUSTAINED seconds=60 observedPaintedTimes=" << observed
              << " observedPaintedTimesPerSecond=" << static_cast<double>(observed) / 60.0
              << " continuouslyPlaying=" << continuouslyPlaying << " looped=1 scanout=unmeasured"
              << " dispatchDelta="
              << afterPlayback.nativeDispatches - beforePlayback.nativeDispatches
              << " transferDelta=" << afterPlayback.packedTransfers - beforePlayback.packedTransfers
              << " packedPayload=" << afterClaims.ownedPackedHostBytes << '\n';
    const auto originalComposition = session.compositionId();
    const auto format = session.composition()->format();
    auto replacement = document::makeNewProject("Sustained replacement", "Main",
                                                core::RationalTime::fromInteger(1), format);
    document::Document replacementDocument(std::move(replacement.project));
    commands::CommandStack replacementCommands(replacementDocument);
    controller.purgePreviewCache();
    ram.start();
    checks.expect(ram.isCaching(), "project replacement starts during an actual active refill");
    const auto replacementStarted = PackedStressClock::now();
    session.rebind(replacementDocument, replacementCommands, replacement.initialCompositionId);
    checks.expect(!viewer.displayedFrameForTest() && !ram.isCaching(),
                  "project replacement immediately detaches old picture and in-flight range");
    checks.expect(session.addSolidLayer("Replacement", {0.2, 0.7, 0.1, 1.0}),
                  "replacement authors a real production GPU scene");
    packedStressLatency(checks, "projectReplacementDuringFill", replacementStarted, viewer,
                        controller, session);
    session.rebind(originalDocument, originalCommands, originalComposition);
    checks.expect(waitUntil([&] { return packedStressCurrent(viewer, controller, session); }, 20s),
                  "restored project rejects late replacement products and paints its own image");
}
} // namespace
