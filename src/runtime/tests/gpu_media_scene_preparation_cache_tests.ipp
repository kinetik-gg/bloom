// Cache-identity group: warm upload reuse, changed frame/bypass, the source-key identity contract,
// per-request statistics, and the gesture cache never touching disk.
// Included by gpu_media_scene_preparation_tests.cpp inside its anonymous namespace.

void testWarmReuseAndChangedSource(Expectations& expectations,
                                   const CpuCompositionEvaluator& evaluator,
                                   const MediaFixture& fixture) {
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto planA = mediaPlan(format(8, 8), fixture.asset,
                                 LayerValues{.position = {4.3, 3.1}, .opacity = 1.0}, 1500);
    const auto planB = mediaPlan(format(8, 8), fixture.asset,
                                 LayerValues{.position = {2.7, 2.4}, .opacity = 1.0}, 1600);
    const auto a = builder.build(planA, requestFor(*planA));
    const auto b = builder.build(planB, requestFor(*planB));
    expectations.expect(a.hasValue() && b.hasValue(), "both media builds prepare");
    if (!a || !b) {
        return;
    }
    const auto* uploadA = firstUpload(*a.scene);
    const auto* uploadB = firstUpload(*b.scene);
    const auto* translationA = firstTranslation(*a.scene);
    const auto* translationB = firstTranslation(*b.scene);
    expectations.expect(uploadA != nullptr && uploadB != nullptr && translationA != nullptr &&
                            translationB != nullptr,
                        "both builds emit an upload and a translation command");
    if (uploadA == nullptr || uploadB == nullptr || translationA == nullptr ||
        translationB == nullptr) {
        return;
    }
    expectations.expect(uploadA->semanticKey == uploadB->semanticKey,
                        "an unchanged source keeps one upload semantic key across transforms");
    expectations.expect(uploadA->image.get() == uploadB->image.get(),
                        "an unchanged source reuses the SAME immutable allocation");
    expectations.expect(translationA->semanticKey != translationB->semanticKey,
                        "a changed transform has a different translation pixel key");
    expectations.expect(b.scene->mediaStatistics().uploadCacheHits == 1 &&
                            b.scene->mediaStatistics().imageConversions == 0,
                        "the warm build serves the source from the prepared-upload cache");
}

void testChangedFrameAndBypass(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                               const MediaFixture& fixture) {
    auto secondPixels = signedHdrPixels();
    secondPixels.front().green += 1.0F;
    const auto secondPath = fixture.directory / "bypass_second.exr";
    writeExrRgba(secondPath, 3, 2, secondPixels);
    document::AssetRecord asset = fixture.asset;
    asset.id = bloom::document::AssetId::fromRaw(902);
    asset.kind = document::AssetKind::Sequence;
    asset.manifest.pattern = "bypass_sequence.####.exr";
    asset.manifest.padding = 4;
    asset.manifest.first = 0;
    asset.manifest.last = 1;
    asset.manifest.members = {sequenceMember(fixture.path, 0, 0), sequenceMember(secondPath, 1, 1)};
    const auto plan = mediaPlan(format(8, 8), asset, LayerValues{.position = {4.3, 3.1}}, 1700);

    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto frameZero = builder.build(plan, requestFor(*plan, RationalTime::fromInteger(0)));
    const auto frameOne = builder.build(plan, requestFor(*plan, rationalTime(1, 24)));
    expectations.expect(frameZero.hasValue() && frameOne.hasValue(),
                        "both sequence frames prepare");
    if (!frameZero || !frameOne) {
        return;
    }
    const auto* uploadZero = firstUpload(*frameZero.scene);
    const auto* uploadOne = firstUpload(*frameOne.scene);
    expectations.expect(uploadZero != nullptr && uploadOne != nullptr &&
                            uploadZero->semanticKey != uploadOne->semanticKey,
                        "a changed selected frame changes the upload semantic key");
    expectations.expect(uploadZero->image.get() != uploadOne->image.get(),
                        "a changed selected frame converts a distinct source image");

    // Explicit bypass recalculates the source instead of reading the prepared-upload cache.
    auto bypassRequest = requestFor(*plan, RationalTime::fromInteger(0));
    bypassRequest.bypassOperationCache = true;
    const auto bypassed = builder.build(plan, bypassRequest);
    expectations.expect(bypassed.hasValue(), "the bypass build prepares");
    if (!bypassed) {
        return;
    }
    const auto* uploadBypass = firstUpload(*bypassed.scene);
    expectations.expect(bypassed.scene->mediaStatistics().imageConversions == 1 &&
                            bypassed.scene->mediaStatistics().uploadCacheHits == 0,
                        "an explicit bypass reconverts and never reads the prepared-upload cache");
    expectations.expect(uploadBypass != nullptr && uploadZero != nullptr &&
                            uploadBypass->image.get() != uploadZero->image.get(),
                        "an explicit bypass recalculates a distinct source image");
}

