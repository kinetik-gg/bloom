// GPU scene executor cache tests: warm output hits, unchanged-subtree retention, same-content
// identity, and descriptor validation of a cached output. Included by gpu_scene_executor_tests.cpp
// inside its anonymous namespace so the shared fixture helpers and oracle stay one translation
// unit.

void testWarmCache(Expectations& expectations, GpuDevice& device) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "warm: cache created");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "warm: executor created");
    if (!executor) {
        return;
    }
    const auto plan = basicPlan();
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(prepared.hasValue(), "warm: scene prepares");
    if (!prepared) {
        return;
    }
    const auto first = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(first.ready, "warm: the first scene completes");
    if (!first.ready) {
        return;
    }
    const auto afterFirst = executor.executor->counters();
    expectations.expect(afterFirst.outputCacheMisses == 1,
                        "warm: the first run is an output cache miss");
    expectations.expect(afterFirst.dispatches > 0, "warm: the first run dispatches");

    const auto equivalent = basicPlan();
    const auto secondPrepared = CpuGpuSceneBuilder{}.build(equivalent, requestFor(*equivalent));
    const auto second = runScene(*executor.executor, secondPrepared.scene, kSceneBudget);
    expectations.expect(second.ready, "warm: the second scene completes");
    const auto afterSecond = executor.executor->counters();
    expectations.expect(afterSecond.outputCacheHits == 1,
                        "warm: the unchanged output is a cache hit");
    expectations.expect(afterSecond.dispatches == afterFirst.dispatches,
                        "warm: an unchanged output performs zero native dispatches");
    expectations.expect(second.image != nullptr && first.image != nullptr &&
                            second.image.get() == first.image.get(),
                        "warm: the returned image is the cached resident image");
}

// The image of the merge's bottom-most foreground: for a fractional layer this is the covered
// command's image (the original solid command is deliberately unused).
[[nodiscard]] std::shared_ptr<const GpuImage> bottomLayerImage(GpuSceneCache& cache,
                                                               const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* merge = std::get_if<GpuSceneMergeCommand>(&command)) {
            if (merge->foregrounds.empty()) {
                continue;
            }
            const auto bottom = merge->foregrounds.front();
            if (bottom != bloom::runtime::kInvalidGpuSceneCommand &&
                static_cast<std::size_t>(bottom) < scene.commands().size()) {
                return cache.find(keyOf(scene.commands()[bottom]));
            }
        }
    }
    return nullptr;
}

void testChangedTopRetainsLower(Expectations& expectations, GpuDevice& device) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "changed: cache created");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "changed: executor created");
    if (!executor) {
        return;
    }
    const auto firstPlan = basicPlan();
    const auto firstPrepared = CpuGpuSceneBuilder{}.build(firstPlan, requestFor(*firstPlan));
    expectations.expect(firstPrepared.hasValue(), "changed: the first scene prepares");
    if (!firstPrepared) {
        return;
    }
    const auto first = runScene(*executor.executor, firstPrepared.scene, kSceneBudget);
    expectations.expect(first.ready, "changed: the first scene completes");
    if (!first.ready) {
        return;
    }
    const auto lowerImage = bottomLayerImage(*cache.cache, *firstPrepared.scene);
    expectations.expect(lowerImage != nullptr, "changed: the lower layer image is cached");

    // The merge foregrounds are bottom-to-top, so the FIRST merge entry is the top layer. Move only
    // that top layer; the second (bottom) entry is unchanged.
    const auto movedPlan =
        twoLayerPlan(format(16, 12), LayerValues{.position = {13.0, 9.5}, .opacity = 1.0},
                     LayerValues{.position = {11.5, 8.2}, .opacity = 0.75}, 6.0, 5.0, 1000);
    const auto movedPrepared = CpuGpuSceneBuilder{}.build(movedPlan, requestFor(*movedPlan));
    expectations.expect(movedPrepared.hasValue(), "changed: the moved scene prepares");
    if (!movedPrepared) {
        return;
    }
    const auto before = executor.executor->counters();
    const auto moved = runScene(*executor.executor, movedPrepared.scene, kSceneBudget);
    expectations.expect(moved.ready, "changed: the moved scene completes");
    const auto after = executor.executor->counters();
    expectations.expect(after.commandCacheHits > before.commandCacheHits,
                        "changed: the unchanged lower subtree is served from cache");
    expectations.expect(after.dispatches - before.dispatches < 8,
                        "changed: the cached lower subtree saves at least one dispatch");
    const auto lowerAfter = bottomLayerImage(*cache.cache, *movedPrepared.scene);
    expectations.expect(lowerAfter != nullptr && lowerImage != nullptr &&
                            lowerAfter.get() == lowerImage.get(),
                        "changed: the lower cached image is retained unchanged");
}

