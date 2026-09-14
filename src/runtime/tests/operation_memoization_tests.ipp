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
        expectations.expect(serial.frame() && first.frame()->identity() == serial.frame()->identity(),
                            "memoization does not change semantic identity");
        if (serial.frame()) expectations.expect(
            std::memcmp(serial.frame()->processImage().pixels().data(),
                        first.frame()->processImage().pixels().data(),
                        first.frame()->processImage().pixels().size_bytes()) == 0,
            "serial and cached text pixels are bit identical");
    }
    request.bypassOperationCache = false;
    request.pixelStorageByteLimit = 1;
    expectations.expect(evaluator.evaluate(plan, request, {}).status() == runtime::EvaluationStatus::Failed,
                        "a warm cache cannot bypass request storage preflight");
    evaluator.operationCache()->setByteBudget(0);
    expectations.expect(evaluator.operationCache()->retainedBytes() == 0, "zero budget evicts all entries");
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan> memoizationFixture() {
    auto definition = twoSolidPlan()->copyDefinition();
    definition.format = format(64, 48);
    definition.operations[2] = runtime::CompiledText{
        kTextNode, kTextContent, "Bloom", {kTextSize, 12.0},
        {kTextColor, core::Color4d{0.5, 0.25, 0.75, 1.0}}};
    std::get<runtime::CompiledLayerOutput>(definition.operations[1]).position.source = document::Vec2d{32, 24};
    std::get<runtime::CompiledLayerOutput>(definition.operations[3]).position.source = document::Vec2d{32, 24};
    return publishPlan(std::move(definition));
}

void testOperationTimeInvariance(Expectations& expectations) {
    runtime::CpuCompositionEvaluator evaluator;
    const auto plan = memoizationFixture();
    std::size_t misses = 0, hits = 0;
    for (std::int64_t frame = 0; frame < 24; ++frame) {
        auto request = requestFor(*plan);
        const auto time = core::RationalTime::create(frame, 24);
        if (!time) throw std::runtime_error("invalid frame time");
        request.time = *time;
        runtime::OperationCacheStatistics statistics;
        const auto result = evaluator.evaluate(plan, request, {}, {}, nullptr, &statistics);
        misses += statistics.misses; hits += statistics.hits;
        request.bypassOperationCache = true;
        const auto serial = evaluator.evaluate(plan, request, {});
        expectations.expect(result.frame() && serial.frame(), "static range evaluates");
        if (result.frame() && serial.frame()) expectations.expect(
            std::memcmp(result.frame()->processImage().pixels().data(), serial.frame()->processImage().pixels().data(),
                        result.frame()->processImage().pixels().size_bytes()) == 0,
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
        {document::NodeId::fromRaw(80), runtime::ValueOutputIndex::fromRaw(0), 2, runtime::CompiledValueTime{}},
        {document::NodeId::fromRaw(81), runtime::ValueOutputIndex::fromRaw(2), 1,
         runtime::CompiledValuePassthrough{{{}, runtime::ValueOutputIndex::fromRaw(0)}}},
        {document::NodeId::fromRaw(82), runtime::ValueOutputIndex::fromRaw(3), 1,
         runtime::CompiledValuePassthrough{{{}, runtime::CompiledValue{0.5}}}}};
    drivenDefinition.valueOutputCount = 4;
    std::get<runtime::CompiledLayerOutput>(drivenDefinition.operations[1]).opacity.source = runtime::ValueOutputIndex::fromRaw(2);
    const auto driven = publishPlan(std::move(drivenDefinition));
    const auto dependence = driven->valueTimeDependence();
    expectations.expect(dependence.size() == 3 && dependence[0] == 1 && dependence[1] == 1 && dependence[2] == 0 &&
                        driven->operationTimeDependent(driven->output()),
                        "Time dependence reaches drivers while a constant sibling stays invariant");
    runtime::CpuCompositionEvaluator drivenEvaluator;
    auto request = requestFor(*driven);
    runtime::OperationCacheStatistics statistics;
    const auto first = drivenEvaluator.evaluate(driven, request, {});
    request.time = core::RationalTime::fromInteger(1);
    const auto next = drivenEvaluator.evaluate(driven, request, {}, {}, nullptr, &statistics);
    expectations.expect(first.frame() && next.frame() && statistics.hits == 2 && statistics.misses == 5,
                        "Time and its driver rerun; constant value and solid hit");
}
