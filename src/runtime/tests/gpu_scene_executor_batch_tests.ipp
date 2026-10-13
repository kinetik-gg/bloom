// Production batching proof shares the caller/phase harness above. GPU execution stays entirely
// in GpuSceneExecutor; this helper only controls cold/warm inputs, reads telemetry and checks
// pixels.
void runNativeBatchComparison(Expectations& expectations, GpuDevice& device, const bool measured) {
    using TimingStatus = bloom::render::GpuQueueTimingStatus;
    const auto capacity = device.availableAllocationBudget();
    bloom::render::GpuProcessingPhaseBudget root(1);
    root.setHostCapacity(kSceneBudget * 2, true);
    root.setDeviceCapacity(capacity.available_bytes, capacity.available_bytes > 0);
    expectations.expect(device.bindProcessingRoot(root.rootIdentity()),
                        "batch comparison: device binds one stable actual ownership root");
    const CpuCompositionEvaluator oracle;
    auto clock = bloom::render::GpuQueueTiming::create(device);
    const bool clockSupported = clock && clock->sample().status == TimingStatus::Idle;
    const auto sizes =
        measured ? std::vector<std::pair<std::uint32_t, std::uint32_t>>{{1280, 720}, {1920, 1080}}
                 : std::vector<std::pair<std::uint32_t, std::uint32_t>>{{64, 48}};
    for (const auto size : sizes) {
        const auto baseline = device.allocatorMemorySnapshot();
        const double width = static_cast<double>(size.first) / 2;
        const double height = static_cast<double>(size.second) / 2;
        const auto inner = twoLayerPlan(
            format(size.first, size.second), LayerValues{.position = {width / 2, height / 2}},
            LayerValues{.position = {width / 2 + 2, height / 2 + 1}, .opacity = 0.75}, width,
            height, 87000);
        auto definition = inner->copyDefinition();
        auto outer = std::get<bloom::runtime::CompiledLayerOutput>(definition.operations[1]);
        outer.input = bloom::runtime::OperationIndex::fromRaw(4);
        outer.sourceNodeId = bloom::document::NodeId::fromRaw(87900);
        outer.layerId = bloom::document::LayerId::fromRaw(87901);
        outer.position.id = bloom::document::ParameterId::fromRaw(87910);
        outer.anchor.id = bloom::document::ParameterId::fromRaw(87911);
        outer.scale.id = bloom::document::ParameterId::fromRaw(87912);
        outer.rotation.id = bloom::document::ParameterId::fromRaw(87913);
        outer.opacity.id = bloom::document::ParameterId::fromRaw(87914);
        outer.position.source = bloom::document::Vec2d{width + 0.3, height - 0.2};
        outer.rotation.source = 7.25;
        definition.operations[5] = outer;
        definition.operations.emplace_back(bloom::runtime::CompiledMerge{
            bloom::document::NodeId::fromRaw(87902),
            {bloom::runtime::CompiledMergeInput{bloom::document::LayerSlotId::fromRaw(87903),
                                                bloom::document::LayerId::fromRaw(87901),
                                                bloom::runtime::OperationIndex::fromRaw(5)}}});
        definition.operations.emplace_back(bloom::runtime::CompiledCompositionOutput{
            bloom::document::NodeId::fromRaw(87904), bloom::runtime::OperationIndex::fromRaw(6)});
        definition.output = bloom::runtime::OperationIndex::fromRaw(7);
        const auto plan = publish(std::move(definition));
        const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
        auto cpuRequest = requestFor(*plan);
        cpuRequest.bypassOperationCache = true;
        const auto reference = oracle.evaluate(plan, cpuRequest, {});
        auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
        if (!prepared || !reference.frame() || !cache) {
            expectations.expect(false,
                                "batch comparison: actual compiled graph and CPU oracle prepare");
            std::cerr << "batch prep=" << prepared.diagnostic.message
                      << " cpuHasFrame=" << bool(reference.frame()) << '\n';
            return;
        }
        std::array<std::unique_ptr<GpuSceneExecutor>, 2> executors;
        for (std::size_t mode = 0; mode < 2; ++mode) {
            GpuSceneExecutorBudgets budgets;
            budgets.collectQueueTimestamps = true;
            budgets.enableNativeBatches = mode == 1;
            auto created = GpuSceneExecutor::create(device, *cache.cache, budgets);
            if (!created) {
                expectations.expect(
                    false, "batch comparison: same production executor creates in both modes");
                return;
            }
            executors[mode] = std::move(created.executor);
        }
        if (!measured) {
            auto blocker = bloom::render::GpuOperationBatch::create(device);
            auto busy = runBenchmarkScene(device, root, *executors[1], prepared.scene,
                                          std::chrono::milliseconds(0));
            expectations.expect(busy.scene.ready && executors[1]->counters().nativeBatches == 0 &&
                                    executors[1]->counters().nativeBatchFallbacks > 0,
                                "batch slot: occupied owner permits serial native fallback");
            busy.scene.image.reset();
            cache.cache->clear();
            blocker.reset();
            auto retried = runBenchmarkScene(device, root, *executors[1], prepared.scene,
                                             std::chrono::milliseconds(0));
            expectations.expect(
                retried.scene.ready && executors[1]->counters().nativeBatches > 0,
                "batch slot: same executor retries and batches after prior owner releases");
            retried.scene.image.reset();
            cache.cache->clear();
        }
        std::array<std::vector<double>, 2> wall;
        std::array<std::vector<double>, 2> planning;
        std::array<std::vector<double>, 2> queue;
        std::array<std::vector<double>, 2> submit;
        std::array<std::vector<double>, 2> fence;
        std::array<BenchmarkMemory, 2> memory;
        std::uint64_t serialPeak = 0;
        std::array<std::uint64_t, 2> submissions{};
        std::array<std::uint64_t, 2> kernels{};
        const int count = measured ? 15 : 2;
        for (int iteration = 0; iteration < count; ++iteration) {
            std::vector<Rgba32f> serialPixels;
            // Alternating cold run order avoids always giving the batch the warmer queue.
            for (std::size_t order = 0; order < 2; ++order) {
                const auto mode =
                    measured ? (order + static_cast<std::size_t>(iteration % 2)) % 2 : order;
                cache.cache->clear();
                device.reclaimReleasedImages();
                auto& executor = *executors[mode];
                auto cold = runBenchmarkScene(device, root, executor, prepared.scene,
                                              std::chrono::milliseconds(measured ? 2 : 0));
                if (!cold.scene.ready) {
                    expectations.expect(false,
                                        "batch comparison: every cold graph executes natively");
                    std::cerr << cold.scene.begin.message << ' ' << executor.diagnostic().message
                              << '\n';
                    return;
                }
                const auto& after = cold.scene.countersAtReady;
                const auto dispatches = after.dispatches - cold.before.dispatches;
                const auto nativeSubmissions =
                    after.nativeSubmissions - cold.before.nativeSubmissions;
                expectations.expect(
                    dispatches > 1 &&
                        (mode == 0 ? nativeSubmissions == dispatches
                                   : nativeSubmissions < dispatches) &&
                        (mode == 0 ? after.nativeBatches == cold.before.nativeBatches
                                   : after.nativeBatches > cold.before.nativeBatches) &&
                        after.affineDispatches > cold.before.affineDispatches &&
                        after.translationDispatches > cold.before.translationDispatches &&
                        after.sourceOverDispatches > cold.before.sourceOverDispatches &&
                        after.readbacks == cold.before.readbacks,
                    "batch comparison: same genuine kernels share bounded runs with fewer accepted "
                    "submits and "
                    "no intermediate readback");
                const auto timing = benchmarkQueueSample(executor);
                expectations.expect(
                    !clockSupported || timing.status == TimingStatus::Available,
                    "batch comparison: capable device publishes genuine queue-clock span");
                auto pixels = readbackResidentImage(*cold.scene.image, kReadbackBudget);
                expectations.expect(
                    pixels.hasValue() &&
                        pixelsClose(pixels.pixels, reference.frame()->processImage().pixels()),
                    "batch comparison: full native output matches uncached CPU oracle");
                if (!pixels)
                    return;
                if (serialPixels.empty())
                    serialPixels = pixels.pixels;
                else
                    expectations.expect(
                        pixelsExact(pixels.pixels, serialPixels),
                        "batch comparison: serial and batched GPU pixels are byte-exact");
                if (!measured || iteration >= 5) {
                    wall[mode].push_back(cold.wallMilliseconds);
                    planning[mode].push_back(cold.beginPlanMilliseconds);
                    if (timing.status == TimingStatus::Available)
                        queue[mode].push_back(static_cast<double>(timing.nanoseconds) / 1e6);
                    submit[mode].push_back(
                        static_cast<double>(after.processingSubmitNanoseconds -
                                            cold.before.processingSubmitNanoseconds) /
                        1e6);
                    fence[mode].push_back(
                        static_cast<double>(after.processingFenceObservationNanoseconds -
                                            cold.before.processingFenceObservationNanoseconds) /
                        1e6);
                    submissions[mode] = nativeSubmissions;
                    kernels[mode] = dispatches;
                }
                const auto warmBefore = executor.counters();
                const auto warm = runBenchmarkScene(device, root, executor, prepared.scene,
                                                    std::chrono::milliseconds(0));
                expectations.expect(
                    warm.scene.ready &&
                        executor.counters().nativeSubmissions == warmBefore.nativeSubmissions &&
                        executor.counters().dispatches == warmBefore.dispatches &&
                        executor.pollQueueTiming().status == TimingStatus::NotRequested,
                    "batch comparison: unchanged warm graph submits no kernels or diagnostic "
                    "markers");
            }
        }
        // Separate actual allocation observation from timing medians.
        for (std::size_t mode = 0; mode < 2; ++mode) {
            cache.cache->clear();
            auto observed = runBenchmarkScene(device, root, *executors[mode], prepared.scene,
                                              std::chrono::milliseconds(0), true);
            expectations.expect(
                observed.scene.ready &&
                    observed.memory.rootOwnedNative <= observed.memory.rootDeviceClaims,
                "batch memory: actual backing stays within original admitted root grant");
            memory[mode] = observed.memory;
            if (mode == 0)
                serialPeak = observed.scene.countersAtReady.peakLiveImageBytes;
        }
        cache.cache->clear();
        const auto fallbackBefore = executors[1]->counters().nativeBatchFallbacks;
        auto tight = runBenchmarkScene(device, root, *executors[1], prepared.scene,
                                       std::chrono::milliseconds(0), false, serialPeak);
        expectations.expect(tight.scene.ready &&
                                executors[1]->counters().nativeBatchFallbacks > fallbackBefore,
                            "batch admission: measured serial headroom refuses oversized run and "
                            "succeeds via serial native under original phase");
        tight.scene.image.reset();
        cache.cache->clear();
        executors = {};
        device.reclaimReleasedImages();
        const auto settled = device.allocatorMemorySnapshot();
        expectations.expect(
            root.snapshot().totalDeviceClaimBytes == 0 &&
                root.snapshot().totalHostClaimBytes == 0 &&
                settled.allocationBytes == baseline.allocationBytes,
            "batch memory: purge/retirement restores native payload and root claims");
        const auto queueSummary = [](const std::vector<double>& samples) {
            return samples.empty() ? std::string{"Unavailable"}
                                   : std::to_string(medianValue(samples));
        };
        std::cout << "BATCH_COMPARE size=" << size.first << 'x' << size.second
                  << " serialWallMs=" << medianValue(wall[0])
                  << " batchWallMs=" << medianValue(wall[1])
                  << " serialQueueMs=" << queueSummary(queue[0])
                  << " batchQueueMs=" << queueSummary(queue[1])
                  << " serialPlanMs=" << medianValue(planning[0])
                  << " batchPlanMs=" << medianValue(planning[1])
                  << " queueSamples=" << queue[0].size() << '/' << queue[1].size()
                  << " serialSubmitMs=" << medianValue(submit[0])
                  << " batchSubmitMs=" << medianValue(submit[1])
                  << " serialFenceMs=" << medianValue(fence[0])
                  << " batchFenceMs=" << medianValue(fence[1])
                  << " serialSubmissions=" << submissions[0]
                  << " batchSubmissions=" << submissions[1] << " serialKernels=" << kernels[0]
                  << " batchKernels=" << kernels[1] << " serialPayloadPeak=" << memory[0].vmaPayload
                  << " batchPayloadPeak=" << memory[1].vmaPayload
                  << " serialOwnedPeak=" << memory[0].rootOwnedNative
                  << " batchOwnedPeak=" << memory[1].rootOwnedNative
                  << " serialBlocksPeak=" << memory[0].vmaBlocks
                  << " batchBlocksPeak=" << memory[1].vmaBlocks
                  << " settledPayload=" << settled.allocationBytes
                  << " settledBlocks=" << settled.blockBytes << '\n';
    }
}
