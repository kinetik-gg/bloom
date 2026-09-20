// Real general-display service acceptance for GpuPreviewDisplayService.
//
// It drives the production service with a real PreviewGpuSceneStageFunction that carries an
// off-UI-prepared general GPU display program (runtime::GpuDisplayProgramService over the real
// OCIO config + pinned glslang/spirv-val). The service must:
//   (1) take the prepared general arm for the ORDINARY DEFAULT Neutral request at a geometry ABOVE
//       the retired 4K ceiling -- proving the old pixel-interval/4K gate is gone at the actual
//       service, not only in the arm;
//   (2) publish a genuine resident GpuResident frame with a valid lease and NO full-frame readback;
//   (3) serve a warm identical request and a display-only adjustment change while RETAINING the
//       process output (scene content-cache hit, zero additional native scene operations) and
//       changing only the exact DisplayRgba8 command identity.
//
// The presentation generation is required by the resident route (the production gate), so the test
// requests Wayland presentation exactly as the app does; without a presentable device it is an
// explicit SKIP (exit 77) unless --require-device is passed. Nothing here fabricates an image, a
// command identity, or a qualification report.

#include "gpu_preview_display_service_general_support.hpp"

#include "gpu_preview_display_service_general_fixture.ipp"

int main(const int argc, char** argv) {
    const TestOptions options = parseOptions(argc, argv);
    if (!options.valid) {
        return 2;
    }
#if !defined(BLOOM_GPU_TOOLS_AVAILABLE) || (BLOOM_GPU_TOOLS_AVAILABLE == 0)
    std::cout << "SKIP: the packaged GPU shader tools are unavailable\n";
    return kSkipExit;
#else
    Expectations expectations;
    auto neutralResolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    auto neutral = std::move(neutralResolution).takeResolved();
    if (!neutral.has_value()) {
        std::cerr << "FAILED: the Bloom Neutral v1 built-in does not resolve\n";
        return 1;
    }
    auto processor = makeNeutralProcessor(*neutral);
    if (processor == nullptr) {
        std::cerr << "FAILED: the Bloom Neutral v1 display processor does not build\n";
        return 1;
    }
    const auto acesRevision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    if (!acesRevision.has_value()) {
        std::cout << "SKIP: the ACES built-in is unavailable\n";
        return kSkipExit;
    }
    auto acesResolution =
        bloom::color::resolveOcioBuiltIn(bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                                         bloom::color::kAcesCgV1ConfigUri, *acesRevision, "ACEScg");
    auto aces = std::move(acesResolution).takeResolved();
    if (!aces.has_value()) {
        std::cout << "SKIP: the ACES built-in does not resolve\n";
        return kSkipExit;
    }
    std::string acesDisplay;
    std::string acesView;
    for (const auto& candidate : aces->displays()) {
        auto built =
            bloom::color::buildCpuDisplayProcessorForView(*aces, candidate.display, candidate.view);
        if (built.handle() != nullptr) {
            acesDisplay = candidate.display;
            acesView = candidate.view;
            break;
        }
    }
    if (acesDisplay.empty() || acesView.empty()) {
        std::cout << "SKIP: no ACES display/view pair has a CPU processor\n";
        return kSkipExit;
    }

    // The production shape: one shared runtime OCIO context resolver built from the packaged tools
    // staged beside this test executable; the display service consumes it (no injected paths).
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
    auto programService = std::make_shared<const GpuDisplayProgramService>(
        std::make_shared<GpuOcioContextResolver>(std::move(ocioRequest)));
    // The exact ACES project color identity used for the non-default requests.
    EvaluationColorIntent acesIntent;
    acesIntent.workingColorSpaceId = "ACEScg";
    acesIntent.ocioConfigRevision = *acesRevision;
    acesIntent.ocioConfigUri = bloom::color::kAcesCgV1ConfigUri;

    // Acceptance (1): the DEFAULT Neutral request above the retired 4K ceiling (3840*2160).
    constexpr std::uint32_t kNeutralWidth = 4096;
    constexpr std::uint32_t kNeutralHeight = 2304;
    static_assert(static_cast<std::uint64_t>(kNeutralWidth) * kNeutralHeight > 3840ULL * 2160ULL,
                  "the default Neutral case must exceed the retired 4K ceiling");
    auto neutralPlan = makePlan(kNeutralWidth, kNeutralHeight);

    auto builder = std::make_shared<CpuGpuSceneBuilder>();
    auto evaluator = std::make_shared<CpuCompositionEvaluator>();
    TaskScheduler scheduler(schedulerConfig());
    GpuPreviewDisplayService service(
        scheduler, generalStageFunction(neutralPlan, builder, programService, processor),
        generalCpuStageFunction(neutralPlan, processor, evaluator), generalFallback(processor),
        serviceOptions(options.loaderPath));

    const bool terminal = waitUntil(
        [&] { return service.status().state != GpuPreviewDisplayServiceState::Initializing; }, 90s);
    const auto status = service.status();
    const bool residentReady =
        terminal && status.state == GpuPreviewDisplayServiceState::Ready &&
        status.residentQualification != nullptr && status.residentQualification->eligible() &&
        status.presentationClient != nullptr &&
        status.presentationAvailability == bloom::render::GpuPresentationAvailability::Ready;
    if (!residentReady) {
        service.beginShutdown();
        if (options.requireDevice) {
            std::cerr << "FAIL: a presentable resident route is required but unavailable: "
                      << status.residentDetail << " / " << status.presentationDetail << '\n';
            return 1;
        }
        std::cout << "SKIP: resident route unavailable; no native success claimed: "
                  << status.residentDetail << " / " << status.presentationDetail << '\n';
        return kSkipExit;
    }

    const auto core = bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::coreOf(service);
    expectations.expect(core != nullptr, "the service core is reachable");
    if (core == nullptr) {
        service.beginShutdown();
        return 1;
    }
    expectations.expect(
        bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::residentRouteAvailable(*core),
        "the owner created the resident scene executor/display on the service device");
    expectations.expect(
        !bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::generalDisplayActive(*core),
        "the general display arm is created lazily, not at startup");

    bloom::runtime::TaskOwner owner;
    owner.kind = bloom::runtime::TaskOwnerKind::Composition;
    owner.id = bloom::runtime::TaskOwnerId::fromRaw(7);
    const auto snapshot = [&] {
        bloom::document::Document document(bloom::document::Project(kProjectId, "general-service"));
        return document.snapshot();
    }();

    // --- Acceptance (1): ordinary DEFAULT Neutral above the retired 4K ceiling ---------------
    const auto neutralIdentity = makeIdentity(*neutralPlan, 1, {}, {}, ViewAdjust{});
    auto neutralSubmission = service.submit(TaskRequest("general neutral", owner), snapshot,
                                            neutralIdentity, kBudget, {});
    expectations.expect(neutralSubmission.status == TaskSubmissionStatus::Accepted,
                        "the >4K default Neutral general request was admitted");
    const auto neutralResult = awaitResult(neutralSubmission.handle);
    std::shared_ptr<const bloom::runtime::PreparedPreviewFrame> neutralFrame;
    const bool neutralResident = isResidentPrepared(neutralResult, 1, neutralFrame);
    if (!neutralResident) {
        std::cerr << "neutral >4K general request did not produce a resident frame: "
                  << service.status().residentDetail << '\n';
    }
    expectations.expect(
        neutralResident,
        "the >4K default Neutral request produced a resident GpuResident frame at the "
        "actual service (old 4K/pixel-interval gate gone)");
    expectations.expect(
        bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::generalDisplayActive(*core),
        "the service took the prepared general display arm for the default Neutral request");
    expectations.expect(neutralFrame != nullptr && neutralFrame->residentFrame() != nullptr &&
                            neutralFrame->residentFrame()->lease().isValid(),
                        "the general frame carries a valid resident lease");
    expectations.expect(
        bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::generalDisplayIdentity(*core) !=
            bloom::core::Sha256Digest{},
        "the service retained the exact general display command identity");
    const auto afterNeutral = service.status().counters;
    expectations.expect(afterNeutral.fullFrameReadbacks == 0U,
                        "the resident general route performed no full-frame readback");
    expectations.expect(
        afterNeutral.displayStatusReads >= 1U,
        "the general display invalidated its status word (a real display dispatch)");

    // --- Acceptance (2): non-default ACES, cold/warm, display-only change -------------------
    const ViewAdjust adjustA{.exposure = 1.0, .gamma = 0.8};
    const ViewAdjust adjustB{.exposure = -1.0, .gamma = 1.0};
    const auto identityA =
        makeIdentity(*neutralPlan, 1, acesDisplay, acesView, adjustA, acesIntent);
    const auto identityB =
        makeIdentity(*neutralPlan, 2, acesDisplay, acesView, adjustB, acesIntent);

    auto coldSubmission =
        service.submit(TaskRequest("general aces cold", owner), snapshot, identityA, kBudget, {});
    expectations.expect(coldSubmission.status == TaskSubmissionStatus::Accepted,
                        "the non-default adjusted request was admitted");
    const auto coldResult = awaitResult(coldSubmission.handle);
    std::shared_ptr<const bloom::runtime::PreparedPreviewFrame> coldFrame;
    const bool coldResident = isResidentPrepared(coldResult, 1, coldFrame);
    if (!coldResident) {
        std::cerr << "non-default adjusted request did not produce a resident frame: "
                  << service.status().residentDetail << '\n';
    }
    expectations.expect(
        coldResident, "the non-default display + adjustment produced a resident GpuResident frame");
    expectations.expect(coldFrame != nullptr && coldFrame->residentFrame() != nullptr &&
                            coldFrame->residentFrame()->lease().isValid(),
                        "the cold adjusted frame carries a valid resident lease");
    const auto identityACommand =
        bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::generalDisplayIdentity(*core);
    expectations.expect(identityACommand != bloom::core::Sha256Digest{},
                        "the cold adjusted frame bound an exact display command identity");
    const auto afterCold = service.status().counters;
    expectations.expect(afterCold.fullFrameReadbacks == 0U,
                        "the cold adjusted resident frame performed no full-frame readback");

    // Warm identical request: the process output is retained (scene cache hit, no new native scene
    // operation) and the same display command identity is reused.
    auto warmSubmission =
        service.submit(TaskRequest("general aces warm", owner), snapshot, identityA, kBudget, {});
    expectations.expect(warmSubmission.status == TaskSubmissionStatus::Accepted,
                        "the warm adjusted request was admitted");
    const auto warmResult = awaitResult(warmSubmission.handle);
    std::shared_ptr<const bloom::runtime::PreparedPreviewFrame> warmFrame;
    expectations.expect(isResidentPrepared(warmResult, 1, warmFrame),
                        "the warm adjusted request produced a resident frame");
    const auto afterWarm = service.status().counters;
    expectations.expect(afterWarm.gpuCacheHits > afterCold.gpuCacheHits,
                        "the warm request reused the retained process output from the scene cache");
    expectations.expect(afterWarm.nativeDispatches == afterCold.nativeDispatches,
                        "the warm request performed zero additional native scene operations");
    expectations.expect(
        bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::generalDisplayIdentity(*core) ==
            identityACommand,
        "the warm request retained the exact display command identity");

    // Regression: the controller passes the HOST pixel-storage allowance from settings, which may
    // be far larger than the effective GPU request ceiling. That host ceiling is the
    // decode/CPU-fallback limit and must NEVER force the GPU to the CPU; the device stage clamps to
    // the GPU share.
    const auto beforeLargeHost = service.status().counters;
    const std::uint64_t largeHostLimit = 64ULL * kBudget;
    const auto largeHostIdentity =
        makeIdentity(*neutralPlan, 7, acesDisplay, acesView, adjustA, acesIntent);
    auto largeHostSubmission = service.submit(TaskRequest("general large host ceiling", owner),
                                              snapshot, largeHostIdentity, largeHostLimit, {});
    expectations.expect(largeHostSubmission.status == TaskSubmissionStatus::Accepted,
                        "a request whose host ceiling exceeds the GPU budget was admitted");
    const auto largeHostResult = awaitResult(largeHostSubmission.handle);
    std::shared_ptr<const bloom::runtime::PreparedPreviewFrame> largeHostFrame;
    if (!isResidentPrepared(largeHostResult, 7, largeHostFrame)) {
        std::cerr << "large host ceiling did not produce a resident frame: "
                  << service.status().residentDetail << '\n';
    }
    expectations.expect(largeHostFrame != nullptr && largeHostFrame->residentFrame() != nullptr &&
                            largeHostFrame->residentFrame()->lease().isValid(),
                        "a large host ceiling still produced a resident GpuResident lease");
    const auto afterLargeHost = service.status().counters;
    expectations.expect(afterLargeHost.residentGraphJobs > beforeLargeHost.residentGraphJobs,
                        "a large host ceiling still admitted a GPU graph job");
    expectations.expect(afterLargeHost.cpuFallbacks == beforeLargeHost.cpuFallbacks,
                        "a large host ceiling did NOT force a CPU fallback");
    expectations.expect(afterLargeHost.fullFrameReadbacks == 0U,
                        "the large-host-ceiling resident frame performed no full-frame readback");

    // Display-only change: same process scene, a different adjustment -> the process output is
    // retained and only the DisplayRgba8 command identity changes.
    auto changeSubmission = service.submit(TaskRequest("general aces display-only", owner),
                                           snapshot, identityB, kBudget, {});
    expectations.expect(changeSubmission.status == TaskSubmissionStatus::Accepted,
                        "the display-only change request was admitted");
    const auto changeResult = awaitResult(changeSubmission.handle);
    std::shared_ptr<const bloom::runtime::PreparedPreviewFrame> changeFrame;
    expectations.expect(isResidentPrepared(changeResult, 2, changeFrame),
                        "the display-only change produced a resident frame");
    const auto afterChange = service.status().counters;
    expectations.expect(afterChange.gpuCacheHits > afterWarm.gpuCacheHits,
                        "the display-only change retained the process output (scene cache hit)");
    expectations.expect(
        afterChange.nativeDispatches == afterWarm.nativeDispatches,
        "the display-only change performed zero additional native scene operations");
    expectations.expect(
        bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::generalDisplayIdentity(*core) !=
            identityACommand,
        "a changed adjustment changed the exact DisplayRgba8 command identity");
    expectations.expect(afterChange.fullFrameReadbacks == 0U,
                        "the whole general service run performed no full-frame readback");

    service.beginShutdown();
    expectations.expect(
        waitUntil([&] { return service.status().state == GpuPreviewDisplayServiceState::Stopped; },
                  60s),
        "the general service owner drained and stopped");
    scheduler.beginShutdown();
    expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }, 30s),
                        "the scheduler reached quiescence");

    // Forced Neutral-qualification refusal: an impossible qualification budget makes the
    // Neutral-specific report ineligible, yet the service keeps the device/scene executor and the
    // general display route still runs when its own requirements (the prepared binding) are valid.
    {
        TaskScheduler forcedScheduler(schedulerConfig());
        GpuPreviewDisplayServiceOptions forcedOptions = serviceOptions(options.loaderPath);
        forcedOptions.residentQualificationBudgets.maxImageBytes = 1;
        GpuPreviewDisplayService forcedService(
            forcedScheduler, generalStageFunction(neutralPlan, builder, programService, processor),
            generalCpuStageFunction(neutralPlan, processor, evaluator), generalFallback(processor),
            forcedOptions);
        const bool forcedTerminal = waitUntil(
            [&] {
                return forcedService.status().state != GpuPreviewDisplayServiceState::Initializing;
            },
            90s);
        const auto forcedStatus = forcedService.status();
        expectations.expect(forcedTerminal, "forced-gate: the service reaches a terminal state");
        expectations.expect(forcedStatus.residentQualification != nullptr &&
                                !forcedStatus.residentQualification->eligible(),
                            "forced-gate: the Neutral qualification is genuinely ineligible");
        expectations.expect(forcedStatus.state == GpuPreviewDisplayServiceState::Ready &&
                                forcedStatus.presentationClient != nullptr &&
                                forcedStatus.presentationAvailability ==
                                    bloom::render::GpuPresentationAvailability::Ready,
                            "forced-gate: the general route keeps the service Ready");
        const auto forcedCore =
            bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::coreOf(forcedService);
        if (forcedCore != nullptr && forcedStatus.state == GpuPreviewDisplayServiceState::Ready &&
            forcedStatus.presentationClient != nullptr) {
            auto forcedSubmission = forcedService.submit(TaskRequest("forced neutral 4k", owner),
                                                         snapshot, neutralIdentity, kBudget, {});
            const auto forcedResult = awaitResult(forcedSubmission.handle);
            std::shared_ptr<const bloom::runtime::PreparedPreviewFrame> forcedFrame;
            expectations.expect(
                isResidentPrepared(forcedResult, 1, forcedFrame),
                "forced-gate: the general arm runs despite the ineligible Neutral qualification");
            expectations.expect(
                bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::generalDisplayActive(
                    *forcedCore),
                "forced-gate: the general display arm was created");
        }
        forcedService.beginShutdown();
        expectations.expect(waitUntil(
                                [&] {
                                    return forcedService.status().state ==
                                           GpuPreviewDisplayServiceState::Stopped;
                                },
                                60s),
                            "forced-gate: the owner drained and stopped");
        forcedScheduler.beginShutdown();
        expectations.expect(waitUntil([&] { return forcedScheduler.isQuiescent(); }, 30s),
                            "forced-gate: the scheduler reached quiescence");
    }

    // Production-default scheduler regression: `TaskSchedulerConfig::defaults()` with NO bespoke
    // gpuRequestOwnedByteCapacity must admit a full-resolution 6000x4000 resident request whose
    // resolved GPU budget exceeds the retired fixed 1 GiB ceiling. The host submit limit is far
    // larger than the GPU share and must not force the CPU. Run as a separate fixture so the
    // default scheduler config, not a test override, is the thing under test.
    {
        constexpr std::size_t kGib = std::size_t{1} << 30U;
        auto bigPlan = makePlan(6000U, 4000U);

        TaskScheduler defaultScheduler(TaskSchedulerConfig::defaults());
        GpuPreviewDisplayServiceOptions defaultOptions = serviceOptions(options.loaderPath);
        defaultOptions.residentLeaseBudgets.maxBytes = 4ULL * kGib;
        defaultOptions.residentSceneCacheBudgets.maxRetainedBytes = 4ULL * kGib;
        defaultOptions.residentExecutorBudgets.maxImageBytes = 2ULL * kGib;
        defaultOptions.residentExecutorBudgets.maxOcioRetainedProgramBytes = 2ULL * kGib;
        defaultOptions.residentExecutorBudgets.maxOcioOwnedBytesPerProgram = 2ULL * kGib;
        defaultOptions.previewByteAllowance = 2ULL * kGib;
        GpuPreviewDisplayService defaultService(
            defaultScheduler, generalStageFunction(bigPlan, builder, programService, processor),
            generalCpuStageFunction(bigPlan, processor, evaluator), generalFallback(processor),
            defaultOptions);

        const bool defaultTerminal = waitUntil(
            [&] {
                return defaultService.status().state != GpuPreviewDisplayServiceState::Initializing;
            },
            120s);
        const auto defaultStatus = defaultService.status();
        const bool defaultReady = defaultTerminal &&
                                  defaultStatus.state == GpuPreviewDisplayServiceState::Ready &&
                                  defaultStatus.presentationClient != nullptr &&
                                  defaultStatus.presentationAvailability ==
                                      bloom::render::GpuPresentationAvailability::Ready;
        if (!defaultReady) {
            expectations.expect(
                !options.requireDevice,
                "default-scheduler: the resident route must be Ready when required");
            std::cout << "SKIP: default-scheduler resident route unavailable: "
                      << defaultStatus.residentDetail << " / " << defaultStatus.presentationDetail
                      << '\n';
        } else {
            expectations.expect(
                defaultStatus.residentCapacityPlan.requestBytes > kGib,
                "default-scheduler: the resolved GPU request budget exceeds the old "
                "fixed 1 GiB ceiling");
            const auto bigIdentity = makeIdentity(*bigPlan, 1, {}, {}, ViewAdjust{});
            const std::uint64_t hostLimit = 64ULL * kGib;
            auto bigSubmission = defaultService.submit(TaskRequest("default scheduler 6k", owner),
                                                       snapshot, bigIdentity, hostLimit, {});
            expectations.expect(bigSubmission.status == TaskSubmissionStatus::Accepted,
                                "default-scheduler: the 6K request was admitted under defaults()");
            const auto bigResult = awaitResult(bigSubmission.handle, 120s);
            std::shared_ptr<const bloom::runtime::PreparedPreviewFrame> bigFrame;
            if (!isResidentPrepared(bigResult, 1, bigFrame)) {
                std::cerr << "default-scheduler 6K did not produce a resident frame: "
                          << defaultService.status().residentDetail << '\n';
            }
            expectations.expect(bigFrame != nullptr && bigFrame->residentFrame() != nullptr &&
                                    bigFrame->residentFrame()->lease().isValid(),
                                "default-scheduler: the 6K request produced a resident GpuResident "
                                "lease");
            const auto afterBig = defaultService.status().counters;
            expectations.expect(afterBig.residentGraphJobs > 0U,
                                "default-scheduler: a positive GPU graph-job count");
            expectations.expect(afterBig.gpuAdmissionRefusals == 0U,
                                "default-scheduler: no bounded-admission refusal");
            expectations.expect(afterBig.cpuFallbacks == 0U,
                                "default-scheduler: the 6K request did NOT fall back to the CPU");

            const auto defaultWarmIdentity = makeIdentity(*bigPlan, 2, {}, {}, ViewAdjust{});
            auto defaultWarmSubmission =
                defaultService.submit(TaskRequest("default scheduler 6k warm", owner), snapshot,
                                      defaultWarmIdentity, hostLimit, {});
            const auto defaultWarmResult = awaitResult(defaultWarmSubmission.handle, 120s);
            std::shared_ptr<const bloom::runtime::PreparedPreviewFrame> defaultWarmFrame;
            expectations.expect(isResidentPrepared(defaultWarmResult, 2, defaultWarmFrame),
                                "default-scheduler: the warm 6K request produced a resident frame");
            const auto afterBigWarm = defaultService.status().counters;
            expectations.expect(afterBigWarm.gpuCacheHits > afterBig.gpuCacheHits,
                                "default-scheduler: the warm 6K request reused the retained scene");
        }
        defaultService.beginShutdown();
        static_cast<void>(waitUntil(
            [&] { return defaultService.status().state == GpuPreviewDisplayServiceState::Stopped; },
            60s));
        defaultScheduler.beginShutdown();
        static_cast<void>(waitUntil([&] { return defaultScheduler.isQuiescent(); }, 30s));
    }

    // Production graph + production capacity options + production-DEFAULT scheduler: a full
    // 4608x3164 EXR source over a 6000x4000 solid + text composition must take the actual GPU
    // resident route with positive native dispatches, ZERO CPU fallback and ZERO admission refusal,
    // then reuse the retained scene on an identical warm request. The capacity options come from
    // the same ledger-derived split gpu_preview_app uses, not ad hoc values.
    {
        namespace runtime = bloom::runtime;
        const auto mediaDirectory = makeUniqueProductionMediaDirectory();
        std::filesystem::create_directories(mediaDirectory);
        const auto exrPath = mediaDirectory / "production_4608x3164.exr";
        writeProductionLargeExr(exrPath, 4608, 3164);
        const auto mediaAsset = productionImageAsset(exrPath, 7001);
        auto mediaEvaluator = std::make_shared<CpuCompositionEvaluator>();
        mediaEvaluator->setAssetBaseDirectory(mediaDirectory);
        auto mediaContext = runtime::GpuSceneMediaContext::fromEvaluator(*mediaEvaluator);
        mediaContext.assetBaseDirectory = mediaDirectory;
        auto mediaBuilder = std::make_shared<CpuGpuSceneBuilder>(nullptr, std::move(mediaContext));
        auto mediaPlan = makeProductionMediaTextPlan(format(6000U, 4000U), mediaAsset, 8000);

        TaskScheduler productionScheduler(TaskSchedulerConfig::defaults());
        GpuPreviewDisplayServiceOptions productionOptions = serviceOptions(options.loaderPath);
        const auto ledgerAllocation = runtime::processMemoryBudgetLedger().allocate();
        const auto configuredPlan = runtime::gpuResidentCapacityPlanConfigured(
            ledgerAllocation.previewFrameCacheByteBudget);
        // Capacity options exactly as gpu_preview_app derives them: the configured resident plan
        // from the ledger preview budget, and the PRODUCTION executor image ceiling (the test
        // harness's ad hoc 256 MiB cap is deliberately replaced with the app's ledger-derived
        // default).
        productionOptions.residentLeaseBudgets.maxBytes = configuredPlan.leaseBytes;
        productionOptions.residentSceneCacheBudgets.maxRetainedBytes =
            configuredPlan.sceneCacheBytes;
        productionOptions.previewByteAllowance = configuredPlan.requestBytes;
        productionOptions.residentExecutorBudgets = runtime::GpuSceneExecutorBudgets{};
        GpuPreviewDisplayService productionService(
            productionScheduler,
            generalStageFunction(mediaPlan, mediaBuilder, programService, processor),
            generalCpuStageFunction(mediaPlan, processor, mediaEvaluator),
            generalFallback(processor), productionOptions);

        const bool productionTerminal = waitUntil(
            [&] {
                return productionService.status().state !=
                       GpuPreviewDisplayServiceState::Initializing;
            },
            120s);
        const auto productionStatus = productionService.status();
        const bool productionReady =
            productionTerminal && productionStatus.state == GpuPreviewDisplayServiceState::Ready &&
            productionStatus.presentationClient != nullptr &&
            productionStatus.presentationAvailability ==
                bloom::render::GpuPresentationAvailability::Ready;
        if (!productionReady) {
            expectations.expect(!options.requireDevice,
                                "production: the resident route must be Ready when required");
            std::cout << "SKIP: production resident route unavailable: "
                      << productionStatus.residentDetail << '\n';
        } else {
            const auto productionIdentity = makeIdentity(*mediaPlan, 1, {}, {}, ViewAdjust{});
            const std::uint64_t hostLimit = 64ULL * (std::size_t{1} << 30U);
            auto productionSubmission =
                productionService.submit(TaskRequest("production media text 6k", owner), snapshot,
                                         productionIdentity, hostLimit, {});
            expectations.expect(productionSubmission.status == TaskSubmissionStatus::Accepted,
                                "production: the media+text 6K request was admitted");
            const auto productionResult = awaitResult(productionSubmission.handle, 180s);
            std::shared_ptr<const bloom::runtime::PreparedPreviewFrame> productionFrame;
            if (!isResidentPrepared(productionResult, 1, productionFrame)) {
                const auto diag = productionService.status();
                std::cerr << "production media+text 6K did not produce a resident frame: detail='"
                          << diag.residentDetail
                          << "' requestBytes=" << diag.residentCapacityPlan.requestBytes
                          << " nativeDispatches=" << diag.counters.nativeDispatches
                          << " graphJobs=" << diag.counters.residentGraphJobs
                          << " completions=" << diag.counters.residentCompletions
                          << " failures=" << diag.counters.residentFailures
                          << " cacheHits=" << diag.counters.gpuCacheHits
                          << " cacheMisses=" << diag.counters.gpuCacheMisses
                          << " cpuFallbacks=" << diag.counters.cpuFallbacks
                          << " admissionRefusals=" << diag.counters.gpuAdmissionRefusals << '\n';
            }
            expectations.expect(productionFrame != nullptr &&
                                    productionFrame->residentFrame() != nullptr &&
                                    productionFrame->residentFrame()->lease().isValid(),
                                "production: the media+text 6K request produced a genuine resident "
                                "GpuResident lease");
            const auto afterProduction = productionService.status().counters;
            expectations.expect(afterProduction.nativeDispatches > 0U,
                                "production: the media+text 6K request performed real native GPU "
                                "dispatches");
            expectations.expect(afterProduction.gpuAdmissionRefusals == 0U,
                                "production: no bounded-admission refusal under defaults()");
            expectations.expect(
                afterProduction.cpuFallbacks == 0U,
                "production: the media+text 6K request did NOT fall back to the CPU");

            std::cout << "PHASE production-cold requestBytes="
                      << productionStatus.residentCapacityPlan.requestBytes
                      << " nativeDispatches=" << afterProduction.nativeDispatches
                      << " graphJobs=" << afterProduction.residentGraphJobs
                      << " completions=" << afterProduction.residentCompletions
                      << " failures=" << afterProduction.residentFailures
                      << " cacheHits=" << afterProduction.gpuCacheHits
                      << " cacheMisses=" << afterProduction.gpuCacheMisses
                      << " cpuFallbacks=" << afterProduction.cpuFallbacks
                      << " admissionRefusals=" << afterProduction.gpuAdmissionRefusals << '\n';

            const auto productionWarmIdentity = makeIdentity(*mediaPlan, 2, {}, {}, ViewAdjust{});
            auto productionWarmSubmission =
                productionService.submit(TaskRequest("production media text 6k warm", owner),
                                         snapshot, productionWarmIdentity, hostLimit, {});
            const auto productionWarmResult = awaitResult(productionWarmSubmission.handle, 180s);
            std::shared_ptr<const bloom::runtime::PreparedPreviewFrame> productionWarmFrame;
            expectations.expect(isResidentPrepared(productionWarmResult, 2, productionWarmFrame),
                                "production: the warm media+text 6K request produced a resident "
                                "frame");
            const auto afterProductionWarm = productionService.status().counters;
            expectations.expect(
                afterProductionWarm.gpuCacheHits > afterProduction.gpuCacheHits,
                "production: the warm media+text request reused the retained scene");
            expectations.expect(afterProductionWarm.nativeDispatches ==
                                    afterProduction.nativeDispatches,
                                "production: the warm media+text request performed zero additional "
                                "native dispatches");
            expectations.expect(afterProductionWarm.cpuFallbacks == afterProduction.cpuFallbacks,
                                "production: the warm media+text request did not fall back to the "
                                "CPU");
            std::cout << "PHASE production-warm nativeDispatches="
                      << afterProductionWarm.nativeDispatches
                      << " cacheHits=" << afterProductionWarm.gpuCacheHits
                      << " cpuFallbacks=" << afterProductionWarm.cpuFallbacks
                      << " admissionRefusals=" << afterProductionWarm.gpuAdmissionRefusals << '\n';
        }
        productionService.beginShutdown();
        static_cast<void>(waitUntil(
            [&] {
                return productionService.status().state == GpuPreviewDisplayServiceState::Stopped;
            },
            60s));
        productionScheduler.beginShutdown();
        static_cast<void>(waitUntil([&] { return productionScheduler.isQuiescent(); }, 30s));
        std::error_code ignoredCleanup;
        std::filesystem::remove_all(mediaDirectory, ignoredCleanup);
    }

    // Explicit small scheduler capacity is an INTENTIONAL refusal: the service must record the
    // bounded-admission refusal and take the honest CPU path, never silently substitute a GPU
    // frame.
    {
        TaskSchedulerConfig refusalConfig = schedulerConfig();
        refusalConfig.gpuRequestOwnedByteCapacity = 1; // one byte: every real request is refused
        TaskScheduler refusalScheduler(refusalConfig);
        GpuPreviewDisplayService refusalService(
            refusalScheduler, generalStageFunction(neutralPlan, builder, programService, processor),
            generalCpuStageFunction(neutralPlan, processor, evaluator), generalFallback(processor),
            serviceOptions(options.loaderPath));
        const bool refusalTerminal = waitUntil(
            [&] {
                return refusalService.status().state != GpuPreviewDisplayServiceState::Initializing;
            },
            90s);
        const auto refusalStatus = refusalService.status();
        if (refusalTerminal && refusalStatus.state == GpuPreviewDisplayServiceState::Ready &&
            refusalStatus.presentationClient != nullptr) {
            const auto beforeRefusal = refusalStatus.counters;
            const auto refusalIdentity = makeIdentity(*neutralPlan, 1, {}, {}, ViewAdjust{});
            auto refusalSubmission = refusalService.submit(TaskRequest("explicit refusal", owner),
                                                           snapshot, refusalIdentity, kBudget, {});
            const auto refusalResult = awaitResult(refusalSubmission.handle, 60s);
            const auto afterRefusal = refusalService.status().counters;
            expectations.expect(afterRefusal.gpuAdmissionRefusals >
                                    beforeRefusal.gpuAdmissionRefusals,
                                "explicit refusal: the admission refusal counter incremented");
            std::shared_ptr<const bloom::runtime::PreparedPreviewFrame> refusalFrame;
            expectations.expect(
                !isResidentPrepared(refusalResult, 1, refusalFrame) && refusalResult.has_value() &&
                    refusalResult->state() == TaskState::Succeeded,
                "explicit refusal: the honest CPU path completed with no fabricated "
                "GPU frame");
        }
        refusalService.beginShutdown();
        static_cast<void>(waitUntil(
            [&] { return refusalService.status().state == GpuPreviewDisplayServiceState::Stopped; },
            60s));
        refusalScheduler.beginShutdown();
        static_cast<void>(waitUntil([&] { return refusalScheduler.isQuiescent(); }, 30s));
    }

    if (expectations.failures() == 0) {
        std::cout
            << "PASS: default >4K Neutral general arm at the service; non-default adjusted "
               "cold/warm general frames; process output retained across a display-only change; "
               "zero full-frame readback; production-default scheduler admits a 6K resident "
               "request "
               "with zero admission refusal; media+text 6K production graph dispatches natively "
               "with "
               "zero CPU fallback; explicit small capacity refuses honestly\n";
        return 0;
    }
    std::cerr << expectations.failures() << " general display service expectation(s) failed\n";
    return 1;
#endif
}
