void testOperationMemoization(Expectations& expectations) {
    runtime::CpuCompositionEvaluator evaluator;
    const auto plan = oneTextPlan();
    auto request = requestFor(*plan);
    runtime::OperationCacheStatistics cold, warm;
    const auto first = evaluator.evaluate(plan, request, {}, {}, nullptr, &cold);
    const auto second = evaluator.evaluate(plan, request, {}, {}, nullptr, &warm);
    expectations.expect(first.frame() && second.frame(), "memoized text evaluates");
    expectations.expect(cold.misses == 4 && warm.hits == 4 && warm.misses == 0,
                        "each image operation is memoized, including output");
    if (first.frame() && second.frame()) {
        expectations.expect(first.frame()->processImage().pixels().data() ==
                                second.frame()->processImage().pixels().data(),
                            "cache hit retains immutable image storage without copying");
        request.bypassOperationCache = true;
        const auto serial = evaluator.evaluate(plan, request, {});
        expectations.expect(serial.frame() &&
                                first.frame()->identity() == serial.frame()->identity(),
                            "memoization does not change semantic identity");
        if (serial.frame())
            expectations.expect(std::memcmp(serial.frame()->processImage().pixels().data(),
                                            first.frame()->processImage().pixels().data(),
                                            first.frame()->processImage().pixels().size_bytes()) ==
                                    0,
                                "serial and cached text pixels are bit identical");
    }
    request.bypassOperationCache = false;
    request.pixelStorageByteLimit = 1;
    expectations.expect(evaluator.evaluate(plan, request, {}).status() ==
                            runtime::EvaluationStatus::Failed,
                        "a warm cache cannot bypass request storage preflight");
    evaluator.operationCache()->setByteBudget(0);
    expectations.expect(evaluator.operationCache()->retainedBytes() == 0,
                        "zero budget evicts all entries");
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan> memoizationFixture() {
    auto definition = twoSolidPlan()->copyDefinition();
    definition.format = format(64, 48);
    definition.operations[2] =
        runtime::CompiledText{kTextNode,
                              kTextContent,
                              "Bloom",
                              {kTextSize, 12.0},
                              {kTextColor, core::Color4d{0.5, 0.25, 0.75, 1.0}},
                              {{document::ParameterId::fromRaw(901)},
                               0,
                               {document::ParameterId::fromRaw(902), 1.0},
                               {document::ParameterId::fromRaw(903), 0.0}}};
    std::get<runtime::CompiledLayerOutput>(definition.operations[1]).position.source =
        document::Vec2d{32, 24};
    std::get<runtime::CompiledLayerOutput>(definition.operations[3]).position.source =
        document::Vec2d{32, 24};
    return publishPlan(std::move(definition));
}

// GRAPH-1, D1: a key's ease handle is part of what the curve means, so editing one has to reach the
// pixels of an already-memoized frame. The two plans differ in nothing but the left key's outgoing
// handle offset; sharing one evaluator makes the cache the thing under test rather than the
// arithmetic.
[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
easedOpacityPlan(const double outgoingHandleValue) {
    auto definition = oneSolidPlan()->copyDefinition();
    std::get<runtime::CompiledLayerOutput>(definition.operations[1]).opacity.source =
        runtime::ScalarCurveIndex::fromRaw(0);
    runtime::CompiledScalarKeyframe start{document::KeyframeId::fromRaw(70),
                                          core::RationalTime::fromInteger(0), 0.0,
                                          runtime::CompiledKeyframeInterpolation::EaseInOut};
    start.outgoingHandle.value = outgoingHandleValue;
    const runtime::CompiledScalarKeyframe end{document::KeyframeId::fromRaw(71),
                                              core::RationalTime::fromInteger(1), 1.0,
                                              runtime::CompiledKeyframeInterpolation::Linear};
    definition.scalarCurves.push_back({kOpacityCurve, {start, end}});
    return std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
}

void testEaseHandleChangeReachesAMemoizedFrame(Expectations& expectations) {
    const auto halfway = core::RationalTime::create(1, 2);
    if (!halfway)
        throw std::runtime_error("invalid halfway time");
    runtime::CpuCompositionEvaluator evaluator;
    const auto plain = easedOpacityPlan(bloom::document::kDefaultKeyframeHandleValue);
    auto request = requestFor(*plain);
    request.time = *halfway;
    runtime::OperationCacheStatistics first;
    const auto before = evaluator.evaluate(plain, request, {}, {}, nullptr, &first);
    render::Rgba32f beforeSample = render::Rgba32f::transparent();
    const auto* beforePixel = pixel(before, 0, 0, beforeSample);

    const auto lifted = easedOpacityPlan(0.5);
    auto liftedRequest = requestFor(*lifted);
    liftedRequest.time = *halfway;
    runtime::OperationCacheStatistics second;
    const auto after = evaluator.evaluate(lifted, liftedRequest, {}, {}, nullptr, &second);
    render::Rgba32f afterSample = render::Rgba32f::transparent();
    const auto* afterPixel = pixel(after, 0, 0, afterSample);

    expectations.expect(beforePixel != nullptr && afterPixel != nullptr && second.misses > 0 &&
                            beforePixel->alpha() != afterPixel->alpha(),
                        "editing an ease handle re-evaluates the memoized frame it changes");
}

void testOperationTimeInvariance(Expectations& expectations) {
    runtime::CpuCompositionEvaluator evaluator;
    const auto plan = memoizationFixture();
    std::size_t misses = 0, hits = 0;
    for (std::int64_t frame = 0; frame < 24; ++frame) {
        auto request = requestFor(*plan);
        const auto time = core::RationalTime::create(frame, 24);
        if (!time)
            throw std::runtime_error("invalid frame time");
        request.time = *time;
        runtime::OperationCacheStatistics statistics;
        const auto result = evaluator.evaluate(plan, request, {}, {}, nullptr, &statistics);
        misses += statistics.misses;
        hits += statistics.hits;
        request.bypassOperationCache = true;
        const auto serial = evaluator.evaluate(plan, request, {});
        expectations.expect(result.frame() && serial.frame(), "static range evaluates");
        if (result.frame() && serial.frame())
            expectations.expect(std::memcmp(result.frame()->processImage().pixels().data(),
                                            serial.frame()->processImage().pixels().data(),
                                            result.frame()->processImage().pixels().size_bytes()) ==
                                    0,
                                "every cached static frame equals its serial reference to the bit");
    }
    expectations.expect(misses == 6 && hits == 138,
                        "static solid plus text evaluates every operation once across 24 frames");
    for (std::size_t i = 0; i < plan->operations().size(); ++i)
        expectations.expect(!plan->operationTimeDependent(runtime::OperationIndex::fromRaw(i)),
                            "static source, transform, merge and output are invariant");
    const auto animated = animatedLayerPlan();
    expectations.expect(!animated->operationTimeDependent(runtime::OperationIndex::fromRaw(0)) &&
                            animated->operationTimeDependent(runtime::OperationIndex::fromRaw(1)) &&
                            animated->operationTimeDependent(animated->output()),
                        "a curve marks only its owner and downstream time dependent");
    auto drivenDefinition = oneSolidPlan()->copyDefinition();
    drivenDefinition.valueOperations = {
        {document::NodeId::fromRaw(80), runtime::ValueOutputIndex::fromRaw(0), 2,
         runtime::CompiledValueTime{}},
        {document::NodeId::fromRaw(81), runtime::ValueOutputIndex::fromRaw(2), 1,
         runtime::CompiledValuePassthrough{{{}, runtime::ValueOutputIndex::fromRaw(0)}}},
        {document::NodeId::fromRaw(82), runtime::ValueOutputIndex::fromRaw(3), 1,
         runtime::CompiledValuePassthrough{{{}, runtime::CompiledValue{0.5}}}}};
    drivenDefinition.valueOutputCount = 4;
    std::get<runtime::CompiledLayerOutput>(drivenDefinition.operations[1]).opacity.source =
        runtime::ValueOutputIndex::fromRaw(2);
    const auto driven = publishPlan(std::move(drivenDefinition));
    const auto dependence = driven->valueTimeDependence();
    expectations.expect(dependence.size() == 3 && dependence[0] == 1 && dependence[1] == 1 &&
                            dependence[2] == 0 && driven->operationTimeDependent(driven->output()),
                        "Time dependence reaches drivers while a constant sibling stays invariant");
    runtime::CpuCompositionEvaluator drivenEvaluator;
    auto request = requestFor(*driven);
    runtime::OperationCacheStatistics statistics;
    const auto first = drivenEvaluator.evaluate(driven, request, {});
    request.time = core::RationalTime::fromInteger(1);
    const auto next = drivenEvaluator.evaluate(driven, request, {}, {}, nullptr, &statistics);
    expectations.expect(first.frame() && next.frame() && statistics.hits == 2 &&
                            statistics.misses == 5,
                        "Time and its driver rerun; constant value and solid hit");
}

void testOperationDirtyPropagation(Expectations& expectations) {
    runtime::CpuCompositionEvaluator evaluator;
    const auto plan = memoizationFixture();
    const auto first = evaluator.evaluate(plan, requestFor(*plan), {});
    auto definition = plan->copyDefinition();
    definition.sourceRevision = document::Revision::fromRaw(8);
    std::get<runtime::CompiledSolid>(definition.operations[0]).color.source =
        core::Color4d{0, 1, 0, 1};
    const auto edited = publishPlan(std::move(definition));
    runtime::OperationCacheStatistics statistics;
    auto request = requestFor(*edited);
    const auto result = evaluator.evaluate(edited, request, {}, {}, nullptr, &statistics);
    expectations.expect(
        first.frame() && result.frame() && statistics.hits == 2 && statistics.misses == 4,
        "color edit reruns solid, its transform, merge and output; text and its transform hit");
    expectations.expect(
        statistics.evaluatedNodes == std::vector{kSolidNodeA, kLayerNodeA, kStackNode, kOutputNode},
        "dirty propagation identifies exactly the changed branch and its downstream");
    request.bypassOperationCache = true;
    const auto serial = evaluator.evaluate(edited, request, {});
    if (result.frame() && serial.frame())
        expectations.expect(std::memcmp(result.frame()->processImage().pixels().data(),
                                        serial.frame()->processImage().pixels().data(),
                                        result.frame()->processImage().pixels().size_bytes()) == 0,
                            "edited cached image equals uncached serial pixels");
    auto unrelatedDefinition = edited->copyDefinition();
    unrelatedDefinition.sourceRevision = document::Revision::fromRaw(9);
    const auto unrelated = publishPlan(std::move(unrelatedDefinition));
    const auto unchanged =
        evaluator.evaluate(unrelated, requestFor(*unrelated), {}, {}, nullptr, &statistics);
    expectations.expect(unchanged.frame() && statistics.hits == 6 && statistics.misses == 0,
                        "unrelated revision edit reuses all operation content");
    auto trimDefinition = unrelated->copyDefinition();
    auto& layer = std::get<runtime::CompiledLayerOutput>(trimDefinition.operations[3]);
    layer.inPoint = core::RationalTime::fromInteger(1);
    layer.outPoint = core::RationalTime::fromInteger(2);
    const auto trimmed = publishPlan(std::move(trimDefinition));
    for (const auto frame : {0, 1, 2, 1}) {
        auto trimRequest = requestFor(*trimmed);
        trimRequest.time = core::RationalTime::fromInteger(frame);
        const auto cached = evaluator.evaluate(trimmed, trimRequest, {});
        trimRequest.bypassOperationCache = true;
        const auto reference = evaluator.evaluate(trimmed, trimRequest, {});
        expectations.expect(cached.frame() && reference.frame(), "trim boundary evaluates");
        if (cached.frame() && reference.frame())
            expectations.expect(std::memcmp(cached.frame()->processImage().pixels().data(),
                                            reference.frame()->processImage().pixels().data(),
                                            cached.frame()->processImage().pixels().size_bytes()) ==
                                    0,
                                "cached absence and presence respect both trim boundaries");
    }
}

void testMemoryBudgetLedger(Expectations& expectations) {
    constexpr auto gibibyte = std::size_t{1024} * 1024U * 1024U;
    constexpr std::size_t plentiful = std::size_t{1024} * gibibyte;

    const runtime::MemoryBudgetLedger sixteen(std::size_t{16} * gibibyte, plentiful);
    const auto defaults = sixteen.allocate();
    const auto expectedOperation = std::size_t{8} * gibibyte * 3 / 5;
    expectations.expect(sixteen.reserveByteBudget() == 0 &&
                            defaults.usableByteBudget == std::size_t{16} * gibibyte &&
                            sixteen.defaultTotalByteBudget() == std::size_t{8} * gibibyte,
                        "hardware capacity and the automatic total have no host reserve");
    expectations.expect(defaults.operationCacheByteBudget == expectedOperation &&
                            defaults.previewFrameCacheByteBudget ==
                                sixteen.defaultTotalByteBudget() - expectedOperation,
                        "the automatic total splits 60/40 operation/preview");

    const runtime::MemoryBudgetLedger sixty(std::size_t{60} * gibibyte, plentiful);
    const auto sixtyDefaults = sixty.allocate();
    expectations.expect(
        sixtyDefaults.operationCacheByteBudget == std::size_t{18} * gibibyte &&
            sixtyDefaults.previewFrameCacheByteBudget == std::size_t{12} * gibibyte,
        "a 60 GiB machine automatically selects 18 GiB operation and 12 GiB preview");
    const runtime::MemoryBudgetLedger busy(std::size_t{60} * gibibyte, std::size_t{12} * gibibyte);
    expectations.expect(busy.allocate() == sixtyDefaults,
                        "launch-time free memory never rewrites automatic cache selections");

    const runtime::MemoryBudgetLedger small(std::size_t{4} * gibibyte, plentiful);
    const auto smallDefaults = small.allocate();
    expectations.expect(small.defaultTotalByteBudget() == runtime::kFallbackUsableMemoryBudget &&
                            smallDefaults.operationCacheByteBudget == gibibyte &&
                            smallDefaults.previewFrameCacheByteBudget ==
                                runtime::kMinimumPreviewFrameCacheByteBudget,
                        "small hardware uses the bounded automatic fallback and preview minimum");
    const runtime::MemoryBudgetLedger unknown(0, 0);
    expectations.expect(unknown.usableByteBudget() == runtime::kFallbackUsableMemoryBudget &&
                            unknown.reserveByteBudget() == 0,
                        "unknown hardware has a visible automatic fallback without a reserve");

    const auto operationOverride = sixty.allocate(std::size_t{34} * gibibyte);
    expectations.expect(operationOverride.operationCacheByteBudget == std::size_t{34} * gibibyte &&
                            operationOverride.previewFrameCacheByteBudget ==
                                sixtyDefaults.previewFrameCacheByteBudget,
                        "an operation preference preserves the independent automatic preview");
    const auto abovePhysical = sixty.allocate(std::size_t{80} * gibibyte);
    expectations.expect(abovePhysical.operationCacheByteBudget == std::size_t{80} * gibibyte,
                        "an explicit preference above physical capacity remains exact");
    const auto previewOverride = sixty.allocate({}, std::size_t{30} * gibibyte);
    expectations.expect(previewOverride.previewFrameCacheByteBudget == std::size_t{30} * gibibyte &&
                            previewOverride.operationCacheByteBudget ==
                                sixtyDefaults.operationCacheByteBudget,
                        "a preview preference preserves the independent automatic operation cache");
    const auto overcommitted =
        sixty.allocate(std::size_t{80} * gibibyte, std::size_t{90} * gibibyte);
    expectations.expect(overcommitted.operationCacheByteBudget == std::size_t{80} * gibibyte &&
                            overcommitted.previewFrameCacheByteBudget == std::size_t{90} * gibibyte,
                        "two explicit preferences never undergo proportional rescaling");
    expectations.expect(sixty.allocate(0, 0) == sixtyDefaults &&
                            sixty.allocate(0, std::size_t{6} * gibibyte).operationCacheByteBudget ==
                                sixtyDefaults.operationCacheByteBudget,
                        "zero selects each pool's automatic default independently");
}

void testOperationCacheByteAccounting(Expectations& expectations) {
    // CACHEFIX-1 accounting: the audit has to be able to say WHICH half of the operation cache is
    // holding the bytes, because decoded media is the expensive half and the protected one.
    runtime::OperationCache cache;
    const auto revision = document::Revision::fromRaw(1);
    const runtime::OperationCacheValue value{.image = {}, .values = {runtime::CompiledValue{0.5}}};
    cache.store("operation-a", revision, value);
    const auto operationBytes = cache.retainedBytes();
    cache.store("decoded-a", document::Revision{}, value,
                runtime::OperationCacheEntryKind::DecodedMedia);
    expectations.expect(cache.retainedBytes(runtime::OperationCacheEntryKind::Operation) ==
                                operationBytes &&
                            cache.retainedBytes(runtime::OperationCacheEntryKind::DecodedMedia) ==
                                cache.retainedBytes() - operationBytes,
                        "the operation cache separates operation bytes from decoded-media bytes");
    expectations.expect(
        cache.retainedBytes(runtime::OperationCacheEntryKind::Operation) +
                cache.retainedBytes(runtime::OperationCacheEntryKind::DecodedMedia) ==
            cache.retainedBytes(),
        "the two kind accounts add up to the total");

    // A pressure trim evicts down to a limit WITHOUT surrendering the budget, so the cache can
    // fill back up once the machine recovers.
    const auto budget = cache.byteBudget();
    const auto before = cache.retainedBytes();
    cache.trimToBytes(0);
    expectations.expect(cache.retainedBytes() == 0 && cache.byteBudget() == budget,
                        "a trim to zero empties the cache and leaves the budget alone");
    expectations.expect(cache.statistics().pressureDrops == 2,
                        "a trim counts its drops apart from ordinary budget eviction");
    expectations.expect(cache.retainedBytes(runtime::OperationCacheEntryKind::DecodedMedia) == 0,
                        "the decoded-media account falls with the entries it was counting");
    expectations.expect(before > 0, "the cache held something before the trim");
    cache.store("operation-b", revision, value);
    expectations.expect(
        cache.retainedBytes() > 0,
        "the cache accepts entries again after a trim, because the budget survived");
    cache.trimToBytes(std::numeric_limits<std::size_t>::max());
    expectations.expect(cache.retainedBytes() > 0, "a trim above the budget evicts nothing");
}

void testOperationCacheLifecycle(Expectations& expectations) {
    runtime::OperationCache cache;
    const auto revision = document::Revision::fromRaw(1);
    const runtime::OperationCacheValue value{.image = {}, .values = {runtime::CompiledValue{0.5}}};
    cache.store("a", revision, value);
    const auto cost = cache.retainedBytes();
    cache.setByteBudget(cost * 2);
    cache.store("b", revision, value);
    expectations.expect(cache.find("a", revision).has_value(), "touch refreshes LRU recency");
    cache.store("c", revision, value);
    expectations.expect(!cache.find("b", revision) && cache.find("a", revision) &&
                            cache.find("c", revision),
                        "LRU evicts the least recently used operation");
    expectations.expect(cache.retainedBytes() <= cost * 2, "cache retention respects byte budget");
    cache.store(std::string(cost * 3, 'x'), revision, value);
    expectations.expect(cache.find("a", revision) && cache.find("c", revision),
                        "oversized entry does not evict useful operations");
    std::vector<std::thread> workers;
    workers.reserve(3);
    for (std::size_t i = 0; i < 3; ++i)
        workers.emplace_back([&cache, revision, value, i] {
            for (std::size_t n = 0; n < 30; ++n) {
                const auto key = std::to_string(i * 30 + n);
                cache.store(key, revision, value);
                (void)cache.find(key, document::Revision::fromRaw(2));
            }
        });
    for (auto& worker : workers)
        worker.join();
    expectations.expect(cache.retainedBytes() <= cost * 2,
                        "concurrent cache adoption and eviction stay bounded");

    runtime::OperationCache mediaCache(cost * 12);
    mediaCache.store("decoded-media", document::Revision{}, value,
                     runtime::OperationCacheEntryKind::DecodedMedia);
    const auto mediaCost = mediaCache.retainedBytes();
    mediaCache.store("revision-operation-0", revision, value);
    const auto operationCost = mediaCache.retainedBytes() - mediaCost;
    mediaCache.setByteBudget(mediaCost + operationCost * 2);
    for (std::size_t index = 1; index <= 6; ++index)
        mediaCache.store("revision-operation-" + std::to_string(index),
                         document::Revision::fromRaw(index + 1), value);
    expectations.expect(mediaCache.find("decoded-media", document::Revision{}).has_value(),
                        "a burst of revision entries does not evict recently decoded media");

    runtime::CpuCompositionEvaluator evaluator;
    auto definition = memoizationFixture()->copyDefinition();
    definition.bypassOperationCache = true;
    const auto overridePlan = publishPlan(std::move(definition));
    const auto result = evaluator.evaluate(overridePlan, requestFor(*overridePlan), {});
    expectations.expect(result.frame() && evaluator.operationCache()->retainedBytes() == 0,
                        "override plans bypass cache even without a request flag");
    auto normalDefinition = overridePlan->copyDefinition();
    normalDefinition.bypassOperationCache = false;
    const auto normalPlan = publishPlan(std::move(normalDefinition));
    const auto normal = evaluator.evaluate(normalPlan, requestFor(*normalPlan), {});
    expectations.expect(result.frame() && normal.frame() &&
                            result.frame()->identity() == normal.frame()->identity(),
                        "memoization metadata cannot alter process frame identity");
    if (result.frame())
        expectations.expect(result.frame()->operationCacheStatistics().hits == 0 &&
                                result.frame()->operationCacheStatistics().misses == 6,
                            "per-frame statistics are exposed without UI state");
}

void benchmarkOperationMemoization(Expectations& expectations) {
    auto definition = memoizationFixture()->copyDefinition();
    definition.format = format(640, 360);
    std::get<runtime::CompiledLayerOutput>(definition.operations[1]).position.source =
        document::Vec2d{320, 180};
    std::get<runtime::CompiledLayerOutput>(definition.operations[3]).position.source =
        document::Vec2d{320, 180};
    const auto plan = publishPlan(std::move(definition));
    auto changed = plan->copyDefinition();
    changed.sourceRevision = document::Revision::fromRaw(8);
    std::get<runtime::CompiledSolid>(changed.operations[0]).color.source =
        core::Color4d{0, 1, 0, 1};
    const auto edited = publishPlan(std::move(changed));
    for (const bool bypass : {true, false}) {
        runtime::CpuCompositionEvaluator evaluator;
        const auto start = std::chrono::steady_clock::now();
        std::size_t misses = 0, hits = 0;
        for (std::int64_t frame = 0; frame < 24; ++frame) {
            auto request = requestFor(*plan, 256U << 20U);
            const auto time = core::RationalTime::create(frame, 24);
            if (!time)
                throw std::runtime_error("invalid benchmark time");
            request.time = *time;
            request.bypassOperationCache = bypass;
            runtime::OperationCacheStatistics statistics;
            const auto result = evaluator.evaluate(plan, request, {}, {}, nullptr, &statistics);
            expectations.expect(result.frame() != nullptr, "benchmark frame evaluates");
            misses += statistics.misses;
            hits += statistics.hits;
        }
        const auto rangeEnd = std::chrono::steady_clock::now();
        auto request = requestFor(*edited, 256U << 20U);
        request.bypassOperationCache = bypass;
        runtime::OperationCacheStatistics statistics;
        const auto result = evaluator.evaluate(edited, request, {}, {}, nullptr, &statistics);
        const auto end = std::chrono::steady_clock::now();
        expectations.expect(result.frame() != nullptr, "benchmark edit evaluates");
        std::cout << "MEMO-1 benchmark 640x360 serial " << (bypass ? "uncached" : "cached")
                  << " range24_ms="
                  << std::chrono::duration<double, std::milli>(rangeEnd - start).count()
                  << " hits=" << hits << " evaluations=" << misses << " edit_ms="
                  << std::chrono::duration<double, std::milli>(end - rangeEnd).count()
                  << " edit_hits=" << statistics.hits << " edit_evaluations=" << statistics.misses
                  << '\n';
    }
}
