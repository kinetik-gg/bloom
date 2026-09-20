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
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
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
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
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
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
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
