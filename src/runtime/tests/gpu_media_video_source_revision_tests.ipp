// Included in the owning video preparation fixture's anonymous namespace.
void testVideoSourceRevisionBoundary(Expectations& expectations,
                                     const std::filesystem::path& fixtures) {
    const auto path = fixtures / "revision-runtime-scratch.mp4";
    struct Remove final {
        std::filesystem::path path;
        ~Remove() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } remove{path};
    std::filesystem::copy_file(fixtures / "numbered-h264.mp4", path,
                               std::filesystem::copy_options::overwrite_existing);
    const auto asset = videoAsset(path, 31000);
    const auto plan =
        videoPlan(format(96, 64), asset, LayerValues{.position = {48.3, 32.1}}, 31000);
    const CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(fixtures);
    evaluator.setVideoCacheByteBudget(1U << 20U);
    auto context = legacyPreparedMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    auto request = requestFor(*plan);
    request.mediaSourceRevisionPolicy = bloom::runtime::MediaSourceRevisionPolicy::Retained;
    const auto cold = builder.build(plan, request);
    expectations.expect(cold && cold.scene->mediaStatistics().sourceRevisionCaptures == 1 &&
                            cold.scene->mediaStatistics().sourceRevisionBytesHashed ==
                                std::filesystem::file_size(path),
                        "cold video selection records a bounded exact-byte source capture");
    const auto stamp = std::filesystem::last_write_time(path);
    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        char first = 0;
        file.read(&first, 1);
        first ^= 1;
        file.seekp(0);
        file.write(&first, 1);
    }
    std::filesystem::last_write_time(path, stamp);
    const auto warm = builder.build(plan, request);
    expectations.expect(warm && warm.scene->mediaStatistics().sourceRevisionCacheHits == 1 &&
                            warm.scene->mediaStatistics().sourceBytesRead == 0 &&
                            warm.scene->mediaStatistics().sourceRevisionBytesHashed == 0 &&
                            warm.scene->mediaStatistics().sourceIoNanoseconds == 0 &&
                            warm.scene->mediaStatistics().sourceHashNanoseconds == 0 &&
                            warm.scene->mediaStatistics().uploadCacheHits == 1,
                        "warm video edit uses owned bytes with zero source I/O and hash work");
    bloom::runtime::GpuProcessFrameEvaluatorOptions finalOptions;
    finalOptions.mediaContext = context;
    const auto changed =
        bloom::runtime::detail::prepareExportScene(finalOptions, plan, request, {}, false);
    expectations.expect(!changed.scene &&
                            changed.diagnosticMessage.find("Video changed") != std::string::npos,
                        "final preparation forces revalidation even with explicit Retained and "
                        "same-mtime replacement");
    auto finalCpuRequest = request;
    finalCpuRequest.requireVerifiedMediaSources = true;
    const auto cpuChanged = evaluator.evaluate(plan, finalCpuRequest, {});
    expectations.expect(
        cpuChanged.status() == bloom::runtime::EvaluationStatus::Failed && !cpuChanged.frame(),
        "CPU final boundary rejects same-mtime source change despite explicit Retained request");
    std::filesystem::remove(path);
    const auto missing =
        bloom::runtime::detail::prepareExportScene(finalOptions, plan, request, {}, false);
    expectations.expect(
        !missing.scene &&
            missing.diagnosticMessage.find("Video source capture") != std::string::npos,
        "final preparation rejects a missing original despite decoded/prepared hits");
    const auto cpuMissing = evaluator.evaluate(plan, finalCpuRequest, {});
    expectations.expect(
        cpuMissing.status() == bloom::runtime::EvaluationStatus::Failed && !cpuMissing.frame(),
        "CPU final boundary rejects missing video instead of accepting preview placeholder");
    const auto decodedBeforeReopen = evaluator.videoContext()->cache.residentBytes();
    evaluator.operationCache()->invalidateSourceRevisions();
    expectations.expect(evaluator.videoContext()->cache.residentBytes() == decodedBeforeReopen,
                        "New/Open source invalidation preserves decoded pixels without granting "
                        "retained source authority");
    evaluator.clearDecodedVideoCache();
    expectations.expect(!builder.build(plan, request),
                        "media purge clears video source authority as well as decoded frames");
    std::filesystem::copy_file(fixtures / "numbered-h264.mp4", path);
    const auto reconnect = builder.build(plan, request);
    expectations.expect(reconnect && reconnect.scene->mediaStatistics().sourceRevisionCaptures == 1,
                        "reconnect after refresh captures original bytes before reuse");
    auto relink = asset;
    relink.locator.path = "absent-relinked-video.mp4";
    relink.locator.relinkHint = "file:///absent-relinked-video.mp4";
    const auto relinkPlan = videoPlan(format(96, 64), relink, LayerValues{}, 32000);
    auto relinkRequest = requestFor(*relinkPlan);
    relinkRequest.mediaSourceRevisionPolicy = bloom::runtime::MediaSourceRevisionPolicy::Retained;
    expectations.expect(!builder.build(relinkPlan, relinkRequest),
                        "a changed video locator cannot adopt old authority");
    auto changedDigest = asset;
    const auto replacementDigest =
        bloom::core::Sha256Hasher::hash(std::as_bytes(std::span("different", std::size_t{9})));
    if (!replacementDigest.has_value()) {
        expectations.expect(false, "changed video digest fixture hashes successfully");
        return;
    }
    changedDigest.contentDigest = *replacementDigest;
    const auto changedDigestPlan = videoPlan(format(96, 64), changedDigest, LayerValues{}, 33000);
    auto changedDigestRequest = requestFor(*changedDigestPlan);
    changedDigestRequest.mediaSourceRevisionPolicy =
        bloom::runtime::MediaSourceRevisionPolicy::Retained;
    expectations.expect(!builder.build(changedDigestPlan, changedDigestRequest),
                        "a changed video digest cannot reuse old source authority");

    const CpuCompositionEvaluator gestureEvaluator;
    gestureEvaluator.setAssetBaseDirectory(fixtures);
    gestureEvaluator.setVideoCacheByteBudget(1U << 20U);
    auto gestureDefinition = plan->copyDefinition();
    gestureDefinition.bypassOperationCache = true;
    const auto gesturePlan = publish(std::move(gestureDefinition));
    auto gestureRequest = requestFor(*gesturePlan);
    gestureRequest.mediaSourceRevisionPolicy = bloom::runtime::MediaSourceRevisionPolicy::Retained;
    const CpuGpuSceneBuilder gestureBuilder(nullptr,
                                            GpuSceneMediaContext::fromEvaluator(gestureEvaluator));
    const auto gesture = gestureBuilder.build(gesturePlan, gestureRequest);
    expectations.expect(
        gesture && gestureEvaluator.videoContext()->sourceSnapshotBytes() == 0 &&
            gestureEvaluator.videoContext()->sourceSnapshotCount() == 0 &&
            gestureEvaluator.videoContext()->cache.residentBytes() == 0,
        "cold GPU gesture inserts neither encoded video snapshot nor decoded frame");
    const auto cpuGesture = gestureEvaluator.evaluate(gesturePlan, gestureRequest, {});
    expectations.expect(cpuGesture.frame() &&
                            gestureEvaluator.videoContext()->sourceSnapshotBytes() == 0 &&
                            gestureEvaluator.videoContext()->cache.residentBytes() == 0,
                        "cold CPU gesture follows the same read-only video cache contract");
}

