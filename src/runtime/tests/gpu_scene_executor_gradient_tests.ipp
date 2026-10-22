// Both gradient authoring nodes run through the production plan builder and native executor.
// The CPU evaluator remains the numeric oracle; the only readback is the terminal test probe.
[[nodiscard]] PlanPtr gradientPlan(const bool radial, const bool degenerate = false) {
    const auto base = twoLayerPlan(format(9, 6, pixelAspect(4, 3)),
                                   LayerValues{.position = {4.5, 3.0}},
                                   LayerValues{.position = {7.5, 5.0}, .opacity = 0.2},
                                   4.0, 3.0, radial ? 181000 : 180000);
    auto definition = base->copyDefinition();
    auto& source = std::get<bloom::runtime::CompiledSolid>(definition.operations[0]);
    const std::uint64_t id = radial ? 181100 : 180100;
    source.color.source = Color4d{-0.25, 2.0, 0.5, 0.75};
    source.gradient = bloom::runtime::CompiledGradient{
        radial,
        {bloom::document::ParameterId::fromRaw(id + 1), Color4d{1.0, -0.5, 0.25, 0.25}},
        {bloom::document::ParameterId::fromRaw(id + 2),
         bloom::document::Vec2d{radial ? 2.0 : 0.0, radial ? 1.5 : 0.0}},
        {bloom::document::ParameterId::fromRaw(id + 3),
         bloom::document::Vec2d{degenerate ? 2.0 : 4.0, radial ? 1.5 : 0.0}},
        bloom::document::ParameterId::fromRaw(id + 4), radial ? 2 : 1};
    return publish(std::move(definition));
}

