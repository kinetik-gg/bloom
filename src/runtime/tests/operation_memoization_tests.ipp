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
