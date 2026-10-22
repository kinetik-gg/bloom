// Non-identity and identity still-image GPU colour tests: cold builder-to-executor parity, the
// proxied still (non-identity) and proxied identity/no-OCIO still through the native point
// resample, proxy-change decode reuse, cancellation, and changed-working-space decode reuse.
// Included by gpu_media_color_tests.cpp inside its anonymous namespace.

void testColdBuilderToExecutor(Expectations& expectations, bloom::render::GpuDevice& device,
                               const CpuCompositionEvaluator& evaluator, const Fixture& fixture,
                               const GpuSceneOcioContext& ocioContext) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto executor = GpuSceneExecutor::create(device, *cache.cache, GpuSceneExecutorBudgets{});
    expectations.expect(cache.hasValue() && executor.hasValue(), "cold: cache and executor host");
    if (!cache || !executor) {
        return;
    }
    const auto context = legacyPreparedMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    const auto plan = mediaPlan(format(8, 8), fixture.asset,
                                LayerValues{.position = {4.3, 3.1}, .opacity = 0.9}, 4100);
    const auto request = acesRequest(*plan, kAcesWorking);
    expectations.expect(expectMediaParity(expectations, *executor.executor, evaluator, builder,
                                          plan, request, 3, 2, "ACES2065-1 -> ACEScg"),
                        "cold: ACES input-to-working parity");

    // The raw colour split uses the same immutable source authority as the connected CPU arm.
    // Removing the pathname proves that retained selection performs no probe/read/hash/decode.
    auto warmRequest = request;
    warmRequest.mediaSourceRevisionPolicy = bloom::runtime::MediaSourceRevisionPolicy::Retained;
    auto held = fixture.path;
    held += ".held-revision";
    std::filesystem::rename(fixture.path, held);
    const auto warm = builder.build(plan, warmRequest);
    std::filesystem::rename(held, fixture.path);
    expectations.expect(warm.hasValue(), "warm: rebuild prepares");
    if (warm) {
        expectations.expect(warm.scene->mediaStatistics().uploadCacheHits == 1 &&
                                warm.scene->mediaStatistics().imageConversions == 0 &&
                                warm.scene->mediaStatistics().sourceRevisionCacheHits == 1 &&
                                warm.scene->mediaStatistics().sourceRevisionBytesHashed == 0 &&
                                warm.scene->mediaStatistics().sourceBytesRead == 0 &&
                                warm.scene->mediaStatistics().sourceIoNanoseconds == 0 &&
                                warm.scene->mediaStatistics().sourceHashNanoseconds == 0 &&
                                warm.scene->mediaStatistics().sourceProbeNanoseconds == 0 &&
                                warm.scene->mediaStatistics().imageDecodeNanoseconds == 0,
                            "warm: the decoded raw upload is reused without a re-decode");
        const auto run = runScene(*executor.executor, warm.scene, kSceneBudget);
        const auto counters = executor.executor->counters();
        expectations.expect(run.ready, "warm: the executor serves the cached scene");
        expectations.expect(counters.outputCacheHits >= 1, "warm: the unchanged output is a hit");
    }
}