void testSameContentDifferentIdentities(Expectations& expectations, GpuDevice& device) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "identity: cache created");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "identity: executor created");
    if (!executor) {
        return;
    }
    const auto firstPlan = basicPlan();
    const auto secondPlan =
        twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}, .opacity = 1.0},
                     LayerValues{.position = {11.5, 8.2}, .opacity = 0.75}, 6.0, 5.0, 900000);
    const auto firstPrepared = CpuGpuSceneBuilder{}.build(firstPlan, requestFor(*firstPlan));
    const auto secondPrepared = CpuGpuSceneBuilder{}.build(secondPlan, requestFor(*secondPlan));
    expectations.expect(firstPrepared.hasValue() && secondPrepared.hasValue(),
                        "identity: both plans prepare");
    if (!firstPrepared || !secondPrepared) {
        return;
    }
    std::vector<std::string> firstKeys;
    std::vector<std::string> secondKeys;
    for (const auto& command : firstPrepared.scene->commands()) {
        firstKeys.push_back(keyOf(command));
    }
    for (const auto& command : secondPrepared.scene->commands()) {
        secondKeys.push_back(keyOf(command));
    }
    std::ranges::sort(firstKeys);
    std::ranges::sort(secondKeys);
    expectations.expect(firstKeys == secondKeys,
                        "identity: different node/parameter IDs keep identical semantic keys");

    const auto first = runScene(*executor.executor, firstPrepared.scene, kSceneBudget);
    expectations.expect(first.ready, "identity: the first scene completes");
    if (!first.ready) {
        return;
    }
    const auto before = executor.executor->counters();
    const auto second = runScene(*executor.executor, secondPrepared.scene, kSceneBudget);
    expectations.expect(second.ready, "identity: the same-content scene completes");
    const auto after = executor.executor->counters();
    expectations.expect(after.dispatches == before.dispatches,
                        "identity: same content with different IDs performs zero dispatches");
    expectations.expect(after.outputCacheHits > before.outputCacheHits,
                        "identity: the scene metadata is current while pixels are cached");
}

// A cached output whose actual descriptor does not match the scene must never be published: the
// executor drops it and recomputes.
void testOutputCacheHitDescriptorValidation(Expectations& expectations, GpuDevice& device) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "descriptor: cache created");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "descriptor: executor created");
    if (!executor) {
        return;
    }
    const auto plan = basicPlan();
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(prepared.hasValue(), "descriptor: the scene prepares");
    if (!prepared) {
        return;
    }
    const auto first = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(first.ready, "descriptor: the first scene completes");
    if (!first.ready) {
        return;
    }
    const auto outputIndex = prepared.scene->outputCommand();
    const std::string outputKey = keyOf(prepared.scene->commands()[outputIndex]);
    static_cast<void>(cache.cache->erase(outputKey));

    auto solid = GpuSolid::create(device);
    expectations.expect(solid.hasValue(), "descriptor: a fixture solid is created");
    if (!solid) {
        return;
    }
    const auto window = ImageWindow::create(0, 0, 4, 4);
    expectations.expect(static_cast<bool>(window), "descriptor: the fixture window builds");
    if (!window) {
        return;
    }
    const GpuSolidParameters parameters{Rgba32f::transparent(), *window.value(), *window.value(),
                                        PixelAspectRatio::square()};
    const auto begun = solid.solid->begin(parameters, 1ULL << 20U);
    expectations.expect(begun.code == bloom::render::GpuSolidDiagnosticCode::None,
                        "descriptor: the fixture solid submits");
    if (begun.code != bloom::render::GpuSolidDiagnosticCode::None) {
        return;
    }
    for (std::uint64_t iteration = 0; iteration < kMaxPollIterations; ++iteration) {
        const auto result = solid.solid->poll();
        if (result != GpuSolidPollResult::Pending) {
            break;
        }
        std::this_thread::yield();
    }
    auto wrongImage = std::make_shared<const GpuImage>(solid.solid->takeImage());
    expectations.expect(wrongImage->isValid(), "descriptor: the wrong-descriptor fixture is valid");
    const auto inserted = cache.cache->insert(outputKey, wrongImage);
    expectations.expect(inserted == bloom::runtime::GpuSceneCacheInsertResult::Inserted,
                        "descriptor: the wrong-descriptor entry inserts");

    const auto before = executor.executor->counters();
    const auto second = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(second.ready, "descriptor: the executor recovers and completes");
    expectations.expect(descriptorMatchesScene(*second.image, *prepared.scene),
                        "descriptor: the recomputed output has the exact scene descriptor");
    expectations.expect(executor.executor->counters().outputCacheMisses > before.outputCacheMisses,
                        "descriptor: a mismatched output cache hit is recomputed, not published");
}
