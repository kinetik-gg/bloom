// Real-decoded video GPU colour tests: non-identity and identity/no-OCIO input-to-working parity
// for the planar8 and alpha16 codecs (including proxied frames), warm decode reuse, changed
// working-space decode reuse, missing/corrupt diagnostics, and pre-cancellation.
// Included by gpu_media_color_tests.cpp inside its anonymous namespace.

void testRealVideoColour(Expectations& expectations, bloom::render::GpuDevice& device,
                         CpuCompositionEvaluator& evaluator, const std::filesystem::path& fixtures,
                         const GpuSceneOcioContext& ocioContext) {
    if (fixtures.empty() || !std::filesystem::is_directory(fixtures)) {
        std::cout << "NOTE: real-video colour vectors skipped (no --fixtures directory)\n";
        return;
    }
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto executor = GpuSceneExecutor::create(device, *cache.cache, GpuSceneExecutorBudgets{});
    expectations.expect(cache.hasValue() && executor.hasValue(),
                        "video: the cache and executor host");
    if (!cache || !executor) {
        return;
    }
    evaluator.setAssetBaseDirectory(fixtures);
    evaluator.setVideoCacheByteBudget(std::size_t{64} << 20U);
    const auto context = legacyPreparedMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    const std::array<std::pair<std::string_view, std::string_view>, 2> cases{
        std::pair<std::string_view, std::string_view>{"numbered-prores.mov", "planar8"},
        std::pair<std::string_view, std::string_view>{"alpha-prores.mov", "alpha16"}};
    std::uint64_t idBase = 4500;
    for (const auto& [file, label] : cases) {
        const std::string tag{label};
        const auto path = fixtures / std::string{file};
        expectations.expect(std::filesystem::is_regular_file(path),
                            tag + ": the real decoded fixture exists");
        if (!std::filesystem::is_regular_file(path)) {
            continue;
        }
        document::AssetRecord asset;
        try {
            asset = videoAsset(path, idBase + 100);
        } catch (const std::exception& error) {
            expectations.expect(false, tag + ": fixture probe failed: " + error.what());
            continue;
        }
        asset.interpretation.inputColorSpaceId = std::string{"ACES2065-1"};
        const auto centre = LayerValues{.position = {static_cast<double>(asset.width) * 0.5,
                                                     static_cast<double>(asset.height) * 0.5},
                                        .opacity = 0.9};
        const auto plan = videoPlan(format(asset.width, asset.height), asset, centre, idBase);
        const auto request = acesRequest(*plan, kAcesWorking);
        expectations.expect(expectMediaParity(expectations, *executor.executor, evaluator, builder,
                                              plan, request, asset.width, asset.height,
                                              tag + " video ACES"),
                            tag + ": video input-to-working parity");

        const auto warm = builder.build(plan, request);
        expectations.expect(warm.hasValue() && warm.scene->mediaStatistics().uploadCacheHits == 1 &&
                                warm.scene->mediaStatistics().videoConversions == 0,
                            tag + ": the decoded video frame is reused without a re-decode");

        // A proxied video frame: the raw upload stays full-resolution, and the GPU gathers the
        // proxy before the OCIO transform. The CPU proxy oracle is the unchanged evaluator frame.
        const auto proxyExtent =
            bloom::render::ImageExtent::create((static_cast<std::uint64_t>(asset.width) + 1) / 2,
                                               (static_cast<std::uint64_t>(asset.height) + 1) / 2);
        expectations.expect(static_cast<bool>(proxyExtent), tag + ": proxy extent builds");
        if (proxyExtent) {
            auto proxyRequest = acesRequest(*plan, kAcesWorking);
            proxyRequest.resolution = ProxyResolution{*proxyExtent.value()};
            const auto proxyPrepared = builder.build(plan, proxyRequest);
            const auto* proxyUpload = proxyPrepared ? firstUpload(*proxyPrepared.scene) : nullptr;
            const auto* proxyResample =
                proxyPrepared ? firstResample(*proxyPrepared.scene) : nullptr;
            const auto* proxyOcio = proxyPrepared ? firstOcio(*proxyPrepared.scene) : nullptr;
            expectations.expect(proxyUpload != nullptr && proxyResample != nullptr &&
                                    proxyOcio != nullptr,
                                tag + ": a proxied frame emits upload -> point-resample -> OCIO");
            if (proxyUpload != nullptr && proxyResample != nullptr && proxyOcio != nullptr) {
                const auto sourceWindow = ImageWindow::create(0, 0, asset.width, asset.height);
                expectations.expect(
                    sourceWindow && proxyUpload->descriptor.dataWindow() == *sourceWindow.value(),
                    tag + ": the proxied raw upload keeps the full frame dimensions");
                expectations.expect(proxyOcio->outputWindow == proxyResample->outputWindow,
                                    tag + ": the proxied OCIO runs over the proxy window");
                auto proxyOracle = proxyRequest;
                proxyOracle.bypassOperationCache = true;
                const auto proxyFrame = evaluator.evaluate(plan, proxyOracle, {});
                const auto proxyRun =
                    runScene(*executor.executor, proxyPrepared.scene, kSceneBudget);
                expectations.expect(proxyRun.ready, tag + ": the proxied frame completes");
                if (proxyFrame.frame() != nullptr && proxyRun.ready && proxyRun.image != nullptr) {
                    const auto proxyReadback =
                        bloom::render::readbackResidentImage(*proxyRun.image, kReadbackBudget);
                    expectations.expect(
                        proxyReadback.hasValue() &&
                            proxyReadback.pixels.size() ==
                                proxyFrame.frame()->processImage().pixels().size() &&
                            pixelsClose(proxyReadback.pixels,
                                        proxyFrame.frame()->processImage().pixels()),
                        tag + ": the proxied frame matches the CPU oracle at 2e-6");
                }
            }
        }

        // An identity (input == working) proxied video frame: no OCIO program, but the full-source
        // raw upload still goes through the native PointResampleV1 gather.
        if (proxyExtent) {
            document::AssetRecord identityAsset = asset;
            identityAsset.interpretation.inputColorSpaceId = std::string{kAcesWorking};
            const auto identityPlan =
                videoPlan(format(asset.width, asset.height), identityAsset, centre, idBase + 4);
            auto identityRequest = acesRequest(*identityPlan, kAcesWorking);
            identityRequest.resolution = ProxyResolution{*proxyExtent.value()};
            const auto identityPrepared = builder.build(identityPlan, identityRequest);
            const auto* identityUpload =
                identityPrepared ? firstUpload(*identityPrepared.scene) : nullptr;
            const auto* identityResample =
                identityPrepared ? firstResample(*identityPrepared.scene) : nullptr;
            const auto* identityOcio =
                identityPrepared ? firstOcio(*identityPrepared.scene) : nullptr;
            expectations.expect(identityUpload != nullptr && identityResample != nullptr &&
                                    identityOcio == nullptr,
                                tag + ": an identity proxied frame emits upload + resample, no "
                                      "OCIO");
            if (identityPrepared && identityUpload != nullptr && identityResample != nullptr) {
                auto identityOracle = identityRequest;
                identityOracle.bypassOperationCache = true;
                const auto identityFrame = evaluator.evaluate(identityPlan, identityOracle, {});
                const auto identityRun =
                    runScene(*executor.executor, identityPrepared.scene, kSceneBudget);
                expectations.expect(identityRun.ready,
                                    tag + ": the identity proxied frame completes");
                if (identityFrame.frame() != nullptr && identityRun.ready &&
                    identityRun.image != nullptr) {
                    const auto identityReadback =
                        bloom::render::readbackResidentImage(*identityRun.image, kReadbackBudget);
                    expectations.expect(
                        identityReadback.hasValue() &&
                            identityReadback.pixels.size() ==
                                identityFrame.frame()->processImage().pixels().size() &&
                            pixelsClose(identityReadback.pixels,
                                        identityFrame.frame()->processImage().pixels()),
                        tag + ": the identity proxied frame matches the CPU oracle at 2e-6");
                }
            }
        }

        document::AssetRecord reuseAsset = asset;
        reuseAsset.interpretation.inputColorSpaceId = std::string{kAcesTextureInput};
        const auto reusePlan =
            videoPlan(format(asset.width, asset.height), reuseAsset, centre, idBase + 1);
        const auto first = builder.build(reusePlan, acesRequest(*reusePlan, kAcesWorking));
        const auto second =
            builder.build(reusePlan, acesRequest(*reusePlan, kAcesAlternateWorking));
        const auto* uploadFirst = first ? firstUpload(*first.scene) : nullptr;
        const auto* uploadSecond = second ? firstUpload(*second.scene) : nullptr;
        const auto* ocioFirst = first ? firstOcio(*first.scene) : nullptr;
        const auto* ocioSecond = second ? firstOcio(*second.scene) : nullptr;
        expectations.expect(
            uploadFirst != nullptr && uploadSecond != nullptr &&
                uploadFirst->semanticKey == uploadSecond->semanticKey && second.hasValue() &&
                second.scene->mediaStatistics().uploadCacheHits == 1 && ocioFirst != nullptr &&
                ocioSecond != nullptr && ocioFirst->semanticKey != ocioSecond->semanticKey,
            tag + ": a changed working space reuses the decoded video frame");

        document::AssetRecord missing = asset;
        missing.locator.path = "missing.mov";
        missing.locator.relinkHint = "file:" + (fixtures / "missing.mov").string();
        const auto missingPlan =
            videoPlan(format(asset.width, asset.height), missing, centre, idBase + 2);
        const auto missingResult =
            builder.build(missingPlan, acesRequest(*missingPlan, kAcesWorking));
        expectations.expect(!missingResult.hasValue() && !missingResult.diagnostic.message.empty(),
                            tag + ": missing media fails closed with a diagnostic");

        const auto corruptPath =
            std::filesystem::temp_directory_path() /
            ("bloom_gpu_media_color_corrupt_" + std::to_string(idBase) + ".mov");
        {
            std::ofstream corrupt(corruptPath, std::ios::binary | std::ios::trunc);
            corrupt << "not a movie";
        }
        document::AssetRecord corrupt = asset;
        corrupt.locator.path = corruptPath.filename().string();
        corrupt.locator.relinkHint = "file:" + corruptPath.string();
        const auto corruptPlan =
            videoPlan(format(asset.width, asset.height), corrupt, centre, idBase + 3);
        const auto corruptResult =
            builder.build(corruptPlan, acesRequest(*corruptPlan, kAcesWorking));
        expectations.expect(!corruptResult.hasValue() && !corruptResult.diagnostic.message.empty(),
                            tag + ": corrupt media fails closed with a diagnostic");
        std::error_code ignored;
        std::filesystem::remove(corruptPath, ignored);
        idBase += 10;
    }
    // Issue #365: a PQ-tagged BT.2020 stream resolves automatically to the config's Rec.2100 PQ
    // space, an untagged stream is interpreted through an explicit Rec.2100 PQ choice, and a
    // tagged Rec.709 stream uses filtered 4:2:0 chroma. All keep GPU/CPU input-to-working parity;
    // the PQ cases exercise the exact half-domain LUT lookup.
    // Owner decision (2026-10-24): untagged HD in Auto renders under the assumed Rec.709 on both
    // routes. Issue #352: an 8K UHD source keeps the same parity on the GPU preview route.
    using Interpretation = std::tuple<std::string_view, std::string_view, std::string_view>;
    const std::array<Interpretation, 5> interpretations{
        Interpretation{"pq2020-h264.mp4", "pq auto", ""},
        Interpretation{"untagged-h264.mp4", "untagged pq", "Rec.2100-PQ - Display"},
        Interpretation{"numbered-h264.mp4", "rec709 h264", "ACES2065-1"},
        Interpretation{"untagged-hd-prores.mov", "untagged hd auto", ""},
        Interpretation{"uhd8k-prores.mov", "8k uhd prores", ""}};
    for (const auto& [file, label, explicitSpace] : interpretations) {
        const std::string tag{label};
        const auto path = fixtures / std::string{file};
        document::AssetRecord asset;
        try {
            asset = videoAsset(path, idBase + 100);
        } catch (const std::exception& error) {
            expectations.expect(false, tag + ": fixture probe failed: " + error.what());
            continue;
        }
        asset.interpretation.inputColorSpaceId = std::string{explicitSpace};
        const auto plan =
            videoPlan(format(asset.width, asset.height), asset,
                      LayerValues{.position = {static_cast<double>(asset.width) * 0.5,
                                               static_cast<double>(asset.height) * 0.5}},
                      idBase);
        // An 8K frame (530 MB as float RGBA) exceeds this test process's default pixel allowance
        // and the executor's per-image ceiling, which follow the process memory ledger; size both
        // for the frame, as a host with an 8K-capable budget does.
        const auto frameBytes =
            static_cast<std::size_t>(asset.width) * asset.height * sizeof(float) * 4U;
        auto request = acesRequest(*plan, kAcesWorking);
        request.pixelStorageByteLimit =
            std::max<std::size_t>(request.pixelStorageByteLimit, frameBytes * 4U);
        auto* run = &*executor.executor;
        // Its own cache, declared first so it outlives the executor bound to it.
        decltype(GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget})) largeCache;
        decltype(GpuSceneExecutor::create(device, *cache.cache, GpuSceneExecutorBudgets{})) large;
        if (frameBytes > GpuSceneExecutorBudgets{}.maxImageBytes) {
            largeCache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
            GpuSceneExecutorBudgets budgets;
            budgets.maxImageBytes = frameBytes * 4U;
            if (largeCache)
                large = GpuSceneExecutor::create(device, *largeCache.cache, budgets);
            expectations.expect(largeCache.hasValue() && large.hasValue(),
                                tag + ": an 8K-sized executor hosts");
            if (!large.hasValue())
                continue;
            run = &*large.executor;
        }
        expectations.expect(expectMediaParity(expectations, *run, evaluator, builder, plan, request,
                                              asset.width, asset.height,
                                              tag + " video interpretation"),
                            tag + ": interpreted video input-to-working parity");
        idBase += 10;
    }
    // Issue #365: HLG streams resolve to the synthesized scene-light Rec.2100 HLG space under both
    // pinned configs and keep the same parity gate.
    for (const bool aces : {true, false}) {
        const std::string tag = aces ? "hlg aces" : "hlg neutral";
        document::AssetRecord asset;
        try {
            asset = videoAsset(fixtures / "hlg2020-h264.mp4", idBase + 100);
        } catch (const std::exception& error) {
            expectations.expect(false, tag + ": fixture probe failed: " + error.what());
            continue;
        }
        const auto plan =
            videoPlan(format(asset.width, asset.height), asset,
                      LayerValues{.position = {static_cast<double>(asset.width) * 0.5,
                                               static_cast<double>(asset.height) * 0.5}},
                      idBase);
        // A fresh evaluator per config keeps each build cold (no shared raw-upload cache).
        CpuCompositionEvaluator hlgEvaluator;
        hlgEvaluator.setAssetBaseDirectory(fixtures);
        hlgEvaluator.setVideoCacheByteBudget(std::size_t{64} << 20U);
        const auto hlgContext = legacyPreparedMediaContext(hlgEvaluator);
        const CpuGpuSceneBuilder hlgBuilder(nullptr, hlgContext, ocioContext);
        const auto request = aces ? acesRequest(*plan, kAcesWorking) : requestFor(*plan);
        expectations.expect(expectMediaParity(expectations, *executor.executor, hlgEvaluator,
                                              hlgBuilder, plan, request, asset.width, asset.height,
                                              tag + " video interpretation"),
                            tag + ": HLG input-to-working parity");
        idBase += 10;
    }
}

