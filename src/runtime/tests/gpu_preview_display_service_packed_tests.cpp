#include <atomic>
#include <cstdlib>
#include <new>

namespace packed_payload_fault {
thread_local unsigned failuresRemaining = 0;
std::atomic_size_t actualFailures{0};
} // namespace packed_payload_fault
void* operator new(const std::size_t bytes) {
    if (packed_payload_fault::failuresRemaining != 0) {
        --packed_payload_fault::failuresRemaining;
        ++packed_payload_fault::actualFailures;
        throw std::bad_alloc{};
    }
    void* const block = std::malloc(bytes == 0 ? 1 : bytes);
    if (!block)
        throw std::bad_alloc{};
    return block;
}
void operator delete(void* const block) noexcept { std::free(block); }
void operator delete(void* const block, std::size_t) noexcept { ::operator delete(block); }

#include "gpu_preview_display_service_packed_retirement.hpp"
#include "gpu_preview_display_service_packed_support.hpp"
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>

namespace {
#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE

GpuPreviewDisplayServiceOptions packedOptions(const TestOptions& options) {
    auto configured = serviceOptions(options.loaderPath);
    configured.product = bloom::runtime::GpuPreviewDisplayProductMode::GpuPreparedPacked;
    configured.presentation = GpuPreviewDisplayServicePresentationMode::Disabled;
    return configured;
}

std::shared_ptr<const bloom::runtime::PreparedPreviewFrame>
requestPacked(Expectations& checks, GpuPreviewDisplayService& service,
              const bloom::document::Snapshot& snapshot, const PreviewRequestIdentity& identity) {
    const bloom::runtime::TaskOwner owner{.kind = bloom::runtime::TaskOwnerKind::Composition,
                                          .id = bloom::runtime::TaskOwnerId::fromRaw(7)};
    auto submission =
        service.submit(TaskRequest("packed proof", owner), snapshot, identity, kBudget, {});
    checks.expect(submission.accepted(), "GPU packed request admitted");
    if (!submission.accepted()) {
        if (submission.diagnostic)
            std::cerr << submission.diagnostic->summary << '\n';
        return {};
    }
    const auto result = awaitResult(submission.handle);
    auto frame = packedResult(result);
    if (!frame) {
        std::cerr << "packed request refused: " << service.status().residentDetail << '\n';
        if (result) {
            for (const auto& diagnostic : result->diagnostics())
                std::cerr << diagnostic.code << ": " << diagnostic.summary << " / "
                          << diagnostic.detail << '\n';
        } else {
            std::cerr << "packed request timed out before terminal delivery\n";
        }
        checks.expect(false, "GPU packed frame returned");
        return {};
    }
    checks.expect(
        frame->provenance().provider == bloom::runtime::PreviewDisplayProvider::GpuPreparedPacked &&
            frame->processIdentity().provider == bloom::runtime::EvaluationProvider::GpuResident &&
            !frame->hasProcessFrame() && frame->displayBufferView() &&
            frame->desiredIdentity() == identity,
        "GPU scene / host-only packed provenance and complete request identity truthful");
    if (frame->provenance().provider != bloom::runtime::PreviewDisplayProvider::GpuPreparedPacked) {
        std::cerr << "GPU packed provider unavailable: " << service.status().residentDetail << '\n';
        return {};
    }
    return frame;
}

bool matrix(Expectations& checks, const TestOptions& options,
            const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan,
            const std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle>& processor,
            const std::shared_ptr<const GpuDisplayProgramService>& programs,
            const bloom::document::Snapshot& snapshot) {
    PackedRetirementScope retirement;
    TaskScheduler scheduler(schedulerConfig());
    auto policy = std::make_shared<bloom::runtime::GpuProcessingPhasePolicy>(32, retirement.owner);
    auto sample = bloom::runtime::memoryBudgetSample();
    bloom::runtime::MemoryBudgetLedger ledger(sample.physicalBytes,
                                              sample.machine.availableBytes.value_or(0));
    ledger.beginProcessSampling();
    const auto deliveredAt = bloom::runtime::GpuProcessingPhasePolicy::Clock::now();
    const auto sampled = ledger.applySample(std::move(sample), deliveredAt);
    policy->applyHostSample(sampled, ledger.physicalByteBudget(), deliveredAt);
    const auto refresh = [&] {
        auto refreshedSample = bloom::runtime::memoryBudgetSample();
        const auto at = bloom::runtime::GpuProcessingPhasePolicy::Clock::now();
        const auto state = ledger.applySample(std::move(refreshedSample), at);
        policy->applyHostSample(state, ledger.physicalByteBudget(), at);
    };
    const bloom::runtime::GpuScenePreparationPhase phase{
        .policy = policy,
        .envelope = {.metadataBytes = 1024ULL * 1024,
                     .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                     .requestedExecutorBytes = kBudget,
                     .deviceWorkingBytes = 832ULL * 1024 * 1024}};
    auto builder =
        std::make_shared<CpuGpuSceneBuilder>(nullptr, bloom::runtime::GpuSceneMediaContext{},
                                             bloom::runtime::GpuSceneOcioContext{}, phase);
    auto fallbackEvaluator = std::make_shared<CpuCompositionEvaluator>();
    auto preparationCount = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto baseStage = packedStageFunction(plan, builder, programs, processor);
    PreviewGpuSceneStageFunction counted =
        [baseStage,
         preparationCount](const bloom::document::Snapshot& snap,
                           const PreviewRequestIdentity& identity, const std::size_t bytes,
                           const std::vector<bloom::runtime::SnapshotParameterOverride>& overrides,
                           TaskContext& context) {
            ++*preparationCount;
            return baseStage(snap, identity, bytes, overrides, context);
        };
    auto configured = packedOptions(options);
    configured.processingPhasePolicy = policy;
    auto serviceOwner = std::make_unique<GpuPreviewDisplayService>(
        scheduler, counted, generalCpuStageFunction(plan, processor, fallbackEvaluator),
        generalFallback(processor), configured);
    auto& service = *serviceOwner;
    checks.expect(
        waitUntil(
            [&] { return service.status().state != GpuPreviewDisplayServiceState::Initializing; },
            30s),
        "GPU service startup terminal");
    const auto status = service.status();
    if (status.state != GpuPreviewDisplayServiceState::Ready) {
        std::cout << "Unsupported: genuine offscreen GPU preparation unavailable: "
                  << status.residentDetail << '\n';
        checks.expect(!options.requireDevice, "required genuine offscreen GPU device available");
        return false;
    }
    checks.expect(!status.presentationClient &&
                      status.presentationAvailability ==
                          bloom::render::GpuPresentationAvailability::NotRequested,
                  "offscreen product requires no native target or presentation bootstrap");
    auto identity = makeIdentity(*plan, 1, {}, {}, {});
    refresh();
    const auto full = requestPacked(checks, service, snapshot, identity);
    if (!full)
        return true;
    compareCpu(checks, *full, plan, *processor);
    checks.expect(
        waitUntil([&] { return policy->snapshot().reservations.admittedPhases == 0; }, 2s),
        "native terminal stage aliases release the original phase");
    const auto ownedFull = policy->snapshot().reservations;
    checks.expect(
        full->displayOnlyFrame()->packedHostOwnership() ==
                bloom::render::PackedRgba8Ownership::RootedPublished &&
            ownedFull.hostReservedBytes == 0 && ownedFull.admittedPhases == 0 &&
            ownedFull.ownedPackedHostAllocations == 1 &&
            ownedFull.ownedPackedHostBytes ==
                full->provenance().gpuPreparedPacked->transfer.hostStorageBytes &&
            ownedFull.totalHostClaimBytes == ownedFull.ownedPackedHostBytes +
                                                 ownedFull.ownedPackedHostMetadataBytes +
                                                 ownedFull.ownedNonlocalNativeHostBytes,
        "actual rooted native transfer publishes one independently charged host allocation");
    const auto proof = full->provenance().gpuPreparedPacked;
    checks.expect(proof && proof->serviceGeneration != 0 && proof->deviceGeneration != 0 &&
                      proof->transfer.submissions == 1 &&
                      proof->transfer.logicalBytes == 1920ULL * 1080ULL * 4ULL,
                  "actual final GPU image transferred once with exact logical bytes");
    std::cout << "SERVICE_" << (programs ? "GENERAL" : "NEUTRAL")
              << " source=" << proof->transfer.sourceAllocationBytes
              << " staging=" << proof->transfer.stagingAllocationBytes
              << " host=" << proof->transfer.hostStorageBytes << '\n';
    const auto beforeWarm = service.status().counters;
    const auto beforePreparation = preparationCount->load();
    const auto fullView = packedView(*full);
    for (std::uint64_t generation = 2; generation < 202; ++generation) {
        auto warm = bloom::runtime::PreparedPreviewFrame::createDisplayOnly(
            generation, full->displayOnlyFrame());
        checks.expect(warm && packedView(*warm).pixels.data() == fullView.pixels.data() &&
                          warm->provenance().gpuPreparedPacked == proof,
                      "retained packed product reuse shares immutable bytes and sealed evidence");
    }
    checks.expect(
        service.status().counters == beforeWarm && preparationCount->load() == beforePreparation &&
            policy->snapshot().reservations.ownedPackedHostAllocations == 1 &&
            policy->snapshot().reservations.ownedPackedHostBytes == ownedFull.ownedPackedHostBytes,
        "retained packed reuse adds no scene preparation/dispatch or repeated transfer");
    const auto halfExtent = bloom::render::ImageExtent::create(960, 540);
    identity.requestGeneration = 202;
    identity.resolution = bloom::runtime::ProxyResolution{*halfExtent.value()};
    identity.resolutionPolicy = PreviewResolutionPolicy::Half;
    refresh();
    const auto half = requestPacked(checks, service, snapshot, identity);
    if (half) {
        compareCpu(checks, *half, plan, *processor);
        checks.expect(packedView(*half).layout.pixelStorageBytes == 960ULL * 540ULL * 4ULL &&
                          half->provenance().gpuPreparedPacked->transfer.sourceAllocationBytes <
                              proof->transfer.sourceAllocationBytes,
                      "Half changes actual physical GPU/host dimensions and measured charges");
    }
    identity.requestGeneration = 203;
    // Neutral admission remains inside its measured interval; General also proves a small ROI.
    const auto roiWidth = programs ? 173U : 800U;
    const auto roiHeight = programs ? 97U : 400U;
    const auto roiWindow = bloom::render::ImageWindow::create(37, 29, roiWidth, roiHeight);
    identity.roi = *roiWindow.value();
    refresh();
    const auto roi = requestPacked(checks, service, snapshot, identity);
    if (roi) {
        compareCpu(checks, *roi, plan, *processor);
        const auto view = packedView(*roi);
        checks.expect(view.dataWindow == identity.roi &&
                          view.displayWindow.extent() == *halfExtent.value() &&
                          view.layout.pixelStorageBytes ==
                              static_cast<std::uint64_t>(roiWidth) * roiHeight * 4ULL,
                      "ROI owns only actual data extent and preserves full Half display mapping");
        auto copied = bloom::runtime::PreviewDisplayOnlyFrame::create(*roi, kBudget);
        const auto copiedView = copied ? copied->displayBufferView() : std::nullopt;
        checks.expect(copiedView && copiedView->dataWindow == view.dataWindow &&
                          copiedView->displayWindow == view.displayWindow &&
                          std::ranges::equal(copiedView->pixels, view.pixels),
                      "retention copy preserves ROI extent, mapping and GPU provenance");
        checks.expect(
            copied &&
                copied->packedHostOwnership() == bloom::render::PackedRgba8Ownership::LegacyUnowned,
            "legacy unrooted retention copy is explicitly unowned, never zero-allocation proof");
    }
    if (programs) {
        identity.requestGeneration = 204;
        identity.roi.reset();
        identity.viewAdjust = {.exposure = 1.0, .gamma = 0.8};
        refresh();
        const auto adjusted = requestPacked(checks, service, snapshot, identity);
        if (adjusted) {
            compareCpu(checks, *adjusted, plan, *processor);
            checks.expect(adjusted->provenance().gpuPreparedPacked->displayCommandIdentity !=
                              proof->displayCommandIdentity,
                          "changed display adjustment names exact changed GPU command");
        }
    }
    const auto counters = service.status().counters;
    std::cout << "SERVICE_COUNTERS transfers=" << counters.packedTransfers
              << " readbacks=" << counters.fullFrameReadbacks
              << " dispatches=" << counters.nativeDispatches
              << " fallback=" << counters.cpuFallbacks
              << " reserved=" << counters.packedReservedBytes
              << " host=" << counters.packedRetainedHostBytes << '\n';
    checks.expect(
        counters.cpuFallbacks == 0 && counters.nativeDispatches > 0 &&
            counters.packedTransfers == (programs ? 4U : 3U) &&
            counters.fullFrameReadbacks == counters.packedTransfers &&
            counters.packedReservedBytes == 0 && counters.packedRetainedHostBytes == 0,
        "production GPU scene and one final transfer per cold product, no CPU scene fallback");
    auto externalPin = full->displayOnlyFrame();
    const auto retainedBytes =
        proof->transfer.hostStorageBytes +
        (half ? half->provenance().gpuPreparedPacked->transfer.hostStorageBytes : 0) +
        (roi ? roi->provenance().gpuPreparedPacked->transfer.hostStorageBytes : 0);
    checks.expect(
        waitUntil(
            [&] { return policy->snapshot().reservations.ownedPackedHostBytes == retainedBytes; },
            2s),
        "off-UI disposal completes before measuring only still-live external products");
    service.beginShutdown();
    serviceOwner.reset();
    const auto externalView = externalPin->displayBufferView();
    checks.expect(
        waitUntil([&] { return policy->snapshot().deviceReports == 0; }, 2s) &&
            policy->snapshot().reservations.admittedPhases == 0 &&
            policy->snapshot().reservations.hostReservedBytes == 0 &&
            policy->snapshot().reservations.ownedPackedHostBytes == retainedBytes &&
            externalPin->packedHostOwnership() ==
                bloom::render::PackedRgba8Ownership::RootedPublished &&
            externalView && externalView->pixels.data() == fullView.pixels.data(),
        "real native owner teardown retires its report while external host pixels stay charged");
    return true;
}

int run(int argc, char** argv) {
    bool shutdownProof = false;
    std::vector<char*> arguments;
    for (int index = 0; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--shutdown-proof")
            shutdownProof = true;
        else
            arguments.push_back(argv[index]);
    }
    const auto options = parseOptions(static_cast<int>(arguments.size()), arguments.data());
    if (!options.valid)
        return 2;
    auto resolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    auto neutral = std::move(resolution).takeResolved();
    if (!neutral)
        return 1;
    auto processor = makeNeutralProcessor(*neutral);
    if (!processor)
        return 1;
    GpuOcioContextRequest ocioRequest;
    ocioRequest.applicationExecutable = std::filesystem::path{BLOOM_GPU_GENERAL_TEST_EXECUTABLE};
    ocioRequest.toolPackage.toolsDirectory = BLOOM_GPU_TOOLS_DIR;
    ocioRequest.toolPackage.inventoryName = BLOOM_GPU_TOOLS_INVENTORY_NAME;
    ocioRequest.toolPackage.glslangValidatorName = BLOOM_GPU_TOOLS_GLSLANG_NAME;
    ocioRequest.toolPackage.spirvValName = BLOOM_GPU_TOOLS_SPIRV_VAL_NAME;
    ocioRequest.toolPackage.relocated = static_cast<bool>(BLOOM_GPU_TOOLS_RELOCATED);
#ifdef BLOOM_GPU_TOOLS_BUNDLE_RELATIVE
    ocioRequest.toolPackage.bundleRelative = true;
#endif
#ifdef BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256
    ocioRequest.toolPackage.glslangStagedDigest = bloom::core::Sha256Digest::fromLowercaseHex(
        std::string_view{BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256}.substr(7));
#endif
#ifdef BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256
    ocioRequest.toolPackage.spirvValStagedDigest = bloom::core::Sha256Digest::fromLowercaseHex(
        std::string_view{BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256}.substr(7));
#endif
    auto programs = std::make_shared<const GpuDisplayProgramService>(
        std::make_shared<GpuOcioContextResolver>(std::move(ocioRequest)));
    const auto plan = makePlan(1920, 1080);
    bloom::document::Project project(kProjectId, "packed-proof");
    const auto seed = bloom::document::makeNewProject("Fixture", "Main",
                                                      RationalTime::fromInteger(4), plan->format());
    if (!project.addComposition(bloom::document::Composition(
            plan->compositionId(), "Main", RationalTime::fromInteger(4),
            seed.project.findComposition(seed.initialCompositionId)->graph(), plan->format())))
        return 1;
    bloom::document::Document document(std::move(project));
    Expectations checks;
    if (!shutdownProof) {
        if (!matrix(checks, options, plan, processor, {}, document.snapshot()) ||
            !matrix(checks, options, plan, processor, programs, document.snapshot()))
            return options.requireDevice ? 1 : kSkipExit;
        unsupportedPackedProof(checks, packedOptions(options), plan, processor, programs,
                               document.snapshot());
        payloadAllocationFailureProof(checks, packedOptions(options), plan, processor,
                                      document.snapshot());
    }
    heldTransferProof(checks, packedOptions(options), plan, processor, programs,
                      document.snapshot(), shutdownProof);
    if (checks.failures())
        return 1;
    std::cout << (shutdownProof ? "PASS: bounded shutdown quarantine reservation/context\n"
                                : "PASS: offscreen GPU packed parity/reuse/late-cancel/detach\n");
    return 0;
}
#else
int run(int argc, char** argv) {
    const auto options = parseOptions(argc, argv);
    std::cout << "Unsupported: genuine GPU preparation tools unavailable\n";
    return options.requireDevice ? 1 : kSkipExit;
}
#endif
} // namespace
int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
