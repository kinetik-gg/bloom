// Real production scene -> native display -> registry refusal/recovery, without a desktop window.
#include "gpu_service_chain_fixture.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QGuiApplication>
#include <QTemporaryDir>

#include <iostream>
#include <memory>

using namespace bloom;
using namespace bloom::ui::verticalproof;
using namespace std::chrono_literals;

#include "gpu_processing_phase_retry_native.ipp"

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    const auto parsed = parseOptions(argc, argv);
    if (!parsed.valid)
        return 2;
    QGuiApplication application(argc, argv);
    if (!QGuiApplication::platformName().startsWith("wayland")) {
        std::cout << "SKIP: native phase recovery requires private Wayland\n";
        return parsed.require_device ? 1 : 77;
    }
    QTemporaryDir temporary;
    Expectations checks;
    Fixture fixture(temporary.path().toStdString());
    auto project = document::makeNewProject(
        "Phase recovery", "Main", core::RationalTime::fromInteger(4), format1920x1080NonSquare());
    document::Document document(std::move(project.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, project.initialCompositionId);
    checks.expect(session.addSolidLayer("Solid", {0.125, 0.25, 0.5, 1.0}),
                  "ordinary 1080p solid is authored");
    checks.expect(session.addTextLayer(QStringLiteral("Text"), QStringLiteral("Phase recovery")),
                  "ordinary 1080p text is authored");
    const auto policy = std::make_shared<runtime::GpuProcessingPhasePolicy>(1);
    constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
    runtime::MemoryBudgetState sample;
    sample.machine.availableBytes = 60 * gib;
    sample.process.status = runtime::ProcessMemoryStatus::Finite;
    sample.process.source = runtime::ProcessMemorySource::LinuxCgroupV2;
    sample.process.sampledAt = runtime::GpuProcessingPhasePolicy::Clock::now();
    sample.process.domains.push_back({.path = "/fixture",
                                      .kind = runtime::ProcessMemoryLimitKind::Finite,
                                      .limitBytes = 8 * gib,
                                      .currentBytes = 0});
    sample.processSampleFresh = true;
    sample.sampleSerial = 1;
    policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    const runtime::GpuScenePreparationPhase phase{
        .policy = policy,
        .envelope = {.metadataBytes = 1024ULL * 1024,
                     .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                     .requestedExecutorBytes = kBudget,
                     .deviceWorkingBytes = 832ULL * 1024 * 1024}};
    fixture.sceneBuilder = std::make_shared<runtime::CpuGpuSceneBuilder>(
        nullptr, runtime::GpuSceneMediaContext::fromEvaluator(fixture.evaluator),
        runtime::GpuSceneOcioContext{}, phase);
    TaskScheduler scheduler(schedulerConfig());
    const runtime::CpuReferenceDisplayPreparer referenceDisplay;
    // Early disabled/unavailable CPU roots must reserve before their heavy callback, independently
    // of native startup. The same root/count is shared with the native producer below.
    auto disabledOptions = serviceOptions(parsed.loader_path);
    disabledOptions.enabled = false;
    disabledOptions.processingPhasePolicy = policy;
    std::atomic_size_t cpuPreparations{0};
    const auto actualCpuStage = fixture.productionCpuStage();
    GpuPreviewDisplayService disabledService(
        scheduler,
        [&cpuPreparations,
         actualCpuStage](const document::Snapshot& snapshot, const PreviewRequestIdentity& identity,
                         const std::size_t limit,
                         const std::vector<runtime::SnapshotParameterOverride>& overrides,
                         TaskContext& context) {
            cpuPreparations.fetch_add(1, std::memory_order_relaxed);
            return actualCpuStage(snapshot, identity, limit, overrides, context);
        },
        fixture.productionCpuFallback(referenceDisplay), disabledOptions);
    const auto cpuSnapshot = session.snapshot();
    const auto cpuIdentity = makeIdentity(100, cpuSnapshot, session.compositionId());
    const TaskOwner cpuOwner{.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(8)};
    auto heldHost = runtime::reserveHostEvaluationPhase(
        *policy, 1920ULL * 1080, kBudget, runtime::GpuProcessingPhaseOutput::FloatFinal, true);
    checks.expect(heldHost.lease != nullptr, "CPU fixture holds one shared competing phase");
    auto deniedCpu = disabledService.submit(TaskRequest("CPU phase refusal", cpuOwner), cpuSnapshot,
                                            cpuIdentity, kBudget, {});
    auto deniedCpuResult = deniedCpu.accepted() ? awaitResult(deniedCpu.handle) : std::nullopt;
    checks.expect(deniedCpuResult && deniedCpuResult->state() == TaskState::Failed &&
                      cpuPreparations.load(std::memory_order_relaxed) == 0,
                  "disabled CPU root refuses shared contention before evaluation callback");
    heldHost.lease.reset();
    auto acceptedCpu = disabledService.submit(TaskRequest("CPU phase recovery", cpuOwner),
                                              cpuSnapshot, cpuIdentity, kBudget, {});
    auto acceptedCpuResult =
        acceptedCpu.accepted() ? awaitResult(acceptedCpu.handle) : std::nullopt;
    const auto cpuFrame = acceptedCpuResult ? residentFrameOf(*acceptedCpuResult) : nullptr;
    checks.expect(
        cpuFrame && hasCpuPixels(cpuFrame) && !isResident(cpuFrame) &&
            cpuPreparations.load(std::memory_order_relaxed) == 1 &&
            policy->snapshot().reservations.admittedPhases == 0,
        "capacity release admits genuine 1080p solid/text CPU fallback and releases phase");
    acceptedCpuResult.reset();
    disabledService.beginShutdown();
    { // Release this completed owner slot before constructing the resident service below.
        auto legacyOptions = serviceOptions(parsed.loader_path);
        legacyOptions.presentation = GpuPreviewDisplayServicePresentationMode::Disabled;
        legacyOptions.processingPhasePolicy = policy;
        std::atomic_size_t legacyPreparations{0};
        GpuPreviewDisplayService legacyService(
            scheduler,
            [&legacyPreparations,
             actualCpuStage](const document::Snapshot& snapshot,
                             const PreviewRequestIdentity& identity, const std::size_t limit,
                             const std::vector<runtime::SnapshotParameterOverride>& overrides,
                             TaskContext& context) {
                legacyPreparations.fetch_add(1, std::memory_order_relaxed);
                return actualCpuStage(snapshot, identity, limit, overrides, context);
            },
            fixture.productionCpuFallback(referenceDisplay), legacyOptions);
        checks.expect(waitUntil(
                          [&] {
                              return legacyService.status().state !=
                                     GpuPreviewDisplayServiceState::Initializing;
                          },
                          90s) &&
                          legacyService.status().state == GpuPreviewDisplayServiceState::Ready,
                      "legacy native owner reports capacity independently before CPU preparation");
        sample.process.sampledAt = runtime::GpuProcessingPhasePolicy::Clock::now();
        sample.sampleSerial += 1;
        policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
        auto legacyHeld = runtime::reserveHostEvaluationPhase(
            *policy, 1920ULL * 1080, kBudget, runtime::GpuProcessingPhaseOutput::FloatFinal, true);
        checks.expect(legacyHeld.lease != nullptr, "legacy fixture holds competing phase");
        auto legacyDenied = legacyService.submit(TaskRequest("legacy paired refusal", cpuOwner),
                                                 cpuSnapshot, cpuIdentity, kBudget, {});
        auto legacyDeniedResult =
            legacyDenied.accepted() ? awaitResult(legacyDenied.handle) : std::nullopt;
        checks.expect(legacyDeniedResult && legacyDeniedResult->state() == TaskState::Failed &&
                          legacyPreparations.load(std::memory_order_relaxed) == 0,
                      "legacy paired contention refuses before CPU callback");
        legacyHeld.lease.reset();
        auto legacyAccepted = legacyService.submit(TaskRequest("legacy paired recovery", cpuOwner),
                                                   cpuSnapshot, cpuIdentity, kBudget, {});
        auto legacyResult =
            legacyAccepted.accepted() ? awaitResult(legacyAccepted.handle) : std::nullopt;
        const auto legacyFrame = legacyResult ? residentFrameOf(*legacyResult) : nullptr;
        checks.expect(legacyFrame && hasCpuPixels(legacyFrame) &&
                          legacyFrame->provenance().provider ==
                              runtime::PreviewDisplayProvider::GpuNeutral &&
                          legacyPreparations.load(std::memory_order_relaxed) == 1,
                      "released phase admits real1080 CPU-prepared pixels with genuine Neutral "
                      "native display");
        checks.expect(
            waitUntil([&] { return policy->snapshot().reservations.admittedPhases == 0; }, 15s) &&
                policy->snapshot().reservations.unprovenPhases == 0,
            "legacy fence-proven phase releases after stage storage disposal");
        const auto legacyCore =
            runtime::detail::GpuPreviewDisplayServiceTestAccess::coreOf(legacyService);
        for (const int phaseFault : {1, 3}) {
            legacyCore->testNativeExceptionPhase.store(phaseFault, std::memory_order_release);
            auto faultSubmission = legacyService.submit(TaskRequest("legacy exception", cpuOwner),
                                                        cpuSnapshot, cpuIdentity, kBudget, {});
            auto faultResult =
                faultSubmission.accepted() ? awaitResult(faultSubmission.handle) : std::nullopt;
            checks.expect(
                faultResult && faultResult->state() == TaskState::Failed &&
                    waitUntil([&] { return policy->snapshot().reservations.admittedPhases == 0; },
                              15s),
                "pre-begin/post-Ready exceptions terminally fail without stranded phase");
            auto healthySubmission =
                legacyService.submit(TaskRequest("legacy exception recovery", cpuOwner),
                                     cpuSnapshot, cpuIdentity, kBudget, {});
            auto healthyResult =
                healthySubmission.accepted() ? awaitResult(healthySubmission.handle) : std::nullopt;
            const auto healthyFrame = healthyResult ? residentFrameOf(*healthyResult) : nullptr;
            checks.expect(healthyFrame && healthyFrame->provenance().provider ==
                                              runtime::PreviewDisplayProvider::GpuNeutral,
                          "native generation remains usable after proven exception cleanup");
        }
        legacyCore->testNativeExceptionPhase.store(2, std::memory_order_release);
        auto unknownSubmission =
            legacyService.submit(TaskRequest("legacy submitted exception", cpuOwner), cpuSnapshot,
                                 cpuIdentity, kBudget, {});
        auto unknownResult =
            unknownSubmission.accepted() ? awaitResult(unknownSubmission.handle) : std::nullopt;
        checks.expect(
            unknownResult && unknownResult->state() == TaskState::Failed &&
                policy->snapshot().reservations.admittedPhases == 1 &&
                policy->snapshot().reservations.unprovenPhases == 1,
            "post-submit exception terminates stage but retains exact unknown generation quota");
        legacyService.beginShutdown();
        checks.expect(waitUntil(
                          [&] {
                              return legacyService.status().state ==
                                     GpuPreviewDisplayServiceState::Stopped;
                          },
                          15s),
                      "legacy native service retires before resident fixture");
    }
    checks.expect(policy->snapshot().reservations.admittedPhases == 0,
                  "destroyed legacy allocator generation releases conservative unknown quota");
    retryNativeChecks(checks, fixture, session, scheduler, parsed.loader_path, referenceDisplay);
    if (checks.failures() != 0)
        return 1;
    auto options = serviceOptions(parsed.loader_path);
    options.processingPhasePolicy = policy;
    // Exactly one Full packed-size native image fits. A second live publication must refuse.
    options.residentLeaseBudgets.maxBytes = 12ULL * 1024 * 1024;
    GpuPreviewDisplayService service(scheduler, fixture.productionGpuStage(),
                                     fixture.productionCpuStage(),
                                     fixture.productionCpuFallback(referenceDisplay), options);
    checks.expect(
        waitUntil(
            [&] { return service.status().state != GpuPreviewDisplayServiceState::Initializing; },
            90s) &&
            service.status().state == GpuPreviewDisplayServiceState::Ready,
        "cold owner reports capacity and reaches genuine native readiness");
    if (service.status().state != GpuPreviewDisplayServiceState::Ready) {
        std::cerr << service.status().residentDetail << '\n';
        return parsed.require_device ? 1 : 77;
    }
    sample.process.sampledAt = runtime::GpuProcessingPhasePolicy::Clock::now();
    sample.sampleSerial += 1;
    policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    const auto snapshot = session.snapshot();
    TaskOwner owner;
    owner.kind = TaskOwnerKind::Composition;
    owner.id = TaskOwnerId::fromRaw(7);
    auto submit = [&](const std::uint64_t generation) {
        const auto identity = makeIdentity(generation, snapshot, session.compositionId());
        auto admitted = service.submit(TaskRequest("phase native recovery", owner), snapshot,
                                       identity, kBudget, {});
        checks.expect(admitted.accepted(), "the real request is admitted");
        if (!admitted.accepted())
            return std::optional<TaskResult<PreviewPreparationResultHandle>>{};
        return awaitResult(admitted.handle);
    };
    auto firstResult = submit(1);
    auto firstFrame = firstResult ? residentFrameOf(*firstResult) : nullptr;
    checks.expect(isResident(firstFrame), "first solid/text frame is genuinely GPU resident");
    firstResult.reset();
    const auto beforeRefusal = service.status().counters;
    auto refusedResult = submit(2);
    const auto fallback = refusedResult ? residentFrameOf(*refusedResult) : nullptr;
    checks.expect(fallback && hasCpuPixels(fallback) && !isResident(fallback),
                  "post-fence registry pressure produces an explicit CPU fallback");
    checks.expect(service.status().counters.residentGraphJobs ==
                      beforeRefusal.residentGraphJobs + 1,
                  "registry refusal follows an actual native scene and display");
    checks.expect(policy->snapshot().reservations.unprovenPhases == 0 &&
                      policy->snapshot().reservations.admittedPhases == 0,
                  "positive native retirement releases the phase despite publication refusal");
    firstFrame.reset();
    auto recoveredResult = submit(3);
    const auto recovered = recoveredResult ? residentFrameOf(*recoveredResult) : nullptr;
    checks.expect(isResident(recovered),
                  "released registry capacity admits the next genuine GPU frame");
    checks.expect(policy->snapshot().reservations.admittedPhases == 0,
                  "successful publication releases both phase quotas before terminal delivery");
    service.beginShutdown();
    checks.expect(
        waitUntil([&] { return service.status().state == GpuPreviewDisplayServiceState::Stopped; },
                  15s),
        "the native phase service retires without an unproven phase");
    if (checks.failures() != 0)
        return 1;
    std::cout << "PASS: 1080p solid/text native phase registry refusal and recovery\n";
    return 0;
}