// A proxied non-identity still: a full-resolution raw upload plus a GPU point-resample to the proxy
// window plus the OCIO transform over that window, compared to the CPU oracle at 2e-6. Asserts the
// exact command sequence, the small-proxy output window, a positive point-resample dispatch on a
// cold build, and zero additional resample/OCIO dispatch on the warm cached run.
void testProxiedStill(Expectations& expectations, bloom::render::GpuDevice& device,
                      const CpuCompositionEvaluator& evaluator, const Fixture& fixture,
                      const GpuSceneOcioContext& ocioContext) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto executor = GpuSceneExecutor::create(device, *cache.cache, GpuSceneExecutorBudgets{});
    expectations.expect(cache.hasValue() && executor.hasValue(), "proxy: cache and executor host");
    if (!cache || !executor) {
        return;
    }
    const auto context = legacyPreparedMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    const auto plan = mediaPlan(format(9, 6), fixture.asset,
                                LayerValues{.position = {4.5, 3.0}, .opacity = 0.9}, 4600);
    // A 6x4 proxy over a 9x6 composition: non-unit vertical/horizontal scales.
    const auto extent = bloom::render::ImageExtent::create(6, 4);
    expectations.expect(static_cast<bool>(extent), "proxy: the proxy extent builds");
    if (!extent) {
        return;
    }
    auto request = acesRequest(*plan, kAcesWorking);
    request.resolution = ProxyResolution{*extent.value()};

    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), "proxy: prepares");
    if (!prepared) {
        return;
    }
    const auto* upload = firstUpload(*prepared.scene);
    const auto* resample = firstResample(*prepared.scene);
    const auto* ocio = firstOcio(*prepared.scene);
    expectations.expect(upload != nullptr && resample != nullptr && ocio != nullptr,
                        "proxy: emits upload -> point-resample -> OCIO");
    if (upload == nullptr || resample == nullptr || ocio == nullptr) {
        return;
    }
    // The raw upload stays at the FULL source dimensions (3x2) even under a 6x4 proxy.
    const auto sourceWindow = ImageWindow::create(0, 0, 3, 2);
    expectations.expect(sourceWindow && upload->descriptor.dataWindow() == *sourceWindow.value(),
                        "proxy: the raw upload keeps the full source dimensions");
    // The proxy output is max(1, ceil(3 * 6/9)) x max(1, ceil(2 * 4/6)) = 2 x 2.
    const auto proxyWindow = ImageWindow::create(0, 0, 2, 2);
    expectations.expect(proxyWindow && resample->outputWindow == *proxyWindow.value(),
                        "proxy: the resample output is the CPU proxy window");
    expectations.expect(resample->inputKey == upload->semanticKey &&
                            resample->sourceWindow == upload->descriptor.dataWindow() &&
                            resample->horizontalScale > 0.0 && resample->horizontalScale < 1.0 &&
                            resample->verticalScale > 0.0 && resample->verticalScale < 1.0,
                        "proxy: the resample consumes the full-resolution upload at fractional "
                        "scales");

    // A changed proxy is a different resample/output but must NOT re-decode: the raw upload
    // identity is independent of the proxy scales and display descriptor.
    {
        const auto otherExtent = bloom::render::ImageExtent::create(3, 2);
        if (otherExtent) {
            auto otherRequest = acesRequest(*plan, kAcesWorking);
            otherRequest.resolution = ProxyResolution{*otherExtent.value()};
            const auto otherPrepared = builder.build(plan, otherRequest);
            const auto* otherUpload = otherPrepared ? firstUpload(*otherPrepared.scene) : nullptr;
            const auto* otherResample =
                otherPrepared ? firstResample(*otherPrepared.scene) : nullptr;
            expectations.expect(
                otherPrepared && otherUpload != nullptr && otherResample != nullptr &&
                    otherUpload->semanticKey == upload->semanticKey &&
                    otherPrepared.scene->mediaStatistics().uploadCacheHits == 1 &&
                    otherPrepared.scene->mediaStatistics().uploadCacheMisses == 0 &&
                    otherPrepared.scene->mediaStatistics().imageConversions == 0 &&
                    otherResample->semanticKey != resample->semanticKey,
                "proxy: a changed proxy reuses the decoded raw upload without a re-decode");
        }
    }
    expectations.expect(ocio->inputKey == resample->semanticKey &&
                            ocio->outputWindow == resample->outputWindow,
                        "proxy: the OCIO transform runs over the proxy window");

    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, "proxy: the CPU oracle evaluates");
    if (frame.frame() == nullptr) {
        return;
    }
    expectations.expect(prepared.scene->outputDescriptor() ==
                            *frame.frame()->processImage().descriptor(),
                        "proxy: output descriptor matches the CPU frame");

    const auto run = runScene(*executor.executor, prepared.scene, kSceneBudget);
    if (!run.ready) {
        std::cerr << "proxy: executor diagnostic code="
                  << static_cast<int>(executor.executor->diagnostic().code)
                  << " message=" << executor.executor->diagnostic().message << '\n';
    }
    expectations.expect(run.ready, "proxy: the executor reaches Ready");
    if (!run.ready || run.image == nullptr) {
        return;
    }
    const auto counters = executor.executor->counters();
    expectations.expect(counters.pointResampleDispatches >= 1,
                        "proxy: the cold build dispatched a GPU point-resample");
    expectations.expect(counters.ocioEffectDispatches >= 1,
                        "proxy: the OCIO transform was dispatched on the GPU");
    const auto readback = bloom::render::readbackResidentImage(*run.image, kReadbackBudget);
    expectations.expect(readback.hasValue(), "proxy: the output reads back for the oracle");
    if (!readback) {
        return;
    }
    const auto& cpuImage = frame.frame()->processImage();
    expectations.expect(readback.pixels.size() == cpuImage.pixels().size(),
                        "proxy: the pixel count matches the CPU frame");
    if (readback.pixels.size() != cpuImage.pixels().size()) {
        return;
    }
    expectations.expect(pixelsClose(readback.pixels, cpuImage.pixels()),
                        "proxy: every pixel is within the 2e-6 process gate");
    // Alpha is carried through both the exact nearest gather and the OCIO CST unchanged, so it must
    // match the CPU oracle bit for bit (not merely within a tolerance).
    {
        bool alphaExact = true;
        for (std::size_t i = 0; i < readback.pixels.size(); ++i) {
            if (readback.pixels[i].alpha() != cpuImage.pixels()[i].alpha()) {
                alphaExact = false;
                break;
            }
        }
        expectations.expect(alphaExact, "proxy: every alpha lane is exactly the CPU oracle's");
    }
    // No host per-pixel resampling: the only resampling command is the dispatched GPU
    // PointResampleV1, and the raw upload that feeds it is the full-resolution source.
    expectations.expect(prepared.scene->mediaStatistics().ocioCommandPreparations == 1 &&
                            counters.pointResampleDispatches == 1 &&
                            counters.ocioEffectDispatches == 1,
                        "proxy: exactly one GPU point-resample and one GPU OCIO dispatch ran");

    // Warm: the same scene is served from the content cache with zero additional dispatches.
    const auto warmPrepared = builder.build(plan, request);
    const auto warm = runScene(*executor.executor, warmPrepared.scene, kSceneBudget);
    expectations.expect(warm.ready, "proxy: the warm scene serves from cache");
    const auto warmCounters = executor.executor->counters();
    expectations.expect(warmCounters.pointResampleDispatches == counters.pointResampleDispatches &&
                            warmCounters.ocioEffectDispatches == counters.ocioEffectDispatches,
                        "proxy: a warm scene performs zero additional resample/OCIO dispatches");

    // Tiny budget: a proxy scene whose full-resolution upload is larger than the budget is refused
    // at begin without any native work, and the executor stays reusable.
    const auto tightPrepared = builder.build(plan, request);
    const auto tight = executor.executor->begin(tightPrepared.scene, 1);
    expectations.expect(tight.code == GpuSceneExecutorDiagnosticCode::OverBudget,
                        "proxy: a one-byte budget is refused");
    expectations.expect(executor.executor->state() ==
                            bloom::runtime::GpuSceneExecutorJobState::Idle,
                        "proxy: the budget refusal leaves the executor idle");
}

