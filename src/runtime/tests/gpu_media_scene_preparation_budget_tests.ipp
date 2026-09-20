// Budget/capacity group: the media budget refusal, the large-source-under-default-allowance proof,
// and the capacity-aware producer policy. Included by gpu_media_scene_preparation_tests.cpp inside
// its anonymous namespace.

void testBudgetRefusal(Expectations& expectations, const MediaFixture& fixture) {
    auto context = GpuSceneMediaContext::fromEvaluator(CpuCompositionEvaluator{});
    context.assetBaseDirectory = fixture.directory;
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan =
        mediaPlan(format(256, 256), fixture.asset, LayerValues{.position = {128.0, 128.0}}, 2000);
    auto request = requestFor(*plan);
    request.pixelStorageByteLimit = 1U << 16U;
    const auto prepared = builder.build(plan, request);
    expectations.expect(!prepared.hasValue(), "a media scene under a tiny budget fails closed");
    expectations.expect(
        !prepared.hasValue() &&
            prepared.diagnostic.code ==
                bloom::runtime::PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded,
        "the media budget refusal is diagnosed as a pixel-storage budget");
}

// The reported failure: a large EXR source (4608x3164, ~233 MB as RGBA32F) composited over an FHD
// solid and text is refused by the scene builder under the default 512 MiB preview allowance even
// though the retained host set is a single source upload. The builder must not sum mutually
// exclusive GPU output lifetimes into one artificial per-frame total, and a full-source upload must
// keep its resolution. A follow-up request must still prepare (liveness after pressure).
void testLargeSourceUnderDefaultAllowance(Expectations& expectations) {
    const auto directory = std::filesystem::temp_directory_path() / "bloom_gpu_large_source_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto path = directory / "large_4608x3164.exr";
    writeLargeExrRgba(path, 4608, 3164);
    const auto asset = imageAsset(path, "large_source", 4200);

    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(directory);
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan = largeSourcePlan(format(1920, 1080), asset, 5000);
    auto request = requestFor(*plan);
    request.pixelStorageByteLimit = std::size_t{512} * 1024U * 1024U;

    const auto prepared = builder.build(plan, request);
    if (!prepared) {
        std::cerr << "large-source diagnostic: " << prepared.diagnostic.message << '\n';
    }
    expectations.expect(prepared.hasValue(),
                        "large source + solid/text prepares under the 512 MiB preview allowance");
    if (!prepared) {
        return;
    }
    const auto* upload = firstUpload(*prepared.scene);
    expectations.expect(upload != nullptr &&
                            upload->descriptor.dataWindow().extent().width() == 4608 &&
                            upload->descriptor.dataWindow().extent().height() == 3164,
                        "the converted source keeps its full 4608x3164 resolution");
    expectations.expect(prepared.scene->outputDescriptor().dataWindow().extent().width() == 1920 &&
                            prepared.scene->outputDescriptor().dataWindow().extent().height() ==
                                1080,
                        "the composition output stays FHD");

    // Identity and bounds parity against the genuine CPU frame. Pixel replay of the full 233 MB
    // intermediate set is deliberately not run here; the small-source tests above own bit parity.
    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr,
                        "the CPU oracle evaluates the large-source graph");
    if (frame.frame() != nullptr) {
        expectations.expect(prepared.scene->processIdentity() == frame.frame()->identity(),
                            "large source: process identity matches the CPU frame");
        expectations.expect(
            std::ranges::equal(prepared.scene->bounds(), frame.frame()->evaluatedBounds()),
            "large source: evaluated bounds match the CPU frame");
    }

    // Liveness: a later, ordinary request must still prepare after the pressure.
    const auto small =
        mediaPlan(format(64, 48), asset, LayerValues{.position = {32.0, 24.0}}, 5200);
    const auto recovered = builder.build(small, requestFor(*small));
    expectations.expect(recovered.hasValue(),
                        "a later request still prepares after large-source pressure");

    // A constrained injected budget refuses cleanly and does not poison the builder for the next
    // ordinary request.
    auto constrained = requestFor(*plan);
    constrained.pixelStorageByteLimit = std::size_t{8} * 1024U * 1024U;
    const auto refused = builder.build(plan, constrained);
    expectations.expect(
        !refused.hasValue() &&
            refused.diagnostic.code ==
                bloom::runtime::PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded,
        "a constrained injected budget refuses the large source cleanly");
    const auto recoveredAfterConstraint = builder.build(small, requestFor(*small));
    expectations.expect(recoveredAfterConstraint.hasValue(),
                        "a normal request still prepares after a constrained-budget refusal");
}

// The capacity-aware producer policy: pure functions of the ASSIGNED budget. A deliberately tiny
// budget must yield a tiny ceiling (no floor override), a high-capacity budget must admit a
// 6000x4000 RGBA32F source, and the arithmetic must be overflow-safe.
void testCapacityAwareProducerPolicy(Expectations& expectations) {
    constexpr std::size_t kSource6000x4000 = std::size_t{6000} * 4000 * 16U;
    constexpr std::size_t kTiny = std::size_t{8} * 1024U * 1024U;
    constexpr std::size_t kAmple = std::size_t{8} * 1024U * 1024U * 1024U;

    // Explicit tiny limits are honoured: never floored up to a legacy constant.
    expectations.expect(bloom::runtime::gpuProducerMaxImageBytesFor(kTiny) == kTiny / 4,
                        "a tiny assigned operation budget yields a tiny producer image ceiling");
    expectations.expect(bloom::runtime::gpuProducerMaxImageBytesFor(kTiny) <
                            std::size_t{256} * 1024U * 1024U,
                        "the producer ceiling does not floor a tiny budget up to 256 MiB");
    expectations.expect(bloom::runtime::gpuPreparedUploadCacheByteBudgetFor(kTiny) == kTiny / 16,
                        "a tiny assigned budget yields a tiny upload-cache ceiling");
    expectations.expect(bloom::runtime::gpuPreviewRequestByteAllowanceFor(kTiny) == kTiny / 4,
                        "a tiny assigned preview budget yields a tiny request allowance");

    // High capacity admits the 6000x4000 source and never exceeds the assigned budget.
    expectations.expect(bloom::runtime::gpuProducerMaxImageBytesFor(kAmple) >= kSource6000x4000,
                        "an ample assigned operation budget admits a 6000x4000 RGBA32F source");
    expectations.expect(bloom::runtime::gpuProducerMaxImageBytesFor(kAmple) <= kAmple,
                        "the producer ceiling never exceeds the assigned operation budget");
    expectations.expect(bloom::runtime::gpuPreparedUploadCacheByteBudgetFor(kAmple) >=
                            std::size_t{233} * 1024U * 1024U,
                        "an ample assigned budget retains the 233 MB source");

    // Overflow-safe: the largest representable budget still divides before multiplying.
    const std::size_t maximum = std::numeric_limits<std::size_t>::max();
    expectations.expect(bloom::runtime::gpuProducerMaxImageBytesFor(maximum) == (maximum / 4),
                        "the largest budget does not overflow the producer ceiling");
    expectations.expect(bloom::runtime::gpuPreparedUploadCacheByteBudgetFor(maximum) ==
                            (maximum / 16),
                        "the largest budget does not overflow the upload-cache ceiling");
    expectations.expect(bloom::runtime::gpuPreviewRequestByteAllowanceFor(maximum) == (maximum / 4),
                        "the largest budget does not overflow the request allowance");
}
