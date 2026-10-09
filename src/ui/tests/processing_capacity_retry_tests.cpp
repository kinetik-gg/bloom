#include "ram_preview_pipeline_test_support.hpp"

#include <bloom/render/packed_rgba8_storage.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>

#include <QApplication>
#include <QTimer>

#include <exception>

namespace {
using namespace bloom;
using namespace bloom::ui::ram_pipeline_test;
constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;

runtime::MemoryBudgetState capacity(const std::uint64_t serial,
                                    const std::uint64_t limit = 8 * gib) {
    runtime::MemoryBudgetState state;
    state.machine.availableBytes = 60 * gib;
    state.process.source = runtime::ProcessMemorySource::LinuxCgroupV2;
    state.process.status = runtime::ProcessMemoryStatus::Finite;
    state.process.sampledAt = runtime::GpuProcessingPhasePolicy::Clock::now();
    state.process.domains.push_back({.path = "/fixture",
                                     .kind = runtime::ProcessMemoryLimitKind::Finite,
                                     .limitBytes = limit,
                                     .currentBytes = 0});
    state.processSampleFresh = true;
    state.sampleSerial = serial;
    return state;
}
void run(Expectations& checks) {
    auto project = makeTestProject("Capacity retry", timeAt(10));
    document::Document document(std::move(project.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, project.initialCompositionId);
    checks.expect(session.addSolidLayer("Solid", {0.25, 0.5, 0.75, 1}), "real solid authored");
    PipelineFixture fixture;
    auto policy = std::make_shared<runtime::GpuProcessingPhasePolicy>(1);
    runtime::TaskScheduler scheduler(twoWorkerConfig());
    ui::TaskUiBridge bridge(scheduler);
    std::atomic_size_t heavy{0};
    const auto actualStage = ui::makeCompositionPreviewCpuStage(
        fixture.compiler, fixture.evaluator, fixture.qualifiedProcessorProvider, fixture.planCache);
    runtime::GpuPreviewDisplayServiceOptions options;
    options.enabled = false;
    options.processingPhasePolicy = policy;
    runtime::GpuPreviewDisplayService service(
        scheduler,
        [actualStage, &heavy](const document::Snapshot& snapshot,
                              const runtime::PreviewRequestIdentity& identity, std::size_t limit,
                              const std::vector<runtime::SnapshotParameterOverride>& overrides,
                              runtime::TaskContext& context) {
            heavy.fetch_add(1);
            return actualStage(snapshot, identity, limit, overrides, context);
        },
        ui::makeCompositionPreviewCpuDisplayFallback(fixture.displayPreparer), options);
    auto cache = std::make_shared<ui::PreviewFrameCache>(64 * 1024 * 1024);
    std::atomic_size_t submissions{0};
    std::vector<runtime::SnapshotParameterOverride> submittedOverrides;
    const ui::PreviewPreparationSubmitter submitter =
        [&service, &submissions,
         &submittedOverrides](runtime::TaskRequest request, const document::Snapshot& snapshot,
                              const runtime::PreviewRequestIdentity& identity, std::size_t limit,
                              const std::vector<runtime::SnapshotParameterOverride>& overrides) {
            submissions.fetch_add(1);
            submittedOverrides = overrides;
            return service.submit(std::move(request), snapshot, identity, limit, overrides);
        };
    ui::CompositionPreviewSettings settings;
    settings.resolutionPolicy = runtime::PreviewResolutionPolicy::Full;
    const auto pipeline = ui::makeCompositionPreviewPipeline(
        fixture.compiler, fixture.evaluator, fixture.displayPreparer,
        fixture.qualifiedProcessorProvider, fixture.planCache);
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline, settings,
                                                cache, nullptr, submitter, {}, policy);
    const auto failed = [&] { return controller.state().activity == ui::PreviewActivity::Failed; };
    const auto readyAt = [&](const core::RationalTime time) {
        return controller.state().activity == ui::PreviewActivity::Ready &&
               controller.state().frame &&
               controller.state().freshness == ui::FrameFreshness::Current &&
               controller.state().frame->desiredIdentity().time == time;
    };
    checks.expect(waitUntil(failed) && heavy.load() == 0 && submissions.load() == 1 &&
                      !controller.state().diagnostics.empty() &&
                      controller.state().diagnostics.front().admissionRetry,
                  "cold capacity refuses before actual CPU callback");
    if (checks.failures() != 0)
        return;
    checks.expect(session.setCurrentTime(timeAt(2)) && session.setCurrentTime(timeAt(3)),
                  "newest cold intent selected");
    checks.expect(waitUntil(failed) && heavy.load() == 0,
                  "cold newest intent remains bounded and no evaluation starts");
    auto sample = capacity(1);
    policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    // Represents the existing sampleApplied notification after the root updates its policy.
    controller.notifyProcessingCapacityChanged();
    checks.expect(waitUntil([&] { return readyAt(timeAt(3)); }) && heavy.load() == 1,
                  "sample-ready retries only the newest live intent through the real submitter");
    if (checks.failures() != 0)
        return;
    const auto oldFrame = controller.state().frame;
    // Native zero belongs to another route. This disabled CPU root still recovers using host only.
    checks.expect(
        policy->reportDevice(1, {.source = render::GpuAllocationBudgetSource::MemoryBudget,
                                 .device_local_bytes = gib,
                                 .available_bytes = 0}),
        "known native zero injected");
    auto held = runtime::reserveHostEvaluationPhase(
        *policy, 16ULL * 12, gib, runtime::GpuProcessingPhaseOutput::FloatFinal, true);
    checks.expect(held.lease != nullptr, "one competing phase admitted");
    auto release = std::make_shared<std::atomic_bool>(false);
    struct ReleaseOnExit {
        std::shared_ptr<std::atomic_bool> flag;
        ~ReleaseOnExit() { flag->store(true); }
    } releaseOnExit{release};
    std::atomic_bool entered{false};
    runtime::TaskRequest heldRequest(
        "competing phase",
        {.kind = runtime::TaskOwnerKind::Composition, .id = runtime::TaskOwnerId::fromRaw(100)});
    auto competitor =
        scheduler.submit<int>(std::move(heldRequest), [lease = std::move(held.lease), release,
                                                       &entered](runtime::TaskContext&) mutable {
            entered.store(true);
            while (!release->load())
                std::this_thread::yield();
            lease.reset(); // Last release precedes the existing terminal completion wake.
            return runtime::TaskResult<int>::succeeded(1);
        });
    checks.expect(competitor.accepted() && waitUntil([&] { return entered.load(); }),
                  "real task holds phase");
    checks.expect(session.setCurrentTime(timeAt(4)), "pressure intent requested");
    checks.expect(waitUntil(failed) && controller.state().frame == oldFrame && heavy.load() == 1,
                  "contention preserves last good pixels without entering evaluation");
    bridge.stop();
    const auto before = submissions.load();
    int pulses = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        ++pulses;
        controller.notifyProcessingCapacityChanged();
    });
    heartbeat.start(10);
    QElapsedTimer span;
    span.start();
    checks.expect(waitUntil([&] { return span.elapsed() >= 150 && pulses >= 4; }) &&
                      submissions.load() == before && !bridge.isRunning(),
                  "held span crosses Jobs interval with GUI pulses and no retry spin");
    heartbeat.stop();
    release->store(true);
    checks.expect(waitUntil([&] { return readyAt(timeAt(4)); }) && heavy.load() == 2,
                  "completion-driven external release retries CPU route despite native zero");
    sample = capacity(2, 0);
    policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    controller.requestRefresh();
    checks.expect(waitUntil(failed), "known host zero gives terminal refusal");
    const auto zeroSubmissions = submissions.load();
    for (int i = 0; i < 20; ++i)
        controller.notifyProcessingCapacityChanged();
    QCoreApplication::processEvents();
    checks.expect(submissions.load() == zeroSubmissions && heavy.load() == 2 &&
                      controller.state().frame,
                  "true measured zero never creates a retry loop or removes good pixels");
    sample = capacity(3);
    policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    controller.notifyProcessingCapacityChanged();
    checks.expect(waitUntil([&] { return readyAt(timeAt(4)); }) && heavy.load() == 3,
                  "fresh usable capacity recovers the latest refused intent");
    const auto warmHeavy = heavy.load();
    sample = capacity(4, 9 * gib);
    policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    controller.notifyProcessingCapacityChanged();
    checks.expect(session.setCurrentTime(timeAt(3)) && readyAt(timeAt(3)) &&
                      heavy.load() == warmHeavy,
                  "capacity revision leaves exact warm cache seek free of evaluation");
    auto gestureHeld = runtime::reserveHostEvaluationPhase(
        *policy, 16ULL * 12, gib, runtime::GpuProcessingPhaseOutput::FloatFinal, true);
    checks.expect(gestureHeld.lease != nullptr, "gesture fixture holds the competing phase");
    const auto format = session.composition()->format();
    const auto window = render::ImageWindow::create(0, 0, format.width(), format.height());
    const auto descriptor =
        render::ReferenceDisplayBufferDescriptor::create(*window.value(), format.pixelAspect());
    const ui::ViewerMapping mapping{
        .displayRect = QRectF(0, 0, 160, 120),
        .compositionFormat = format,
        .resolution = runtime::CompositionFormatResolution{},
        .pixelAspect = format.pixelAspect(),
        .displayDescriptor = *descriptor.value(),
    };
    controller.beginInteractiveScrub();
    checks.expect(!session.beginTransformInteraction({}, mapping), "selected solid gesture begins");
    session.updateTransformInteraction(QPointF(10, 0));
    session.updateTransformInteraction(QPointF(30, 0));
    checks.expect(waitUntil(failed), "newest live transform reaches real capacity refusal");
    const auto newestOverrides = session.transformInteractionOverrides();
    const auto gestureInsertions = cache->statistics().insertions;
    gestureHeld.lease.reset();
    controller.notifyProcessingCapacityChanged();
    checks.expect(
        waitUntil([&] { return readyAt(timeAt(3)); }) && !newestOverrides.empty() &&
            submittedOverrides == newestOverrides && heavy.load() == warmHeavy + 1 &&
            cache->statistics().insertions == gestureInsertions,
        "release retries newest live transform through actual stage without committed caching");
    controller.beginShutdown();
    const auto stoppedSubmissions = submissions.load();
    sample = capacity(5, 10 * gib);
    policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    controller.notifyProcessingCapacityChanged();
    checks.expect(submissions.load() == stoppedSubmissions,
                  "shutdown cannot rearm a deferred request");
    service.beginShutdown();
    bridge.beginShutdown();
    checks.expect(waitUntil([&] { return scheduler.isQuiescent(); }), "all owned tasks quiesce");
    std::cout << "retry proof: submissions=" << submissions.load()
              << " actualPreparations=" << heavy.load() << " heldGuiPulses=" << pulses << "\n";
}