// Issue #381 / ADR 0032, GPU route: ordinary BT.709-tagged and untagged H.264 in Auto under Bloom
// Neutral resolve to the sRGB texture space, whose decode to the working space is a dispatched GPU
// OCIO effect with CPU parity. The frame read back once and shown through the default sRGB view
// matches FFmpeg's player-equivalent decode within the same tolerance as the CPU reference route.
void testPlayerReferenceVideo(Expectations& expectations, bloom::render::GpuDevice& device,
                              const std::filesystem::path& fixtures,
                              const GpuSceneOcioContext& ocioContext) {
    namespace reference = bloom::test::video_player_reference;
    if (fixtures.empty() || !std::filesystem::is_directory(fixtures))
        return;
    const auto config = bloom::runtime::detail::resolveInputColorConfig({});
    expectations.expect(config.has_value(), "player reference: the Neutral config resolves");
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto executor = GpuSceneExecutor::create(device, *cache.cache, GpuSceneExecutorBudgets{});
    expectations.expect(cache.hasValue() && executor.hasValue(),
                        "player reference: the cache and executor host");
    if (!config || !cache || !executor)
        return;
    std::uint64_t idBase = 4900;
    for (const auto& fixture : reference::kFixtures) {
        const std::string tag = "player reference " + std::string(fixture.label);
        document::AssetRecord asset;
        try {
            asset = videoAsset(fixtures / std::string(fixture.clip), idBase + 100);
        } catch (const std::exception& error) {
            expectations.expect(false, tag + ": fixture probe failed: " + error.what());
            continue;
        }
        const auto plan =
            videoPlan(format(asset.width, asset.height), asset,
                      LayerValues{.position = {static_cast<double>(asset.width) * 0.5,
                                               static_cast<double>(asset.height) * 0.5}},
                      idBase);
        CpuCompositionEvaluator evaluator;
        evaluator.setAssetBaseDirectory(fixtures);
        evaluator.setVideoCacheByteBudget(std::size_t{64} << 20U);
        const auto context = legacyPreparedMediaContext(evaluator);
        const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
        const auto request = requestFor(*plan);
        expectations.expect(expectMediaParity(expectations, *executor.executor, evaluator, builder,
                                              plan, request, asset.width, asset.height, tag),
                            tag + ": sRGB texture input-to-working parity on the GPU");
        const auto prepared = builder.build(plan, request);
        expectations.expect(prepared.hasValue(), tag + ": the warm frame prepares");
        if (!prepared)
            continue;
        const auto run = runScene(*executor.executor, prepared.scene, kSceneBudget);
        expectations.expect(run.ready && run.image != nullptr, tag + ": the GPU frame completes");
        if (!run.ready || run.image == nullptr)
            continue;
        const auto readback = bloom::render::readbackResidentImage(*run.image, kReadbackBudget);
        expectations.expect(readback.hasValue(), tag + ": the GPU frame reads back once");
        if (!readback)
            continue;
        const auto comparison = reference::compare(*config, readback.pixels,
                                                   fixtures / std::string(fixture.reference));
        std::cout << tag << " (GPU): " << comparison.summary() << '\n';
        expectations.expect(comparison.passes(),
                            tag + ": the GPU frame matches FFmpeg's player decode, " +
                                comparison.summary());
        idBase += 10;
    }
}