// The source key is an identity of the resolved pixels, not of the plan: node/layer/parameter ids
// and the document revision must not enter it, while the proxy/composition descriptor must.
void testSourceKeyExcludesIdsAndRevision(Expectations& expectations,
                                         const CpuCompositionEvaluator& evaluator,
                                         const MediaFixture& fixture) {
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto a = mediaPlan(format(8, 8), fixture.asset,
                             LayerValues{.position = {4.3, 3.1}, .opacity = 0.8}, 2700);
    const auto b = mediaPlan(format(8, 8), fixture.asset,
                             LayerValues{.position = {4.3, 3.1}, .opacity = 0.8}, 2800);
    auto definition = a->copyDefinition();
    definition.sourceRevision = bloom::document::Revision::fromRaw(999);
    const auto revised = publish(std::move(definition));

    const auto builtA = builder.build(a, requestFor(*a));
    const auto builtB = builder.build(b, requestFor(*b));
    const auto builtRevised = builder.build(revised, requestFor(*revised));
    expectations.expect(builtA.hasValue() && builtB.hasValue() && builtRevised.hasValue(),
                        "the identity-key builds prepare");
    if (!builtA || !builtB || !builtRevised) {
        return;
    }
    const auto* uploadA = firstUpload(*builtA.scene);
    const auto* uploadB = firstUpload(*builtB.scene);
    const auto* uploadRevised = firstUpload(*builtRevised.scene);
    const auto* translationA = firstTranslation(*builtA.scene);
    const auto* translationB = firstTranslation(*builtB.scene);
    const auto* translationRevised = firstTranslation(*builtRevised.scene);
    expectations.expect(uploadA != nullptr && uploadB != nullptr && uploadRevised != nullptr &&
                            translationA != nullptr && translationB != nullptr &&
                            translationRevised != nullptr,
                        "the identity-key builds emit uploads and translations");
    if (uploadA == nullptr || uploadB == nullptr || uploadRevised == nullptr ||
        translationA == nullptr || translationB == nullptr || translationRevised == nullptr) {
        return;
    }
    expectations.expect(uploadA->semanticKey == uploadB->semanticKey &&
                            uploadA->semanticKey == uploadRevised->semanticKey,
                        "node/layer/parameter ids and the revision never enter the source key");
    expectations.expect(translationA->semanticKey == translationB->semanticKey &&
                            translationA->semanticKey == translationRevised->semanticKey,
                        "node/layer/parameter ids and the revision never enter a derived key");

    // The proxy scale IS part of the converted source identity: a different proxy is a miss.
    const auto proxyPlan = mediaPlan(format(8, 8), fixture.asset,
                                     LayerValues{.position = {4.3, 3.1}, .opacity = 0.8}, 2900);
    const auto extent = bloom::render::ImageExtent::create(5, 4);
    expectations.expect(static_cast<bool>(extent), "the proxy extent builds");
    if (!extent) {
        return;
    }
    auto proxyRequest = requestFor(*proxyPlan);
    proxyRequest.resolution = ProxyResolution{*extent.value()};
    const auto proxied = builder.build(proxyPlan, proxyRequest);
    expectations.expect(proxied.hasValue(), "the proxy build prepares");
    if (!proxied) {
        return;
    }
    const auto* uploadProxy = firstUpload(*proxied.scene);
    expectations.expect(uploadProxy != nullptr &&
                            uploadProxy->semanticKey != uploadA->semanticKey &&
                            proxied.scene->mediaStatistics().uploadCacheMisses == 1,
                        "a changed proxy scale changes the source key and is a miss");
}

// Per-build CPU work is reported on the result, never in a shared mutable aggregate.
void testPerRequestStatisticsAreLocal(Expectations& expectations,
                                      const CpuCompositionEvaluator& evaluator,
                                      const MediaFixture& fixture) {
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan = mediaPlan(format(8, 8), fixture.asset,
                                LayerValues{.position = {4.3, 3.1}, .opacity = 0.9}, 3000);
    const auto first = builder.build(plan, requestFor(*plan));
    const auto second = builder.build(plan, requestFor(*plan));
    expectations.expect(first.hasValue() && second.hasValue(), "both per-request builds prepare");
    if (!first || !second) {
        return;
    }
    expectations.expect(first.scene->mediaStatistics().uploadCacheMisses == 1 &&
                            first.scene->mediaStatistics().uploadCacheHits == 0 &&
                            first.scene->mediaStatistics().imageConversions == 1,
                        "the first result reports only its own cold work");
    expectations.expect(second.scene->mediaStatistics().uploadCacheHits == 1 &&
                            second.scene->mediaStatistics().uploadCacheMisses == 0 &&
                            second.scene->mediaStatistics().imageConversions == 0,
                        "the second result reports only its own warm work");
}

