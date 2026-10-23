// Budget/capacity group: the media budget refusal, the large-source-under-default-allowance proof,
// and the capacity-aware producer policy. Included by gpu_media_scene_preparation_tests.cpp inside
// its anonymous namespace.

void testBudgetRefusal(Expectations& expectations, const MediaFixture& fixture) {
    auto context = GpuSceneMediaContext::fromEvaluator(CpuCompositionEvaluator{});
    context.assetBaseDirectory = fixture.directory;
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan =
        mediaPlan(format(256, 256), fixture.asset, LayerValues{.position = {128.0, 128.0}}, 2000);
    // GPU preparation charges the real retained source bytes, not the CPU evaluator's hypothetical
    // full-frame working set (gpu-backend.md): 64 KiB holds this 3x2 source, so it prepares.
    auto request = requestFor(*plan);
    request.pixelStorageByteLimit = 1U << 16U;
    expectations.expect(builder.build(plan, request).hasValue(),
                        "a budget above the real source bytes prepares a 256x256 media scene");
    // A budget below the source's own capture/backing bytes must still fail closed.
    request.pixelStorageByteLimit = 64U;
    const auto prepared = builder.build(plan, request);
    expectations.expect(!prepared.hasValue(), "a media scene under a tiny budget fails closed");
    expectations.expect(
        !prepared.hasValue() &&
            prepared.diagnostic.code ==
                bloom::runtime::PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded,
        "the media budget refusal is diagnosed as a pixel-storage budget");
}

[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
sharedSourcePlan(const std::vector<document::AssetRecord>& assets, const std::uint64_t idBase) {
    std::vector<CompiledOperation> operations;
    std::vector<CompiledMergeInput> entries;
    for (std::size_t index = 0; index < assets.size(); ++index) {
        const auto id = idBase + static_cast<std::uint64_t>(index) * 20;
        const auto sourceIndex = OperationIndex::fromRaw(operations.size());
        operations.emplace_back(bloom::runtime::CompiledImageSource{
            document::NodeId::fromRaw(id + 10), assets[index], 0, 0, 0, {}, false});
        const auto layerIndex = OperationIndex::fromRaw(operations.size());
        const auto layerId = document::LayerId::fromRaw(id + 12);
        operations.emplace_back(layerOutput(
            document::NodeId::fromRaw(id + 11), layerId, sourceIndex,
            LayerIds{document::ParameterId::fromRaw(id), document::ParameterId::fromRaw(id + 1),
                     document::ParameterId::fromRaw(id + 2), document::ParameterId::fromRaw(id + 3),
                     document::ParameterId::fromRaw(id + 4),
                     document::ParameterId::fromRaw(id + 5)},
            LayerValues{.position = {4.0, 4.0}}));
        entries.emplace_back(document::LayerSlotId::fromRaw(id + 14), layerId, layerIndex);
    }
    const auto mergeIndex = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledMerge{document::NodeId::fromRaw(idBase + 1000), entries});
    const auto outputIndex = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(
        CompiledCompositionOutput{document::NodeId::fromRaw(idBase + 1001), mergeIndex});
    CompiledCompositionPlanDefinition definition{
        document::Revision::fromRaw(7), kProjectId, kCompositionId, format(8, 8),
        std::move(operations),          outputIndex};
    definition.duration = RationalTime::fromInteger(100);
    return publish(std::move(definition));
}