// This is a CPU/control fixture for the real controller and neutral admission state. Genuine
// rooted GPU pixels/publication are independently exercised by the packed native service test.
void packedProvisionalRetry(Expectations& checks) {
    auto project = makeTestProject("Packed ownership retry", timeAt(10));
    document::Document document(std::move(project.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, project.initialCompositionId);
    checks.expect(session.addSolidLayer("Solid", {0.25, 0.5, 0.75, 1}), "retry solid authored");
    PipelineFixture fixture;
    auto policy = std::make_shared<runtime::GpuProcessingPhasePolicy>(1);
    auto sample = capacity(1);
    policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    checks.expect(
        policy->reportDevice(101, {.source = render::GpuAllocationBudgetSource::MemoryBudget,
                                   .device_local_bytes = gib,
                                   .available_bytes = 0}),
        "packed host retry has irrelevant native zero");
    runtime::TaskScheduler scheduler(twoWorkerConfig());
    ui::TaskUiBridge bridge(scheduler);
    const auto pipeline = ui::makeCompositionPreviewPipeline(
        fixture.compiler, fixture.evaluator, fixture.displayPreparer,
        fixture.qualifiedProcessorProvider, fixture.planCache);
    auto external = std::make_shared<render::PackedRgba8Storage>();
    const auto prepareExternal = [&] {
        auto phase = runtime::reserveHostEvaluationPhase(
            *policy, 192, 128ULL * 1024, runtime::GpuProcessingPhaseOutput::PackedPreview);
        if (!phase.lease)
            return false;
        auto storage = render::PackedRgba8Storage::create(32, phase.lease, 128);
        if (!storage.hasValue() || !storage.value()->promote())
            return false;
        *external = std::move(*storage.value());
        return true;
    };
    std::atomic_bool faultNext{false}, releaseBeforeDelivery{false}, arithmeticValid{true};
    std::atomic_size_t submissions{0}, actualPreparations{0}, failuresAfterAllocation{0};
    ui::PreviewPreparationFunction preparation =
        [&, pipeline](const document::Snapshot& snapshot,
                      const runtime::PreviewRequestIdentity& identity, const std::size_t bytes,
                      const std::vector<runtime::SnapshotParameterOverride>& overrides,
                      runtime::TaskContext& context) {
            using Result = runtime::TaskResult<runtime::PreviewPreparationResultHandle>;
            ++submissions;
            auto phase = runtime::reserveHostEvaluationPhase(
                *policy, 192, bytes, runtime::GpuProcessingPhaseOutput::PackedPreview);
            if (!phase.lease) {
                arithmeticValid = false;
                return Result::failed({{.code = "fixture.phase-admission",
                                        .severity = runtime::DiagnosticSeverity::Error,
                                        .summary = "Expected host phase was not admitted.",
                                        .detail = {},
                                        .suggestedAction = {}}});
            }
            if (!faultNext.exchange(false)) {
                ++actualPreparations;
                return pipeline(snapshot, identity, bytes, overrides, context);
            }
            const auto before = policy->snapshot().reservations;
            auto allocated = render::PackedRgba8Storage::create(32, phase.lease, 128);
            if (!allocated.hasValue()) {
                arithmeticValid = false;
                return Result::failed(
                    {{.code = "fixture.packed-allocation",
                      .severity = runtime::DiagnosticSeverity::Error,
                      .summary = "Expected provisional storage was not allocated.",
                      .detail = {},
                      .suggestedAction = {}}});
            }
            // This real count refusal occurs AFTER the actual bounded allocator request.
            const auto refused =
                policy->reserve({.pixels = 1, .sourceFree = true, .native = false});
            const auto revision = phase.lease->revisionAfterOwnRelease();
            arithmeticValid =
                arithmeticValid &&
                refused.status == render::GpuProcessingPhaseAdmission::BudgetRefused &&
                refused.domain == render::GpuProcessingPhaseDomain::RequestCount &&
                policy->snapshot().reservations.ownedPackedHostBytes ==
                    before.ownedPackedHostBytes + allocated.value()->byteCost();
            phase.lease.reset();
            arithmeticValid =
                arithmeticValid && policy->snapshot().reservations.admittedPhases == 1;
            allocated.value()->reset(); // Fold back, then exactly one origin-phase release.
            const auto after = policy->snapshot().reservations;
            arithmeticValid = arithmeticValid && after.admissionRevision == revision &&
                              after.admittedPhases == 0 && after.hostReservedBytes == 0 &&
                              after.ownedPackedHostBytes == before.ownedPackedHostBytes;
            if (releaseBeforeDelivery.exchange(false))
                external->reset();
            ++failuresAfterAllocation;
            return Result::failed(
                {{.code = "bloom.preview.gpu-scene.budget-refused",
                  .severity = runtime::DiagnosticSeverity::Error,
                  .summary = "Real count refusal after provisional allocation.",
                  .detail = "Controller control witness; no GPU pixel claim.",
                  .suggestedAction = "Wait for an independent capacity release.",
                  .admissionRetry = runtime::TaskAdmissionRetry{revision, false}}});
        };
    ui::CompositionPreviewSettings settings;
    settings.resolutionPolicy = runtime::PreviewResolutionPolicy::Full;
    ui::CompositionPreviewController controller(session, scheduler, bridge, preparation, settings,
                                                {}, nullptr, {}, {}, policy);
    const auto ready = [&] {
        return controller.state().activity == ui::PreviewActivity::Ready &&
               controller.state().freshness == ui::FrameFreshness::Current &&
               controller.state().frame;
    };
    checks.expect(waitUntil(ready) && actualPreparations == 1, "real CPU last-good frame reached");
    if (checks.failures())
        return;
    const auto oldFrame = controller.state().frame;
    checks.expect(prepareExternal(), "published external host owner prepared without phase count");
    faultNext = true;
    controller.requestRefresh();
    checks.expect(
        waitUntil([&] { return controller.state().activity == ui::PreviewActivity::Failed; }) &&
            failuresAfterAllocation == 1 && arithmeticValid && controller.state().frame == oldFrame,
        "typed refusal after allocated storage preserves last good frame and exact R+1");
    const auto failedSubmissions = submissions.load();
    QElapsedTimer held;
    held.start();
    int pulses = 0;
    checks.expect(waitUntil([&] {
                      controller.notifyProcessingCapacityChanged();
                      ++pulses;
                      return held.elapsed() >= 150;
                  }) &&
                      submissions == failedSubmissions && actualPreparations == 1,
                  "own provisional cleanup cannot spin controller across held GUI pulses");
    external->reset();
    controller.notifyProcessingCapacityChanged();
    checks.expect(waitUntil(ready) && submissions == failedSubmissions + 1 &&
                      actualPreparations == 2,
                  "external host last release wakes CPU recovery once despite device zero");
    checks.expect(prepareExternal(), "second independent published host owner prepared");
    faultNext = true;
    releaseBeforeDelivery = true;
    controller.requestRefresh();
    // Deliberately hold UI delivery. The worker's external release precedes terminal drain.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((failuresAfterAllocation != 2 || !scheduler.isQuiescent()) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    checks.expect(failuresAfterAllocation == 2 && scheduler.isQuiescent() && arithmeticValid,
                  "real failed task and interleaved release complete before UI delivery");
    checks.expect(waitUntil(ready) && submissions == failedSubmissions + 3 &&
                      actualPreparations == 3,
                  "external release before refusal delivery remains visible for one newest retry");
    const auto final = policy->snapshot().reservations;
    checks.expect(final.admittedPhases == 0 && final.hostReservedBytes == 0 &&
                      final.ownedPackedHostBytes == 0 && final.totalHostClaimBytes == 0,
                  "controller proof leaves no phase or packed allocation claims");
    controller.beginShutdown();
    bridge.beginShutdown();
    checks.expect(waitUntil([&] { return scheduler.isQuiescent(); }), "packed retry tasks quiesce");
    std::cout << "packed provisional controller: submissions=" << submissions
              << " failuresAfterAllocation=" << failuresAfterAllocation
              << " actualCpuPreparations=" << actualPreparations << " heldPulses=" << pulses
              << '\n';
}
} // namespace
int main(int argc, char** argv) {
    try {
        QApplication application(argc, argv);
        Expectations checks;
        run(checks);
        if (!checks.failures())
            packedProvisionalRetry(checks);
        return checks.failures() == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