void testRawBackingAcrossRequestGeometry(Expectations& expectations,
                                         bloom::render::GpuDevice& device,
                                         const CpuCompositionEvaluator& evaluator,
                                         const Fixture& fixture,
                                         const GpuSceneOcioContext& ocioContext) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "raw geometry: cache host");
    if (!cache)
        return;
    auto executor = GpuSceneExecutor::create(device, *cache.cache, GpuSceneExecutorBudgets{});
    expectations.expect(executor.hasValue(), "raw geometry: executor host");
    if (!executor)
        return;
    const CpuGpuSceneBuilder builder(nullptr, legacyPreparedMediaContext(evaluator), ocioContext);
    const auto square =
        mediaPlan(format(9, 7), fixture.asset, LayerValues{.position = {4.5, 3.5}}, 48100);
    const auto wide = mediaPlan(format(9, 7, pixelAspect(4, 3)), fixture.asset,
                                LayerValues{.position = {4.5, 3.5}}, 48200);
    const auto halfExtent = bloom::render::ImageExtent::create(5, 4);
    if (!halfExtent) {
        expectations.expect(false, "raw geometry: half extent builds");
        return;
    }
    auto full = acesRequest(*square, kAcesWorking);
    auto half = full;
    half.resolution = ProxyResolution{*halfExtent.value()};
    auto changedPar = acesRequest(*wide, kAcesWorking);
    const std::array requests{std::pair{square, full}, std::pair{square, half},
                              std::pair{square, full}, std::pair{wide, changedPar}};
    const Rgba32fImage* sourceBacking = nullptr;
    std::string squareUploadKey;
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto& [plan, request] = requests[index];
        const auto prepared = builder.build(plan, request);
        const auto* upload = prepared ? firstUpload(*prepared.scene) : nullptr;
        expectations.expect(upload != nullptr, "raw geometry: each request prepares an upload");
        if (upload == nullptr)
            return;
        if (index == 0) {
            sourceBacking = upload->image.get();
            squareUploadKey = upload->semanticKey;
        } else {
            expectations.expect(upload->image.get() == sourceBacking &&
                                    prepared.scene->mediaStatistics().uploadCacheHits == 1,
                                "raw geometry: all requests share one decoded allocation");
        }
        expectations.expect(upload->descriptor.displayWindow() ==
                                    prepared.scene->outputDescriptor().displayWindow() &&
                                upload->descriptor.pixelAspect() ==
                                    prepared.scene->outputDescriptor().pixelAspect(),
                            "raw geometry: upload uses the current display window and PAR");
        expectations.expect((index == 3) ? upload->semanticKey != squareUploadKey
                                         : upload->semanticKey == squareUploadKey,
                            "raw geometry: resident identity follows request descriptor");
        auto oracle = request;
        oracle.bypassOperationCache = true;
        const auto frame = evaluator.evaluate(plan, oracle, {});
        const auto run = runScene(*executor.executor, prepared.scene, kSceneBudget);
        const auto readback =
            run.ready && run.image != nullptr
                ? bloom::render::readbackResidentImage(*run.image, kReadbackBudget)
                : bloom::render::GpuImageReadback{};
        expectations.expect(
            frame.frame() != nullptr && run.ready && run.image != nullptr &&
                descriptorMatchesScene(*run.image, *prepared.scene) && readback.hasValue() &&
                pixelsClose(readback.pixels, frame.frame()->processImage().pixels()),
            "raw geometry: Full/Half/Full/PAR native output matches CPU");
    }
}