void testRootedVideoSourceClosure(Expectations& expectations,
                                  const std::filesystem::path& fixtures) {
    const auto owner = bloom::render::PackedHostRetirementOwner::create();
    const CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(fixtures);
    const auto& pool = evaluator.operationCache();
    bloom::runtime::GpuPreparedUploadCache rooted(pool, owner);
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const auto asset = videoAsset(fixtures / "numbered-h264.mp4", 34000);
    const auto plan = videoPlan(format(96, 64), asset, LayerValues{}, 34000);
    auto request = requestFor(*plan);
    request.mediaSourceRevisionPolicy = bloom::runtime::MediaSourceRevisionPolicy::Retained;
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared && context.videoContext->sourceSnapshotCount() == 1,
                        "rooted video producer retains one owned source snapshot");
    pool->closePreparedUploads();
    owner->beginShutdown();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!owner->exitAcknowledged() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    expectations.expect(owner->exitAcknowledged() &&
                            context.videoContext->sourceSnapshotCount() == 0 &&
                            context.videoContext->sourceSnapshotBytes() == 0,
                        "producer close queues session and snapshot retirement before host exit "
                        "while evaluator remains alive");
    expectations.expect(!builder.build(plan, request),
                        "closed video producer refuses new source work");
    // Keep every root alive through actual OS worker collection, including a failed expectation.
    context.videoContext->clear();
    while (!owner->exitAcknowledged())
        std::this_thread::yield();
    expectations.expect(owner->collect(), "video source retirement OS tail collected explicitly");
}
