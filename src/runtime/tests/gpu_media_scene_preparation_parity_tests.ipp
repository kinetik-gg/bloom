// Image-source parity group: full parity against a genuine, uncached CpuCompositionEvaluator frame
// for translation, proxy/PAR, sequence, affine/blend, and changed-colour-interpretation scenes.
// Included by gpu_media_scene_preparation_tests.cpp inside its anonymous namespace.

void testStillImageParity(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                          const MediaFixture& fixture) {
    // Odd 3x2 signed/HDR source smaller than an 8x8 composition, fractional +0.3 translation.
    const auto fractional = mediaPlan(format(8, 8), fixture.asset,
                                      LayerValues{.position = {4.3, 3.1}, .opacity = 1.0}, 1000);
    checkMediaParity(expectations, evaluator, fractional, requestFor(*fractional),
                     "signed/HDR still image fractional");
    // Fractional -0.3 and a non-unit opacity.
    const auto fractionalNegative = mediaPlan(
        format(8, 8), fixture.asset, LayerValues{.position = {2.7, 2.4}, .opacity = 0.625}, 1100);
    checkMediaParity(expectations, evaluator, fractionalNegative, requestFor(*fractionalNegative),
                     "fractional -0.3 / opacity");
    // An integer device grid still uses the raster translation command for media (no vector chain).
    const auto integerGrid = mediaPlan(format(8, 8), fixture.asset,
                                       LayerValues{.position = {4.0, 3.0}, .opacity = 1.0}, 1200);
    const auto prepared = CpuGpuSceneBuilder{}.build(integerGrid, requestFor(*integerGrid));
    // The builder above has no media context, so it must fail closed rather than decode.
    expectations.expect(!prepared &&
                            prepared.diagnostic.code ==
                                bloom::runtime::PreparedGpuSceneDiagnosticCode::MediaUnavailable,
                        "a media scene without an evaluator context fails closed");
    checkMediaParity(expectations, evaluator, integerGrid, requestFor(*integerGrid),
                     "integer grid media still raster");
}

void testProxyNonSquarePar(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                           const MediaFixture& fixture) {
    const auto plan = mediaPlan(format(9, 6, pixelAspect(4, 3)), fixture.asset,
                                LayerValues{.position = {4.5, 3.0}}, 1300);
    const auto extent = bloom::render::ImageExtent::create(5, 4);
    expectations.expect(static_cast<bool>(extent), "the proxy extent builds");
    if (!extent) {
        return;
    }
    auto request = requestFor(*plan);
    request.resolution = ProxyResolution{*extent.value()};
    checkMediaParity(expectations, evaluator, plan, request, "media proxy / non-square PAR");
}

void testSequenceFrames(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                        const MediaFixture& fixture) {
    auto secondPixels = signedHdrPixels();
    for (auto& pixel : secondPixels) {
        pixel.red += 0.5F;
        pixel.blue *= 0.5F;
    }
    const auto secondPath = fixture.directory / "second.exr";
    writeExrRgba(secondPath, 3, 2, secondPixels);

    document::AssetRecord asset = fixture.asset;
    asset.id = bloom::document::AssetId::fromRaw(901);
    asset.kind = document::AssetKind::Sequence;
    asset.manifest.pattern = "sequence.####.exr";
    asset.manifest.padding = 4;
    asset.manifest.first = 0;
    asset.manifest.last = 1;
    asset.manifest.members = {sequenceMember(fixture.path, 0, 0), sequenceMember(secondPath, 1, 1)};

    const auto plan =
        mediaPlan(format(8, 8), asset, LayerValues{.position = {4.3, 3.1}, .opacity = 0.75}, 1400);
    checkMediaParity(expectations, evaluator, plan, requestFor(*plan, RationalTime::fromInteger(0)),
                     "sequence frame 0");
    checkMediaParity(expectations, evaluator, plan, requestFor(*plan, rationalTime(1, 24)),
                     "sequence frame 1");
}

void testAffineAndBlendMedia(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                             const MediaFixture& fixture) {
    // A non-Normal media layer now folds through an explicit BlendV1 with the resolved mode.
    {
        const auto plan = mediaPlan(
            format(8, 8), fixture.asset,
            LayerValues{.position = {4.3, 3.1}, .blendMode = bloom::core::BlendMode::Screen}, 1800);
        checkMediaParity(expectations, evaluator, plan, requestFor(*plan),
                         "non-Normal media blend");
    }
    // Rotation + nonuniform scale emit the accepted GpuAffine command and match the CPU raster.
    {
        const auto plan = mediaPlan(
            format(8, 8), fixture.asset,
            LayerValues{.position = {4.3, 3.1}, .scale = {1.5, 0.5}, .rotation = 30.0}, 1900);
        checkMediaParity(expectations, evaluator, plan, requestFor(*plan), "rotated media affine");
    }
    // Signed scale with a nonzero anchor, proxy resolution and a non-square PAR.
    {
        const auto plan = mediaPlan(format(9, 6, pixelAspect(4, 3)), fixture.asset,
                                    LayerValues{.position = {4.5, 3.0},
                                                .anchor = {1.0, -0.5},
                                                .scale = {-1.25, 0.75},
                                                .rotation = -20.0},
                                    1950);
        const auto extent = bloom::render::ImageExtent::create(6, 4);
        auto request = requestFor(*plan);
        request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
        checkMediaParity(expectations, evaluator, plan, request, "signed/anchor media proxy");
    }
}

// A changed input colour interpretation is a cache miss and must still match the CPU exactly.
void testChangedColourInterpretation(Expectations& expectations,
                                     const CpuCompositionEvaluator& evaluator,
                                     const MediaFixture& fixture) {
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto base = mediaPlan(format(8, 8), fixture.asset,
                                LayerValues{.position = {4.3, 3.1}, .opacity = 0.8}, 2100);
    const auto first = builder.build(base, requestFor(*base));
    expectations.expect(first.hasValue() && first.scene->mediaStatistics().imageConversions == 1 &&
                            first.scene->mediaStatistics().uploadCacheMisses == 1,
                        "the base build converts once and misses the upload cache");

    auto definition = base->copyDefinition();
    auto* source = std::get_if<bloom::runtime::CompiledImageSource>(&definition.operations[0]);
    if (source == nullptr || !source->asset) {
        expectations.expect(false, "the base plan has a media source");
        return;
    }
    source->asset->interpretation.colorSpace = bloom::document::AssetColorSpace::Raw;
    const auto reinterpreted = publish(std::move(definition));

    checkMediaParity(expectations, evaluator, reinterpreted, requestFor(*reinterpreted),
                     "changed input colour interpretation");
    const auto second = builder.build(reinterpreted, requestFor(*reinterpreted));
    expectations.expect(second.hasValue() &&
                            second.scene->mediaStatistics().imageConversions == 1 &&
                            second.scene->mediaStatistics().uploadCacheHits == 0 &&
                            second.scene->mediaStatistics().uploadCacheMisses == 1,
                        "a changed input colour interpretation is an upload cache miss");
    const auto* uploadFirst = firstUpload(*first.scene);
    const auto* uploadSecond = firstUpload(*second.scene);
    expectations.expect(uploadFirst != nullptr && uploadSecond != nullptr &&
                            uploadFirst->semanticKey != uploadSecond->semanticKey,
                        "a changed input colour interpretation changes the source key");
}