void testGradientProduction(Expectations& expectations, GpuDevice& device,
                            const CpuCompositionEvaluator& oracle) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "gradient: native cache creates");
    if (!cache) return;
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "gradient: production executor creates");
    if (!executor) return;

    for (const bool radial : {false, true}) {
        const auto plan = gradientPlan(radial);
        auto request = requestFor(*plan);
        expectParity(expectations, *executor.executor, oracle, plan, request,
                     radial ? "radial gradient scene-linear/PAR/mirror" :
                              "linear gradient scene-linear/repeat");
        const auto proxyExtent = bloom::render::ImageExtent::create(5, 4);
        expectations.expect(static_cast<bool>(proxyExtent), "gradient: proxy extent creates");
        if (proxyExtent) {
            request.resolution = ProxyResolution{*proxyExtent.value()};
            expectParity(expectations, *executor.executor, oracle, plan, request,
                         radial ? "radial gradient proxy" : "linear gradient proxy");
        }
    }
    const auto degenerate = gradientPlan(true, true);
    expectParity(expectations, *executor.executor, oracle, degenerate, requestFor(*degenerate),
                 "radial gradient zero-length geometry");

    // A later checker uses the standalone solid pipeline. Its presence must not
    // discard the earlier gradient's required native prefix on a cold request.
    {
        auto mixedDefinition = gradientPlan(false)->copyDefinition();
        const auto old = std::get<bloom::runtime::CompiledSolid>(mixedDefinition.operations[2]);
        mixedDefinition.operations[2] = bloom::runtime::CompiledChecker{
            old.sourceNodeId, old.color,
            {bloom::document::ParameterId::fromRaw(180999), Color4d{0.8, 0.1, 0.2, 0.6}},
            2, 1, 4, 3};
        const auto mixed = publish(std::move(mixedDefinition));
        cache.cache->clear();
        expectParity(expectations, *executor.executor, oracle, mixed, requestFor(*mixed),
                     "gradient followed by checker retains its native prefix");
    }

    // A completed executor may remain alive while another executor uses the same device.
    // Keeping the first facade must not retain exclusive native batch ownership.
    {
        auto other = GpuSceneExecutor::create(device, *cache.cache);
        expectations.expect(other.hasValue(), "gradient: second live executor creates");
        if (other) {
            cache.cache->clear();
            const auto plan = gradientPlan(false);
            expectParity(expectations, *other.executor, oracle, plan, requestFor(*plan),
                         "gradient after another live executor completed native work");
        }
    }
    // Disabling multi-operation batching still permits the required singleton source kernel.
    {
        GpuSceneExecutorBudgets serialBudgets;
        serialBudgets.enableNativeBatches = false;
        auto serial = GpuSceneExecutor::create(device, *cache.cache, serialBudgets);
        expectations.expect(serial.hasValue(), "gradient: serial native executor creates");
        if (serial) {
            cache.cache->clear();
            const auto plan = gradientPlan(false);
            expectParity(expectations, *serial.executor, oracle, plan, requestFor(*plan),
                         "gradient singleton with multi-operation batching disabled");
        }
    }

    // The authoring endpoints need not individually fit the Float32 process format.
    // A one-pixel source samples their midpoint, which is exactly finite black here.
    {
        auto hdrDefinition = gradientPlan(false)->copyDefinition();
        auto& hdr = std::get<bloom::runtime::CompiledSolid>(hdrDefinition.operations[0]);
        hdr.color.source = Color4d{1e39, -1e39, 0, 1};
        hdr.gradient->endColor.source = Color4d{-1e39, 1e39, 0, 1};
        hdr.width.source = 1.0;
        hdr.height.source = 1.0;
        hdr.gradient->start.source = bloom::document::Vec2d{0, 0};
        hdr.gradient->end.source = bloom::document::Vec2d{1, 0};
        hdr.gradient->repeat = 0;
        const auto hdrPlan = publish(std::move(hdrDefinition));
        const auto hdrFrame = oracle.evaluate(hdrPlan, requestFor(*hdrPlan), {});
        expectations.expect(hdrFrame.frame() != nullptr,
                            "gradient: finite sampled HDR output accepts Float64 endpoints");
    }

    for (const bool radial : {false, true}) {
        for (const std::int64_t repeat : {std::int64_t{1}, std::int64_t{2}}) {
            auto distantDefinition = gradientPlan(radial)->copyDefinition();
            auto& distant = std::get<bloom::runtime::CompiledSolid>(distantDefinition.operations[0]);
            distant.color.source = Color4d{0, 0, 0, 1};
            distant.gradient->endColor.source = Color4d{1, 1, 1, 1};
            distant.gradient->start.source = bloom::document::Vec2d{100000000, 0};
            distant.gradient->end.source = bloom::document::Vec2d{100000008, 0};
            distant.gradient->repeat = repeat;
            const auto distantPlan = publish(std::move(distantDefinition));
            const std::string label = std::string(radial ? "radial" : "linear") +
                " gradient distant " + (repeat == 1 ? "repeat" : "mirror") +
                " preserves fractional phase";
            expectParity(expectations, *executor.executor, oracle, distantPlan,
                         requestFor(*distantPlan), label);
        }
    }

    auto definition = gradientPlan(false)->copyDefinition();
    auto& animated = std::get<bloom::runtime::CompiledSolid>(definition.operations[0]);
    animated.gradient->end.source = bloom::runtime::Vec2CurveIndex::fromRaw(0);
    bloom::runtime::CompiledVec2Curve curve;
    curve.id = bloom::document::AnimationCurveId::fromRaw(180200);
    curve.components[0] = {
        {bloom::document::KeyframeId::fromRaw(180201), RationalTime::fromInteger(0), 4.0,
         bloom::runtime::CompiledKeyframeInterpolation::Linear},
        {bloom::document::KeyframeId::fromRaw(180202), RationalTime::fromInteger(1), 2.0,
         bloom::runtime::CompiledKeyframeInterpolation::Linear}};
    curve.components[1] = {
        {bloom::document::KeyframeId::fromRaw(180203), RationalTime::fromInteger(0), 0.0,
         bloom::runtime::CompiledKeyframeInterpolation::Linear},
        {bloom::document::KeyframeId::fromRaw(180204), RationalTime::fromInteger(1), 2.0,
         bloom::runtime::CompiledKeyframeInterpolation::Linear}};
    definition.vec2Curves.push_back(std::move(curve));
    const auto animatedPlan = publish(std::move(definition));
    const auto firstRequest = requestFor(*animatedPlan);
    const auto nextRequest = requestFor(*animatedPlan, RationalTime::fromInteger(1));
    const auto first = CpuGpuSceneBuilder{}.build(animatedPlan, firstRequest);
    const auto next = CpuGpuSceneBuilder{}.build(animatedPlan, nextRequest);
    expectations.expect(first.hasValue() && next.hasValue(),
                        "gradient: animated endpoints prepare at both times");
    if (!first || !next) return;
    const auto* firstSource =
        std::get_if<GpuSceneSolidCommand>(&first.scene->commands().front());
    const auto* nextSource =
        std::get_if<GpuSceneSolidCommand>(&next.scene->commands().front());
    expectations.expect(firstSource != nullptr && nextSource != nullptr &&
                            firstSource->gradient.has_value() && nextSource->gradient.has_value(),
                        "gradient: both prepared scenes contain native source commands");
    if (firstSource == nullptr || nextSource == nullptr) return;
    const auto firstKey = keyOf(first.scene->commands().front());
    const auto nextKey = keyOf(next.scene->commands().front());
    expectations.expect(firstKey != nextKey,
                        "gradient: endpoint animation invalidates source semantic key");
    cache.cache->clear();
    const auto cold = runScene(*executor.executor, first.scene, kSceneBudget);
    const auto warm = runScene(*executor.executor, first.scene, kSceneBudget);
    const auto changed = runScene(*executor.executor, next.scene, kSceneBudget);
    expectations.expect(cold.ready && warm.ready && changed.ready,
                        "gradient: cold, warm, and changed native scenes finish");
    if (cold.ready && warm.ready && changed.ready) {
        expectations.expect(warm.countersAtReady.dispatches == cold.countersAtReady.dispatches &&
                            changed.countersAtReady.dispatches > warm.countersAtReady.dispatches,
                            "gradient: warm cache skips dispatch and changed endpoint redispatches");
    }
    expectParity(expectations, *executor.executor, oracle, animatedPlan, nextRequest,
                 "animated linear gradient endpoint");
}