// Explicit bypass and the interactive (overridden) plan bypass must not touch the real disk cache.
void testGestureCacheNeverTouchesDisk(Expectations& expectations,
                                      const CpuCompositionEvaluator& evaluator,
                                      const MediaFixture& fixture) {
    const auto diskRoot = fixture.directory / "gesture_disk_cache";
    auto disk = std::make_shared<bloom::media::cache::MediaDiskCache>(
        bloom::media::cache::MediaDiskCacheConfig{.rootDirectory = diskRoot,
                                                  .byteBudget = std::size_t{1} << 24U});
    evaluator.setMediaDiskCache(disk);
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan = mediaPlan(format(8, 8), fixture.asset,
                                LayerValues{.position = {4.3, 3.1}, .opacity = 0.9}, 3100);
    const auto warm = builder.build(plan, requestFor(*plan));
    expectations.expect(warm.hasValue(), "the disk-cache warm build prepares");
    disk->flush();
    const auto before = disk->statistics();

    auto interactiveDefinition = plan->copyDefinition();
    interactiveDefinition.bypassOperationCache = true;
    const auto interactive = publish(std::move(interactiveDefinition));
    const auto gestureHit = builder.build(interactive, requestFor(*interactive));
    disk->flush();
    const auto afterHit = disk->statistics();
    expectations.expect(gestureHit.hasValue() &&
                            gestureHit.scene->mediaStatistics().uploadCacheHits == 1 &&
                            gestureHit.scene->mediaStatistics().imageConversions == 0,
                        "a gesture hit serves the prepared-upload cache");
    expectations.expect(afterHit.hits == before.hits && afterHit.misses == before.misses &&
                            afterHit.entryCount == before.entryCount &&
                            afterHit.storedBytes == before.storedBytes,
                        "a gesture hit never reads or writes the disk cache");

    auto secondPixels = signedHdrPixels();
    secondPixels.front().red += 0.75F;
    const auto secondPath = fixture.directory / "gesture_second.exr";
    writeExrRgba(secondPath, 3, 2, secondPixels);
    const auto secondAsset = imageAsset(secondPath, "gesture_second", 904);
    const auto missPlan =
        mediaPlan(format(8, 8), secondAsset, LayerValues{.position = {4.3, 3.1}}, 3200);
    auto missDefinition = missPlan->copyDefinition();
    missDefinition.bypassOperationCache = true;
    const auto missInteractive = publish(std::move(missDefinition));
    const auto beforeMiss = disk->statistics();
    const auto gestureMiss = builder.build(missInteractive, requestFor(*missInteractive));
    disk->flush();
    const auto afterMiss = disk->statistics();
    expectations.expect(gestureMiss.hasValue() &&
                            gestureMiss.scene->mediaStatistics().uploadCacheMisses == 1 &&
                            gestureMiss.scene->mediaStatistics().uploadCacheHits == 0 &&
                            gestureMiss.scene->mediaStatistics().imageConversions == 1,
                        "a gesture miss converts directly and is not inserted");
    expectations.expect(afterMiss.hits == beforeMiss.hits &&
                            afterMiss.misses == beforeMiss.misses &&
                            afterMiss.entryCount == beforeMiss.entryCount &&
                            afterMiss.storedBytes == beforeMiss.storedBytes,
                        "a gesture miss never reads or writes the disk cache");

    auto explicitRequest = requestFor(*plan);
    explicitRequest.bypassOperationCache = true;
    const auto beforeExplicit = disk->statistics();
    const auto explicitBuild = builder.build(plan, explicitRequest);
    disk->flush();
    const auto afterExplicit = disk->statistics();
    expectations.expect(explicitBuild.hasValue() &&
                            explicitBuild.scene->mediaStatistics().uploadCacheHits == 0 &&
                            explicitBuild.scene->mediaStatistics().imageConversions == 1,
                        "an explicit bypass reconverts and never reads the prepared-upload cache");
    expectations.expect(afterExplicit.hits == beforeExplicit.hits &&
                            afterExplicit.misses == beforeExplicit.misses &&
                            afterExplicit.entryCount == beforeExplicit.entryCount &&
                            afterExplicit.storedBytes == beforeExplicit.storedBytes,
                        "an explicit bypass never reads or writes the disk cache either");
}