void testSharedSourceBackingCharge(Expectations& expectations,
                                   const CpuCompositionEvaluator& evaluator,
                                   const MediaFixture& fixture) {
    constexpr std::uint64_t oneSourceBytes = 16ULL * 16 * sizeof(Rgba32f);
    const auto firstPath = fixture.directory / "shared_backing_a.exr";
    const auto secondPath = fixture.directory / "shared_backing_b.exr";
    auto pixels = std::vector<ExrPixel>(256, signedHdrPixels().front());
    writeExrRgba(firstPath, 16, 16, pixels);
    pixels.front().red += 0.5F;
    writeExrRgba(secondPath, 16, 16, pixels);
    const auto firstAsset = imageAsset(firstPath, "shared-a", 53000);
    const auto secondAsset = imageAsset(secondPath, "shared-b", 53001);
    const CpuGpuSceneBuilder builder(nullptr, legacyPreparedMediaContext(evaluator));
    // Derive the immutable source once with enough room for EXR bytes, decoder staging and the
    // output. The narrow request below tests retained backing accounting, not decode workspace.
    const auto warmPlan = sharedSourcePlan({firstAsset}, 53500);
    const auto warm = builder.build(warmPlan, requestFor(*warmPlan));
    expectations.expect(warm.hasValue(), "source backing warms within the ordinary work budget");
    if (!warm) {
        return;
    }
    const auto aliases = sharedSourcePlan({firstAsset, firstAsset, firstAsset}, 54000);
    auto request = requestFor(*aliases);
    request.pixelStorageByteLimit = oneSourceBytes;
    request.mediaSourceRevisionPolicy = bloom::runtime::MediaSourceRevisionPolicy::Retained;
    const auto shared = builder.build(aliases, request);
    expectations.expect(shared.hasValue(), "three source aliases fit one backing charge");
    if (shared) {
        const Rgba32fImage* backing = nullptr;
        std::size_t uploads = 0;
        bool sameBacking = true;
        for (const auto& command : shared.scene->commands()) {
            if (const auto* upload = std::get_if<GpuSceneUploadCommand>(&command)) {
                ++uploads;
                if (backing != nullptr && backing != upload->image.get())
                    sameBacking = false;
                backing = upload->image.get();
            }
        }
        expectations.expect(uploads == 3 && sameBacking,
                            "all three aliases retain the same immutable pixels");
    }

    const auto distinct = sharedSourcePlan({firstAsset, secondAsset}, 55000);
    request = requestFor(*distinct);
    request.pixelStorageByteLimit = oneSourceBytes;
    request.mediaSourceRevisionPolicy = bloom::runtime::MediaSourceRevisionPolicy::Retained;
    const auto refused = builder.build(distinct, request);
    expectations.expect(
        !refused && refused.diagnostic.code ==
                        bloom::runtime::PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded,
        "two distinct backings need two source charges");

    auto childDefinition = sharedSourcePlan({firstAsset}, 56000)->copyDefinition();
    childDefinition.compositionId = document::CompositionId::fromRaw(3);
    const auto child = publish(std::move(childDefinition));
    auto parentDefinition = sharedSourcePlan({firstAsset}, 57000)->copyDefinition();
    auto parentOperations = std::vector<CompiledOperation>{parentDefinition.operations[0],
                                                           parentDefinition.operations[1]};
    parentOperations.emplace_back(bloom::runtime::CompiledCompositionSource{
        document::NodeId::fromRaw(58000), 0,
        bloom::runtime::CompiledCompositionTimeMapping{{document::ParameterId::fromRaw(58001), 0.0},
                                                       {document::ParameterId::fromRaw(58002), 1.0},
                                                       0}});
    parentOperations.emplace_back(layerOutput(
        document::NodeId::fromRaw(58003), document::LayerId::fromRaw(58004),
        OperationIndex::fromRaw(2),
        LayerIds{document::ParameterId::fromRaw(58010), document::ParameterId::fromRaw(58011),
                 document::ParameterId::fromRaw(58012), document::ParameterId::fromRaw(58013),
                 document::ParameterId::fromRaw(58014), document::ParameterId::fromRaw(58015)},
        LayerValues{.position = {4.0, 4.0}}));
    parentOperations.emplace_back(
        CompiledMerge{document::NodeId::fromRaw(58005),
                      {{document::LayerSlotId::fromRaw(58006), document::LayerId::fromRaw(57012),
                        OperationIndex::fromRaw(1)},
                       {document::LayerSlotId::fromRaw(58007), document::LayerId::fromRaw(58004),
                        OperationIndex::fromRaw(3)}}});
    parentOperations.emplace_back(
        CompiledCompositionOutput{document::NodeId::fromRaw(58008), OperationIndex::fromRaw(4)});
    parentDefinition.operations = std::move(parentOperations);
    parentDefinition.output = OperationIndex::fromRaw(5);
    parentDefinition.nestedPlans.push_back(child);
    const auto parent = publish(std::move(parentDefinition));
    request = requestFor(*parent);
    request.pixelStorageByteLimit = oneSourceBytes;
    request.mediaSourceRevisionPolicy = bloom::runtime::MediaSourceRevisionPolicy::Retained;
    const auto nested = builder.build(parent, request);
    expectations.expect(nested.hasValue(),
                        "parent and nested child share one source backing charge");
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
