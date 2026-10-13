// Cache-identity group: warm upload reuse, changed frame/bypass, the source-key identity contract,
// per-request statistics, and the gesture cache never touching disk.
// Included by gpu_media_scene_preparation_tests.cpp inside its anonymous namespace.

void testWarmReuseAndChangedSource(Expectations& expectations,
                                   const CpuCompositionEvaluator& evaluator,
                                   const MediaFixture& fixture) {
    auto context = legacyPreparedMediaContext(evaluator);
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

    auto context = legacyPreparedMediaContext(evaluator);
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
    auto context = legacyPreparedMediaContext(evaluator);
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
    auto context = legacyPreparedMediaContext(evaluator);
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
    auto context = legacyPreparedMediaContext(evaluator);
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

void testImmutableSourceRevisionReuse(Expectations& expectations, const MediaFixture& fixture) {
    using bloom::runtime::MediaSourceRevisionPolicy;
    using bloom::runtime::OperationCacheEntryKind;
    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(fixture.directory);
    auto context = legacyPreparedMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto planA =
        mediaPlan(format(8, 8), fixture.asset, LayerValues{.position = {4.3, 3.1}}, 3900);
    const auto planB =
        mediaPlan(format(8, 8), fixture.asset, LayerValues{.position = {2.7, 2.4}}, 4000);
    auto requestA = requestFor(*planA);
    auto requestB = requestFor(*planB);
    requestA.mediaSourceRevisionPolicy = MediaSourceRevisionPolicy::Retained;
    requestB.mediaSourceRevisionPolicy = MediaSourceRevisionPolicy::Retained;
    const auto cold = builder.build(planA, requestA);
    expectations.expect(cold && cold.scene->mediaStatistics().sourceRevisionCaptures == 1 &&
                            cold.scene->mediaStatistics().sourceRevisionBytesHashed > 0 &&
                            cold.scene->mediaStatistics().sourceBytesRead > 0 &&
                            cold.scene->mediaStatistics().sourceIoNanoseconds > 0 &&
                            cold.scene->mediaStatistics().sourceHashNanoseconds > 0 &&
                            cold.scene->mediaStatistics().sourceProbeNanoseconds > 0 &&
                            cold.scene->mediaStatistics().imageDecodeNanoseconds > 0,
                        "cold interactive source captures and hashes one immutable revision");
    const auto charged =
        evaluator.operationCache()->retainedBytes(OperationCacheEntryKind::DecodedMedia);
    expectations.expect(charged >= std::filesystem::file_size(fixture.path),
                        "same source pool charges encoded source bytes and metadata");
    // Remove the path while a captured revision is retained. Warm source selection must have no
    // filesystem dependency: the immutable captured bytes, rather than metadata, are authority.
    const auto heldPath = fixture.directory / "held_revision.exr";
    std::filesystem::rename(fixture.path, heldPath);
    const auto warm = builder.build(planB, requestB);
    expectations.expect(warm && warm.scene->mediaStatistics().sourceRevisionCacheHits == 1 &&
                            warm.scene->mediaStatistics().sourceRevisionCaptures == 0 &&
                            warm.scene->mediaStatistics().sourceRevisionBytesHashed == 0 &&
                            warm.scene->mediaStatistics().sourceBytesRead == 0 &&
                            warm.scene->mediaStatistics().sourceIoNanoseconds == 0 &&
                            warm.scene->mediaStatistics().sourceHashNanoseconds == 0 &&
                            warm.scene->mediaStatistics().sourceProbeNanoseconds == 0 &&
                            warm.scene->mediaStatistics().imageDecodeNanoseconds == 0 &&
                            warm.scene->mediaStatistics().imageConversions == 0,
                        "warm transform edit reuses actual revision with zero probe/hash/decode");
    auto finalCpuRequest = requestB;
    finalCpuRequest.requireVerifiedMediaSources = true;
    const auto cpuFinal = evaluator.evaluate(planB, finalCpuRequest, {});
    expectations.expect(
        cpuFinal.status() == bloom::runtime::EvaluationStatus::Failed && !cpuFinal.frame(),
        "CPU final boundary rejects missing still despite explicit Retained request");
    auto finalRequest = requestB;
    finalRequest.mediaSourceRevisionPolicy = MediaSourceRevisionPolicy::Revalidate;
    expectations.expect(
        !builder.build(planB, finalRequest),
        "default final revalidation refuses a disappeared source despite warm upload");
    bloom::runtime::GpuProcessFrameEvaluatorOptions finalOptions;
    finalOptions.mediaContext = context;
    const auto forcedFinal =
        bloom::runtime::detail::prepareExportScene(finalOptions, planB, requestB, {}, false);
    expectations.expect(
        !forcedFinal.scene,
        "production final preparation overrides an explicitly Retained preview request");
    auto bypass = requestB;
    bypass.bypassOperationCache = true;
    expectations.expect(!builder.build(planB, bypass),
                        "explicit bypass revalidates instead of trusting retained revision");
    auto relinked = fixture.asset;
    relinked.locator.path = "absent_relink.exr";
    relinked.locator.relinkHint = "file:///absent_relink.exr";
    const auto relinkPlan = mediaPlan(format(8, 8), relinked, LayerValues{}, 4100);
    auto relinkRequest = requestFor(*relinkPlan);
    relinkRequest.mediaSourceRevisionPolicy = MediaSourceRevisionPolicy::Retained;
    expectations.expect(!builder.build(relinkPlan, relinkRequest),
                        "changed locator cannot adopt an unrelated retained ticket");
    evaluator.operationCache()->invalidateSourceRevisions();
    expectations.expect(!builder.build(planB, requestB),
                        "New/Open invalidates source authority even while prepared pixels remain");
    evaluator.operationCache()->clearDecodedMedia();
    expectations.expect(
        !builder.build(planB, requestB),
        "Refresh/purge drops tickets and rechecks missing bytes before upload hits");
    std::filesystem::rename(heldPath, fixture.path);
    const auto reconnected = builder.build(planB, requestB);
    expectations.expect(reconnected &&
                            reconnected.scene->mediaStatistics().sourceRevisionCaptures == 1,
                        "reconnect after refresh captures fresh bytes");
    const auto captured =
        bloom::media::captureImageSourceRevision(fixture.path, bloom::media::kMaxImageStorageBytes);
    const auto generation = evaluator.operationCache()->decodedMediaGeneration();
    evaluator.operationCache()->clearDecodedMedia();
    if (captured.value.has_value()) {
        evaluator.operationCache()->store(
            "late-before-refresh-source", bloom::document::Revision{},
            {.image = {}, .values = {}, .bounds = {}, .sourceRevision = *captured.value},
            OperationCacheEntryKind::DecodedMedia, generation);
        expectations.expect(!evaluator.operationCache()->find("late-before-refresh-source",
                                                              bloom::document::Revision{}),
                            "capture started before refresh cannot repopulate revision authority");
        const auto stillValid = bloom::media::decodeImage(**captured.value);
        expectations.expect(stillValid.value.has_value(),
                            "refresh preserves already-held immutable snapshot lifetime");
    }
    evaluator.operationCache()->setByteBudget(0);
    expectations.expect(evaluator.operationCache()->retainedBytes() == 0,
                        "live source retention budget reduction removes encoded revision charges");
    const auto unretained = builder.build(planB, requestB);
    expectations.expect(unretained &&
                            unretained.scene->mediaStatistics().sourceRevisionCaptures == 1 &&
                            unretained.scene->mediaStatistics().sourceRevisionCacheHits == 0 &&
                            evaluator.operationCache()->retainedBytes() == 0,
                        "zero retention keeps fresh preparation valid without bypassing live cap");
    if (cold && warm) {
        const auto* uploadCold = firstUpload(*cold.scene);
        const auto* uploadWarm = firstUpload(*warm.scene);
        expectations.expect(
            uploadCold && uploadWarm && uploadCold->image == uploadWarm->image,
            "already published immutable upload remains valid after ticket eviction");
    }
}

void testGestureSourceRevisionIsReadOnly(Expectations& checks, const MediaFixture& fixture) {
    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(fixture.directory);
    auto context = legacyPreparedMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto ordinary =
        mediaPlan(format(8, 8), fixture.asset, LayerValues{.position = {3.0, 3.0}}, 4200);
    auto definition = ordinary->copyDefinition();
    definition.bypassOperationCache = true;
    const auto gesture = publish(std::move(definition));
    auto request = requestFor(*gesture);
    request.mediaSourceRevisionPolicy = bloom::runtime::MediaSourceRevisionPolicy::Retained;
    const auto prepared = builder.build(gesture, request);
    checks.expect(prepared.hasValue() &&
                      prepared.scene->mediaStatistics().sourceRevisionCaptures == 1 &&
                      context.operationCache->retainedBytes() == 0 &&
                      context.preparedUploadCache->entryCount() == 0,
                  "cold GPU gesture captures transient source without inserting ticket or upload");
    const auto cpu = evaluator.evaluate(gesture, request, {});
    checks.expect(cpu.frame() != nullptr && context.operationCache->retainedBytes() == 0,
                  "cold CPU gesture inserts neither source revision nor decoded/derived pixels");
    auto ordinaryRequest = requestFor(*ordinary);
    ordinaryRequest.mediaSourceRevisionPolicy = bloom::runtime::MediaSourceRevisionPolicy::Retained;
    const auto warmed = builder.build(ordinary, ordinaryRequest);
    const auto bytes = context.operationCache->retainedBytes();
    const auto reused = builder.build(gesture, request);
    checks.expect(warmed && bytes > 0 && reused &&
                      reused.scene->mediaStatistics().sourceRevisionCacheHits == 1 &&
                      reused.scene->mediaStatistics().sourceRevisionCaptures == 0 &&
                      context.operationCache->retainedBytes() == bytes,
                  "warm GPU gesture reads the existing immutable ticket without inserting");
    const auto cpuReused = evaluator.evaluate(gesture, request, {});
    checks.expect(cpuReused.frame() != nullptr && context.operationCache->retainedBytes() == bytes,
                  "warm CPU gesture remains a read-only source-cache consumer");
}
