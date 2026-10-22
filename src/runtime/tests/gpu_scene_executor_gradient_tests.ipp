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
