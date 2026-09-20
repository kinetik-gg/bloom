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
    auto acesResolution = bloom::color::resolveOcioBuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri,
        *acesRevision, "ACEScg");
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
    ocioRequest.toolPackage.glslangStagedDigest =
        bloom::core::Sha256Digest::fromLowercaseHex(
            std::string_view{BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256}.substr(7));
#endif
#ifdef BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256
    ocioRequest.toolPackage.spirvValStagedDigest =
        bloom::core::Sha256Digest::fromLowercaseHex(
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
        [&] {
            return service.status().state != GpuPreviewDisplayServiceState::Initializing;
        },
        90s);
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
    const auto neutralIdentity =
        makeIdentity(*neutralPlan, 1, {}, {}, ViewAdjust{});
    auto neutralSubmission =
        service.submit(TaskRequest("general neutral", owner), snapshot, neutralIdentity, kBudget, {});
    expectations.expect(neutralSubmission.status == TaskSubmissionStatus::Accepted,
                        "the >4K default Neutral general request was admitted");
    const auto neutralResult = awaitResult(neutralSubmission.handle);
    std::shared_ptr<const bloom::runtime::PreparedPreviewFrame> neutralFrame;
    const bool neutralResident = isResidentPrepared(neutralResult, 1, neutralFrame);
    if (!neutralResident) {
        std::cerr << "neutral >4K general request did not produce a resident frame: "
                  << service.status().residentDetail << '\n';
    }
    expectations.expect(neutralResident,
                        "the >4K default Neutral request produced a resident GpuResident frame at the "
                        "actual service (old 4K/pixel-interval gate gone)");
    expectations.expect(
        bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::generalDisplayActive(*core),
        "the service took the prepared general display arm for the default Neutral request");
    expectations.expect(
        neutralFrame != nullptr && neutralFrame->residentFrame() != nullptr &&
            neutralFrame->residentFrame()->lease().isValid(),
        "the general frame carries a valid resident lease");
    expectations.expect(
        bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::generalDisplayIdentity(*core) !=
            bloom::core::Sha256Digest{},
        "the service retained the exact general display command identity");
    const auto afterNeutral = service.status().counters;
    expectations.expect(afterNeutral.fullFrameReadbacks == 0U,
                        "the resident general route performed no full-frame readback");
    expectations.expect(afterNeutral.displayStatusReads >= 1U,
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
    expectations.expect(coldResident,
                        "the non-default display + adjustment produced a resident GpuResident frame");
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

    // Display-only change: same process scene, a different adjustment -> the process output is
    // retained and only the DisplayRgba8 command identity changes.
    auto changeSubmission =
        service.submit(TaskRequest("general aces display-only", owner), snapshot, identityB, kBudget,
                       {});
    expectations.expect(changeSubmission.status == TaskSubmissionStatus::Accepted,
                        "the display-only change request was admitted");
    const auto changeResult = awaitResult(changeSubmission.handle);
    std::shared_ptr<const bloom::runtime::PreparedPreviewFrame> changeFrame;
    expectations.expect(isResidentPrepared(changeResult, 2, changeFrame),
                        "the display-only change produced a resident frame");
    const auto afterChange = service.status().counters;
    expectations.expect(afterChange.gpuCacheHits > afterWarm.gpuCacheHits,
                        "the display-only change retained the process output (scene cache hit)");
    expectations.expect(afterChange.nativeDispatches == afterWarm.nativeDispatches,
                        "the display-only change performed zero additional native scene operations");
    expectations.expect(
        bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::generalDisplayIdentity(*core) !=
            identityACommand,
        "a changed adjustment changed the exact DisplayRgba8 command identity");
    expectations.expect(afterChange.fullFrameReadbacks == 0U,
                        "the whole general service run performed no full-frame readback");

    service.beginShutdown();
    expectations.expect(waitUntil(
                            [&] {
                                return service.status().state ==
                                       GpuPreviewDisplayServiceState::Stopped;
                            },
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
            forcedScheduler,
            generalStageFunction(neutralPlan, builder, programService, processor),
            generalCpuStageFunction(neutralPlan, processor, evaluator), generalFallback(processor),
            forcedOptions);
        const bool forcedTerminal = waitUntil(
            [&] {
                return forcedService.status().state !=
                       GpuPreviewDisplayServiceState::Initializing;
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
            auto forcedSubmission = forcedService.submit(
                TaskRequest("forced neutral 4k", owner), snapshot, neutralIdentity, kBudget, {});
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

    if (expectations.failures() == 0) {
        std::cout << "PASS: default >4K Neutral general arm at the service; non-default adjusted "
                     "cold/warm general frames; process output retained across a display-only change; "
                     "zero full-frame readback\n";
        return 0;
    }
    std::cerr << expectations.failures() << " general display service expectation(s) failed\n";
    return 1;
#endif
}