void testVideoCancellation(Expectations& expectations, const std::filesystem::path& fixtures,
                           const GpuSceneOcioContext& ocioContext) {
    if (fixtures.empty() || !std::filesystem::is_directory(fixtures)) {
        return;
    }
    const auto path = fixtures / "numbered-prores.mov";
    if (!std::filesystem::is_regular_file(path)) {
        return;
    }
    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(fixtures);
    const auto context = legacyPreparedMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    document::AssetRecord asset;
    try {
        asset = videoAsset(path, 4700);
    } catch (const std::exception&) {
        return;
    }
    asset.interpretation.inputColorSpaceId = std::string{"ACES2065-1"};
    const auto plan = videoPlan(format(asset.width, asset.height), asset,
                                LayerValues{.position = {static_cast<double>(asset.width) * 0.5,
                                                         static_cast<double>(asset.height) * 0.5}},
                                4700);
    const auto token = makeCancelledToken();
    expectations.expect(token.isCancellationRequested(), "video cancel: the token is cancelled");
    const auto prepared = builder.build(plan, acesRequest(*plan, kAcesWorking), token);
    expectations.expect(prepared.diagnostic.code ==
                                bloom::runtime::PreparedGpuSceneDiagnosticCode::Cancelled &&
                            !prepared.hasValue(),
                        "video cancel: a pre-cancelled request publishes no scene");
}

