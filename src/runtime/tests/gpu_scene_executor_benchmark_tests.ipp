// GPU scene executor boundary benchmark (CPU preparation/evaluation vs native begin-to-Ready).
// Included by gpu_scene_executor_tests.cpp inside its anonymous namespace.

// ---- benchmark ---------------------------------------------------------------------------------

[[nodiscard]] double medianMilliseconds(std::vector<double> samples) {
    if (samples.empty()) {
        return 0.0;
    }
    std::ranges::sort(samples);
    const std::size_t middle = samples.size() / 2;
    if (samples.size() % 2 == 0) {
        return (samples[middle - 1] + samples[middle]) / 2.0;
    }
    return samples[middle];
}

void runBenchmark(Expectations& expectations, GpuDevice& device) {
    const CpuCompositionEvaluator oracle;
    struct Size final {
        std::uint32_t width;
        std::uint32_t height;
    };
    for (const Size size : {Size{1280, 720}, Size{1920, 1080}}) {
        const auto compositionFormat = format(size.width, size.height);
        const auto halfWidth = static_cast<double>(size.width) / 2.0;
        const auto halfHeight = static_cast<double>(size.height) / 2.0;
        const auto plan = twoLayerPlan(
            compositionFormat, LayerValues{.position = {halfWidth * 0.45, halfHeight * 0.5}},
            LayerValues{.position = {halfWidth * 0.62, halfHeight * 0.42}, .opacity = 0.75},
            halfWidth, halfHeight, 77000);
        const auto request = requestFor(*plan);
        const CpuGpuSceneBuilder builder;

        std::vector<double> prepareSamples;
        std::vector<double> cpuSamples;
        for (int iteration = 0; iteration < 15; ++iteration) {
            const auto start = std::chrono::steady_clock::now();
            const auto prepared = builder.build(plan, request);
            const auto afterPrepare = std::chrono::steady_clock::now();
            if (!prepared) {
                expectations.expect(false, "benchmark: the scene prepares");
                return;
            }
            if (iteration >= 5) {
                prepareSamples.push_back(
                    std::chrono::duration<double, std::milli>(afterPrepare - start).count());
            }
            auto cpuRequest = request;
            cpuRequest.bypassOperationCache = true;
            const auto frame = oracle.evaluate(plan, cpuRequest, {});
            const auto afterCpu = std::chrono::steady_clock::now();
            expectations.expect(frame.frame() != nullptr, "benchmark: the CPU frame evaluates");
            if (iteration >= 5) {
                cpuSamples.push_back(
                    std::chrono::duration<double, std::milli>(afterCpu - afterPrepare).count());
            }
        }

        auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
        expectations.expect(cache.hasValue(), "benchmark: cache created");
        if (!cache) {
            return;
        }
        auto executor = GpuSceneExecutor::create(device, *cache.cache);
        expectations.expect(executor.hasValue(), "benchmark: executor created");
        if (!executor) {
            return;
        }
        const auto prepared = builder.build(plan, request);
        if (!prepared) {
            expectations.expect(false, "benchmark: the reusable scene prepares");
            return;
        }
        std::vector<double> uncachedSamples;
        std::vector<double> warmSamples;
        for (int iteration = 0; iteration < 15; ++iteration) {
            cache.cache->clear();
            const auto start = std::chrono::steady_clock::now();
            const auto run = runScene(*executor.executor, prepared.scene, kSceneBudget);
            const auto elapsed =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count();
            expectations.expect(run.ready, "benchmark: the native scene completes");
            if (!run.ready) {
                return;
            }
            if (iteration >= 5) {
                uncachedSamples.push_back(elapsed);
            }
            const auto warmStart = std::chrono::steady_clock::now();
            const auto warm = runScene(*executor.executor, prepared.scene, kSceneBudget);
            const auto warmElapsed = std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - warmStart)
                                         .count();
            expectations.expect(warm.ready, "benchmark: the warm scene completes");
            if (!warm.ready) {
                return;
            }
            if (iteration >= 5) {
                warmSamples.push_back(warmElapsed);
            }
            cache.cache->clear();
        }

        std::cout << std::fixed << std::setprecision(3) << size.width << 'x' << size.height
                  << " CPU-prepare=" << medianMilliseconds(prepareSamples)
                  << "ms CPU-eval=" << medianMilliseconds(cpuSamples)
                  << "ms native-uncached=" << medianMilliseconds(uncachedSamples)
                  << "ms native-warm-cache=" << medianMilliseconds(warmSamples) << "ms\n";
    }
    std::cout << "Boundaries: CPU preparation (CpuGpuSceneBuilder) and CPU evaluation are measured "
                 "separately from native begin-to-Ready; viewer/presentation excluded. No "
                 "application FPS claim.\n";
}