// An identity (input colour space == working colour space) proxied still under a configured GPU
// colour context: no OCIO program is prepared, but the full-resolution raw upload still goes
// through the native PointResampleV1 gather. This is the identity/no-OCIO proxy path the split must
// not leave on the CPU.
void testProxiedIdentityStill(Expectations& expectations, bloom::render::GpuDevice& device,
                              const CpuCompositionEvaluator& evaluator, const Fixture& fixture,
                              const GpuSceneOcioContext& ocioContext) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto executor = GpuSceneExecutor::create(device, *cache.cache, GpuSceneExecutorBudgets{});
    expectations.expect(cache.hasValue() && executor.hasValue(),
                        "identity proxy: cache and executor host");
    if (!cache || !executor) {
        return;
    }
    const auto context = legacyPreparedMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    document::AssetRecord asset = fixture.asset;
    // Explicit input == working space: the resolved transform is exact identity, so no OCIO command
    // is emitted even though a preparer is configured.
    asset.interpretation.inputColorSpaceId = std::string{kAcesWorking};
    const auto plan =
        mediaPlan(format(9, 6), asset, LayerValues{.position = {4.5, 3.0}, .opacity = 0.9}, 4750);
    const auto extent = bloom::render::ImageExtent::create(6, 4);
    expectations.expect(static_cast<bool>(extent), "identity proxy: extent builds");
    if (!extent) {
        return;
    }
    auto request = acesRequest(*plan, kAcesWorking);
    request.resolution = ProxyResolution{*extent.value()};

    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), "identity proxy: prepares");
    if (!prepared) {
        return;
    }
    const auto* upload = firstUpload(*prepared.scene);
    const auto* resample = firstResample(*prepared.scene);
    const auto* ocio = firstOcio(*prepared.scene);
    expectations.expect(upload != nullptr && resample != nullptr && ocio == nullptr,
                        "identity proxy: full-source upload + native point-resample, no OCIO");
    if (upload == nullptr || resample == nullptr || ocio != nullptr) {
        return;
    }
    const auto sourceWindow = ImageWindow::create(0, 0, 3, 2);
    expectations.expect(sourceWindow && upload->descriptor.dataWindow() == *sourceWindow.value(),
                        "identity proxy: the raw upload keeps the full source dimensions");
    const auto proxyWindow = ImageWindow::create(0, 0, 2, 2);
    expectations.expect(proxyWindow && resample->outputWindow == *proxyWindow.value(),
                        "identity proxy: the resample output is the CPU proxy window");
    expectations.expect(resample->inputKey == upload->semanticKey &&
                            resample->displayWindow ==
                                prepared.scene->outputDescriptor().displayWindow(),
                        "identity proxy: the resample carries the composition display window");

    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, "identity proxy: the CPU oracle evaluates");
    if (frame.frame() == nullptr) {
        return;
    }
    expectations.expect(prepared.scene->outputDescriptor() ==
                            *frame.frame()->processImage().descriptor(),
                        "identity proxy: output descriptor matches the CPU frame");

    const auto run = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(run.ready, "identity proxy: the executor reaches Ready");
    if (!run.ready || run.image == nullptr) {
        return;
    }
    const auto counters = executor.executor->counters();
    expectations.expect(counters.pointResampleDispatches == 1 && counters.ocioEffectDispatches == 0,
                        "identity proxy: exactly one GPU point-resample, zero OCIO dispatches");
    const auto readback = bloom::render::readbackResidentImage(*run.image, kReadbackBudget);
    expectations.expect(readback.hasValue(), "identity proxy: the output reads back");
    if (!readback) {
        return;
    }
    const auto& cpuImage = frame.frame()->processImage();
    expectations.expect(readback.pixels.size() == cpuImage.pixels().size() &&
                            pixelsClose(readback.pixels, cpuImage.pixels()),
                        "identity proxy: every pixel is within the 2e-6 process gate");
    {
        bool alphaExact = true;
        for (std::size_t i = 0; i < readback.pixels.size() && i < cpuImage.pixels().size(); ++i) {
            if (readback.pixels[i].alpha() != cpuImage.pixels()[i].alpha()) {
                alphaExact = false;
                break;
            }
        }
        expectations.expect(alphaExact, "identity proxy: every alpha lane is exactly the CPU's");
    }

    const auto warmPrepared = builder.build(plan, request);
    const auto warm = runScene(*executor.executor, warmPrepared.scene, kSceneBudget);
    const auto warmCounters = executor.executor->counters();
    expectations.expect(
        warm.ready && warmCounters.pointResampleDispatches == counters.pointResampleDispatches,
        "identity proxy: a warm scene performs zero additional resample dispatches");
}

