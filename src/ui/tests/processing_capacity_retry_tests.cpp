#include "ram_preview_pipeline_test_support.hpp"

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
} // namespace
int main(int argc, char** argv) {
    try {
        QApplication application(argc, argv);
        Expectations checks;
        run(checks);
        return checks.failures() == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