void testMissingAndCorruptMedia(Expectations& expectations, const Fixture& fixture,
                                const GpuSceneOcioContext& ocioContext) {
    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(fixture.directory);
    const auto context = legacyPreparedMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);

    // Missing file: a valid asset record whose locator points at a file that is not there.
    const auto missingPath = fixture.directory / "missing.exr";
    document::AssetRecord missingAsset = fixture.asset;
    missingAsset.locator.path = missingPath.filename().string();
    missingAsset.locator.relinkHint = "file:" + missingPath.string();
    const auto missingPlan =
        mediaPlan(format(8, 8), missingAsset, LayerValues{.position = {4.3, 3.1}}, 4300);
    const auto missing = builder.build(missingPlan, acesRequest(*missingPlan, kAcesWorking));
    expectations.expect(!missing.hasValue(), "missing media: preparation fails closed");
    expectations.expect(missing.diagnostic.code ==
                                bloom::runtime::PreparedGpuSceneDiagnosticCode::MediaUnavailable &&
                            !missing.diagnostic.message.empty(),
                        "missing media: the diagnostic is honest and non-empty");

    // Corrupt file: an existing file whose bytes are no longer a valid EXR.
    const auto corruptPath = fixture.directory / "corrupt.exr";
    {
        std::ofstream corrupt(corruptPath, std::ios::binary | std::ios::trunc);
        corrupt << "not an exr";
    }
    document::AssetRecord corruptAsset = fixture.asset;
    corruptAsset.locator.path = corruptPath.filename().string();
    corruptAsset.locator.relinkHint = "file:" + corruptPath.string();
    const auto corruptPlan =
        mediaPlan(format(8, 8), corruptAsset, LayerValues{.position = {4.3, 3.1}}, 4400);
    const auto corrupt = builder.build(corruptPlan, acesRequest(*corruptPlan, kAcesWorking));
    expectations.expect(!corrupt.hasValue(), "corrupt media: preparation fails closed");
    expectations.expect(!corrupt.diagnostic.message.empty(),
                        "corrupt media: the diagnostic is honest and non-empty");
}