void testProxyCancellation(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                           const Fixture& fixture, const GpuSceneOcioContext& ocioContext) {
    const auto context = legacyPreparedMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    const auto plan = mediaPlan(format(9, 6), fixture.asset,
                                LayerValues{.position = {4.5, 3.0}, .opacity = 0.9}, 4700);
    const auto extent = bloom::render::ImageExtent::create(6, 4);
    if (!extent) {
        return;
    }
    auto request = acesRequest(*plan, kAcesWorking);
    request.resolution = ProxyResolution{*extent.value()};
    const auto token = makeCancelledToken();
    expectations.expect(token.isCancellationRequested(), "proxy cancel: the token is cancelled");
    const auto prepared = builder.build(plan, request, token);
    expectations.expect(prepared.diagnostic.code ==
                                bloom::runtime::PreparedGpuSceneDiagnosticCode::Cancelled &&
                            !prepared.hasValue(),
                        "proxy cancel: a pre-cancelled request publishes no scene");
}

void testChangedWorkingSpaceReusesDecode(Expectations& expectations,
                                         const CpuCompositionEvaluator& evaluator,
                                         const Fixture& fixture,
                                         const GpuSceneOcioContext& ocioContext) {
    const auto context = legacyPreparedMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    // An explicit texture input keeps the transform non-identity for both working spaces, so the
    // comparison isolates the working space (the decoded bytes are identical).
    document::AssetRecord asset = fixture.asset;
    asset.interpretation.inputColorSpaceId = std::string{kAcesTextureInput};
    const auto plan =
        mediaPlan(format(8, 8), asset, LayerValues{.position = {4.3, 3.1}, .opacity = 0.9}, 4200);
    const auto first = builder.build(plan, acesRequest(*plan, kAcesWorking));
    const auto second = builder.build(plan, acesRequest(*plan, kAcesAlternateWorking));
    expectations.expect(first.hasValue() && second.hasValue(),
                        "working space: both builds prepare");
    if (!first || !second) {
        return;
    }
    const auto* uploadFirst = firstUpload(*first.scene);
    const auto* uploadSecond = firstUpload(*second.scene);
    const auto* ocioFirst = firstOcio(*first.scene);
    const auto* ocioSecond = firstOcio(*second.scene);
    expectations.expect(uploadFirst != nullptr && uploadSecond != nullptr && ocioFirst != nullptr &&
                            ocioSecond != nullptr,
                        "working space: both scenes emit a raw upload and an effect");
    if (uploadFirst == nullptr || uploadSecond == nullptr || ocioFirst == nullptr ||
        ocioSecond == nullptr) {
        return;
    }
    expectations.expect(uploadFirst->semanticKey == uploadSecond->semanticKey &&
                            second.scene->mediaStatistics().uploadCacheHits == 1 &&
                            second.scene->mediaStatistics().uploadCacheMisses == 0,
                        "working space: a changed working space reuses the decoded raw upload");
    expectations.expect(ocioFirst->semanticKey != ocioSecond->semanticKey,
                        "working space: the changed transform has a distinct effect identity");
}