// Issue #365: the Rec.2100 PQ input is a half-domain inverse 1D LUT. Sweep the PQ code range
// (with out-of-range and negative values) through a still so every pixel exercises the exact
// half index/fraction lookup against the CPU oracle at the unchanged 2e-6 gate.
void testPqInputSweep(Expectations& expectations, bloom::render::GpuDevice& device,
                      CpuCompositionEvaluator& evaluator, const GpuSceneOcioContext& ocioContext) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto executor = GpuSceneExecutor::create(device, *cache.cache, GpuSceneExecutorBudgets{});
    expectations.expect(cache.hasValue() && executor.hasValue(), "pq sweep: the executor hosts");
    if (!cache || !executor) {
        return;
    }
    const auto directory = std::filesystem::temp_directory_path() / "bloom_gpu_pq_sweep";
    std::filesystem::create_directories(directory);
    constexpr int width = 128, height = 128;
    std::vector<ExrPixel> pixels;
    pixels.reserve(static_cast<std::size_t>(width) * height);
    for (int i = 0; i < width * height; ++i) {
        const auto t = static_cast<float>(i) / static_cast<float>(width * height - 1);
        pixels.push_back({t, 1.2F * t - 0.1F, 0.7F + 0.3F * t, 1.0F});
    }
    const auto path = directory / "pq_sweep.exr";
    writeExrRgbaWithChromaticities(path, width, height, pixels, acesAp0Chromaticities());
    auto asset = imageAsset(path, "pq_sweep", 9100);
    asset.interpretation.inputColorSpaceId = std::string{"Rec.2100-PQ - Display"};
    evaluator.setAssetBaseDirectory(directory);
    const auto context = legacyPreparedMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    const auto plan = mediaPlan(format(width, height), asset,
                                LayerValues{.position = {width * 0.5, height * 0.5}}, 9100);
    expectations.expect(expectMediaParity(expectations, *executor.executor, evaluator, builder,
                                          plan, acesRequest(*plan, kAcesWorking), width, height,
                                          "pq sweep"),
                        "pq sweep: Rec.2100 PQ input-to-working parity across the code range");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}
