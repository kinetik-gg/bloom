#include "ram_preview_pipeline_test_support.hpp"

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/operation_cache.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/memory_budget_controller.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/window_status_bar.hpp>

#include <QApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <latch>
#include <memory>
#include <optional>
#include <thread>

namespace {
using namespace bloom;
using namespace std::chrono_literals;
constexpr auto gib = std::size_t{1024} * 1024 * 1024;
int failures = 0;
void expect(const bool valid, const char* message) {
    if (!valid) {
        ++failures;
        std::cerr << message << '\n';
    }
}
template <class Predicate> bool spin(Predicate done, const bool events = true) {
    const auto end = std::chrono::steady_clock::now() + 3s;
    while (!done() && std::chrono::steady_clock::now() < end) {
        if (events)
            QCoreApplication::processEvents();
        else
            std::this_thread::yield();
    }
    return done();
}
runtime::TaskSchedulerConfig config() {
    auto value = runtime::TaskSchedulerConfig::defaults();
    value.cpuWorkerCount = 1;
    value.blockingIoWorkerCount = 1;
    value.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    return value;
}
runtime::MemoryBudgetSample sample() {
    return {.machine = {.availableBytes = 60 * gib},
            .physicalBytes = 60 * gib,
            .process = {.source = runtime::ProcessMemorySource::LinuxCgroupV2,
                        .status = runtime::ProcessMemoryStatus::Finite,
                        .sampledAt = std::chrono::steady_clock::now(),
                        .membershipPath = "/fixture",
                        .domains = {{"/fixture", runtime::ProcessMemoryLimitKind::Finite, 8 * gib,
                                     6 * gib}},
                        .reclaimableBytes = {}}};
}
struct Held {
    std::atomic_bool entered = false;
    std::atomic_bool offUi = false;
    std::latch release{1};
};
void heldSampling() {
    runtime::MemoryBudgetLedger ledger(60 * gib, 60 * gib);
    ledger.beginProcessSampling();
    std::size_t budget = 1;
    const auto uiThread = std::this_thread::get_id();
    bool appliedOnUi = true;
    ledger.registerCache(
        &budget, 30 * gib, [] { return 0; },
        [&](const std::size_t value) {
            appliedOnUi = appliedOnUi && std::this_thread::get_id() == uiThread;
            budget = value;
        });
    runtime::TaskScheduler scheduler(config());
    ui::TaskUiBridge bridge(scheduler);
    bridge.stop();
    unsigned polls = 0, applied = 0;
    QObject::connect(&bridge, &ui::TaskUiBridge::snapshotsPolled, &bridge, [&] { ++polls; });
    auto state = std::make_shared<Held>();
    ui::MemoryBudgetController owner(scheduler, bridge, ledger, [state, uiThread](const auto&) {
        state->offUi = std::this_thread::get_id() != uiThread;
        state->entered = true;
        state->release.wait();
        return sample();
    });
    QObject::connect(&owner, &ui::MemoryBudgetController::sampleApplied, &owner,
                     [&] { ++applied; });
    expect(spin([&] { return state->entered.load(); }), "sample worker entered off UI");
    int pulses = 0;
    QTimer heartbeat;
    heartbeat.setInterval(1);
    QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] { ++pulses; });
    bool heldPastMonitorInterval = false;
    QTimer::singleShot(150, &heartbeat, [&] { heldPastMonitorInterval = true; });
    heartbeat.start();
    expect(spin([&] { return pulses >= 4 && heldPastMonitorInterval; }),
           "GUI heartbeat continues while sample is held");
    heartbeat.stop();
    for (int index = 0; index < 20; ++index)
        owner.requestSample();
    expect(owner.admittedSampleCount() == 1 && owner.sampleActive() && applied == 0 &&
               budget == 0 && polls == 0 && !bridge.isRunning() && state->offUi,
           "held sample has no backlog, no result/Jobs poll, no GUI I/O or cold retention");
    state->release.count_down();
    expect(spin([&] { return applied == 1; }), "completion delivers sample with Jobs stopped");
    expect(budget == 8 * gib && appliedOnUi && polls == 0 && !owner.sampleActive(),
           "cache callback applies the actual finite OS limit only on the UI thread");
    owner.beginShutdown();
    owner.requestSample();
    expect(owner.admittedSampleCount() == 1, "shutdown closes sample admission");
    scheduler.beginShutdown();
    expect(spin([&] { return scheduler.isQuiescent(); }), "sample scheduler retires");
    ledger.unregisterCache(&budget);
}
void teardown() {
    runtime::MemoryBudgetLedger ledger(60 * gib, 60 * gib);
    ledger.beginProcessSampling();
    runtime::TaskScheduler scheduler(config());
    ui::TaskUiBridge bridge(scheduler);
    bridge.stop();
    auto state = std::make_shared<Held>();
    auto owner = std::make_unique<ui::MemoryBudgetController>(scheduler, bridge, ledger,
                                                              [state](const auto&) {
                                                                  state->entered = true;
                                                                  state->release.wait();
                                                                  return sample();
                                                              });
    expect(spin([&] { return state->entered.load(); }), "teardown fixture worker entered");
    owner.reset(); // Cancels without waiting on held OS work or retaining a QObject in it.
    state->release.count_down();
    expect(spin([&] { return scheduler.isQuiescent(); }), "destroyed owner worker retires safely");
    expect(ledger.state().sampleSerial == 0 && ledger.state().effectiveBytes == 0,
           "late completion after destruction cannot publish or rearm");
    owner = std::make_unique<ui::MemoryBudgetController>(scheduler, bridge, ledger,
                                                         [](const auto&) { return sample(); });
    owner->requestSample();
    expect(spin([&] { return scheduler.isQuiescent(); }, false), "result held before GUI delivery");
    owner.reset();
    QCoreApplication::processEvents();
    expect(ledger.state().sampleSerial == 0, "queued completion cannot dereference deleted owner");
    owner = std::make_unique<ui::MemoryBudgetController>(scheduler, bridge, ledger,
                                                         [](const auto&) { return sample(); });
    QObject::connect(owner.get(), &ui::MemoryBudgetController::sampleApplied,
                     QCoreApplication::instance(), [&] { owner.reset(); });
    expect(spin([&] { return !owner; }), "synchronous completion observer may delete owner");
    scheduler.beginShutdown();
    expect(spin([&] { return scheduler.isQuiescent(); }), "teardown scheduler retires");
}
void liveSettingsAndDeliveredCapacity() {
    runtime::MemoryBudgetLedger ledger(60 * gib, 22 * gib);
    QTemporaryDir directory;
    QSettings settings(directory.filePath("live-budgets.ini"), QSettings::IniFormat);
    // Retired cache-target keys: present in old settings files, ignored by the headroom model.
    settings.setValue("playback/operation-cache-bytes", QString::number(5 * gib));
    settings.setValue("playback/ram-preview-memory-bytes", QString::number(3 * gib));
    ledger.beginProcessSampling();
    runtime::OperationCache operations(1, ledger);
    ui::PreviewFrameCache preview(1, ledger);
    const auto configured =
        ui::applyCacheMemoryBudgetsFromSettings(settings, operations, preview, ledger);
    auto view = std::make_shared<runtime::TaskBudgetView>();
    runtime::GpuProcessingPhasePolicy policy(32, {}, view);
    const auto origin = runtime::MemoryBudgetLedger::Clock::time_point{};
    const auto deliver = [&](const int seconds, const std::size_t available) {
        const auto now = origin + std::chrono::seconds(seconds);
        auto measured = sample();
        measured.machine.availableBytes = available;
        measured.process.status = runtime::ProcessMemoryStatus::Unsupported;
        measured.process.domains.clear();
        measured.process.sampledAt = now;
        const auto applied = ledger.applySample(std::move(measured), now);
        const auto allocation = ui::cacheMemoryBudgetAllocationFromSettings(settings, ledger);
        expect(
            allocation == configured && ledger.state().configuredBytes == 30 * gib &&
                operations.byteBudget() == 18 * gib && preview.byteBudget() == 12 * gib &&
                applied.processingEffectiveBytes == 60 * gib && applied.retentionPercent == 100 &&
                !applied.memoryPressure,
            "delivered free-memory changes keep the automatic pool budgets; retired keys ignored");
        policy.applyHostSample(applied, ledger.physicalByteBudget(), allocation, now);
        const auto published = view->load();
        expect(
            published && published->sampleSerial == applied.sampleSerial &&
                published->assignedOperationBytes == 18 * gib &&
                published->assignedPreviewBytes == 12 * gib &&
                published->aggregateGpuBytes == available &&
                published->previewRequestBytes == available,
            "fresh phase capacity follows actual host availability independently of cache targets");
    };
    deliver(0, 22 * gib);
    deliver(1, 10 * gib);
    deliver(2, 40 * gib);
    const auto deliveredSerial = ledger.state().sampleSerial;
    settings.setValue("playback/operation-cache-bytes", QString::number(3 * gib));
    settings.setValue("playback/ram-preview-memory-bytes", QString::number(2 * gib));
    const auto changed =
        ui::applyCacheMemoryBudgetsFromSettings(settings, operations, preview, ledger);
    expect(changed == configured && operations.byteBudget() == 18 * gib &&
               preview.byteBudget() == 12 * gib && ledger.state().configuredBytes == 30 * gib &&
               ledger.state().sampleSerial == deliveredSerial,
           "edited retired cache-target keys do not change either owner");
    settings.remove("playback/operation-cache-bytes");
    settings.remove("playback/ram-preview-memory-bytes");
    const auto automatic =
        ui::applyCacheMemoryBudgetsFromSettings(settings, operations, preview, ledger);
    expect(automatic == configured && operations.byteBudget() == 18 * gib &&
               preview.byteBudget() == 12 * gib,
           "removing overrides restores the stable hardware-based Automatic targets");
    std::size_t auxiliary = 0;
    ledger.registerCache(
        &auxiliary, 100 * gib, [] { return 0; },
        [&](const std::size_t bytes) { auxiliary = bytes; });
    expect(auxiliary == 100 * gib && operations.byteBudget() == 18 * gib &&
               preview.byteBudget() == 12 * gib,
           "an auxiliary cache cannot steal selected operation or RAM-preview budgets");
    auto finite = sample();
    finite.machine.availableBytes = 22 * gib;
    finite.process.sampledAt = origin + 3s;
    const auto constrained = ledger.applySample(finite, origin + 3s);
    policy.applyHostSample(constrained, ledger.physicalByteBudget(), automatic, origin + 3s);
    const auto published = view->load();
    expect(constrained.processCacheCeilingBytes == 8 * gib &&
               operations.byteBudget() + preview.byteBudget() + auxiliary <= 8 * gib &&
               operations.byteBudget() > 0 && preview.byteBudget() > 0 &&
               ledger.state().configuredBytes == 30 * gib &&
               settings.value("playback/operation-cache-bytes").isNull() &&
               settings.value("playback/ram-preview-memory-bytes").isNull() && published &&
               published->previewRequestBytes == 2 * gib && published->aggregateGpuBytes == 2 * gib,
           "a real finite OS limit bounds shared reusable targets while fresh paired headroom "
           "bounds work");
    ledger.unregisterCache(&auxiliary);
}

void sampledPressurePreviewRecovery() {
    using namespace ui::ram_pipeline_test;
    runtime::MemoryBudgetLedger ledger(64 * gib, 64 * gib);
    ledger.setConfiguredTotal(30 * gib);
    ledger.beginProcessSampling();
    auto project = makeTestProject("Sampled pressure preview", timeAt(10));
    document::Document document(std::move(project.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, project.initialCompositionId);
    expect(session.addSolidLayer("Solid", {0.2, 0.4, 0.8, 1}), "pressure solid authored");
    PipelineFixture fixture;
    auto view = std::make_shared<runtime::TaskBudgetView>();
    auto policy = std::make_shared<runtime::GpuProcessingPhasePolicy>(1, nullptr, view);
    auto schedulerConfig = config();
    schedulerConfig.gpuRequestOwnedByteCapacity.reset();
    schedulerConfig.budgetView = view;
    runtime::TaskScheduler scheduler(schedulerConfig);
    ui::TaskUiBridge bridge(scheduler);
    bridge.stop();
    std::size_t jobsPolls = 0;
    QObject::connect(&bridge, &ui::TaskUiBridge::snapshotsPolled, &bridge, [&] { ++jobsPolls; });
    std::atomic_size_t preparations{0};
    std::atomic_bool admittedWithinLiveBudget{true};
    const auto stage = ui::makeCompositionPreviewCpuStage(
        fixture.compiler, fixture.evaluator, fixture.qualifiedProcessorProvider, fixture.planCache);
    runtime::GpuPreviewDisplayServiceOptions options;
    options.enabled = false; // This is production CPU fallback/control evidence, never GPU pixels.
    options.processingPhasePolicy = policy;
    runtime::GpuPreviewDisplayService service(
        scheduler,
        [&, stage](const document::Snapshot& snapshot,
                   const runtime::PreviewRequestIdentity& identity, const std::size_t bytes,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides,
                   runtime::TaskContext& context) {
            ++preparations;
            const auto delivered = view->load();
            const auto admitted = policy->snapshot().reservations;
            admittedWithinLiveBudget = admittedWithinLiveBudget && delivered &&
                                       delivered->fresh() && bytes > 0 &&
                                       bytes <= delivered->previewRequestBytes &&
                                       admitted.admittedPhases == 1 && admitted.hostCapacityBytes &&
                                       admitted.totalHostClaimBytes <= *admitted.hostCapacityBytes;
            return stage(snapshot, identity, bytes, overrides, context);
        },
        ui::makeCompositionPreviewCpuDisplayFallback(fixture.displayPreparer), options);
    auto cache = std::make_shared<ui::PreviewFrameCache>(12 * gib, ledger);
    const ui::PreviewPreparationSubmitter submitter =
        [&](runtime::TaskRequest request, const document::Snapshot& snapshot,
            const runtime::PreviewRequestIdentity& identity, const std::size_t bytes,
            const std::vector<runtime::SnapshotParameterOverride>& overrides) {
            return service.submit(std::move(request), snapshot, identity, bytes, overrides);
        };
    ui::CompositionPreviewSettings settings;
    settings.resolutionPolicy = runtime::PreviewResolutionPolicy::Full;
    ui::CompositionPreviewController preview(
        session, scheduler, bridge,
        ui::makeCompositionPreviewPipeline(fixture.compiler, fixture.evaluator,
                                           fixture.displayPreparer,
                                           fixture.qualifiedProcessorProvider, fixture.planCache),
        settings, cache, nullptr, submitter, {}, policy);
    const auto readyAt = [&](const core::RationalTime time,
                             const runtime::PreviewResolutionPolicy resolution) {
        return preview.state().activity == ui::PreviewActivity::Ready && preview.state().frame &&
               preview.state().freshness == ui::FrameFreshness::Current &&
               preview.state().frame->desiredIdentity().time == time &&
               preview.state().frame->desiredIdentity().resolutionPolicy == resolution;
    };
    auto held = std::make_shared<Held>();
    std::atomic_bool holdNext{false};
    std::atomic_size_t available{64 * gib};
    ui::MemoryBudgetController owner(scheduler, bridge, ledger, [&](const auto&) {
        if (holdNext.exchange(false)) {
            held->entered = true;
            held->release.wait();
        }
        auto measured = sample();
        measured.physicalBytes = 64 * gib;
        measured.machine.availableBytes = available.load();
        measured.process.status = runtime::ProcessMemoryStatus::Unsupported;
        measured.process.domains.clear();
        return measured;
    });
    QObject::connect(&owner, &ui::MemoryBudgetController::sampleApplied, &preview, [&] {
        const auto state = ledger.state();
        policy->applyHostSample(state, ledger.physicalByteBudget(),
                                ledger.allocate(18 * gib, 12 * gib));
        preview.notifyProcessingCapacityChanged();
    });
    expect(spin([&] { return readyAt(timeAt(0), runtime::PreviewResolutionPolicy::Full); }) &&
               preparations == 1 && cache->size() != 0 && cache->residentBytes() > 0,
           "first delivered sample prepares and retains a real Full preview");
    available = 22 * gib;
    owner.requestSample();
    expect(spin([&] { return ledger.state().sampleSerial == 2; }),
           "22 GiB availability is delivered through the actual sampler owner");
    const auto pressure = ledger.state();
    const auto host = policy->snapshot().reservations;
    expect(!pressure.memoryPressure && pressure.retentionPercent == 100 &&
               cache->byteBudget() == 12 * gib && pressure.retainedBytes > 0 &&
               cache->size() != 0 && ledger.physicalByteBudget() == 64 * gib && host.hostReady &&
               host.hostCapacityBytes && *host.hostCapacityBytes == 22 * gib && view->load() &&
               view->load()->fresh() && view->load()->previewRequestBytes > 0,
           "available-memory delivery preserves populated cache targets and publishes actual live "
           "capacity");
    cache->setByteBudget(1);
    expect(cache->size() == 0 && cache->byteBudget() == 1,
           "a deliberately tiny retention target drops cached frames before independent live work");
    expect(session.setCurrentTime(timeAt(1)) &&
               spin([&] { return readyAt(timeAt(1), runtime::PreviewResolutionPolicy::Full); }) &&
               preparations == 2,
           "uncached Full preview executes even when the cache target cannot retain one frame");
    preview.setResolutionPolicy(runtime::PreviewResolutionPolicy::Quarter);
    expect(spin([&] { return readyAt(timeAt(1), runtime::PreviewResolutionPolicy::Quarter); }) &&
               preparations == 3 && admittedWithinLiveBudget,
           "uncached Quarter preview executes with a fitting real phase and request budget");
    // Preview submission wakes the Jobs monitor. Stop it after the completed renders so the
    // held-sample span actually proves completion delivery without progress polling.
    bridge.stop();
    const auto pollsBeforeHold = jobsPolls;
    auto stale = *view->load();
    stale.sampledAt -= 11s;
    expect(view->publish(stale), "stalled sample fixture expires actual delivered defaults");
    holdNext = true;
    owner.requestSample();
    expect(spin([&] { return held->entered.load(); }), "replacement memory sampler is held off UI");
    expect(session.setCurrentTime(timeAt(2)) &&
               preview.state().activity == ui::PreviewActivity::Failed &&
               preview.state().message ==
                   QStringLiteral("Awaiting a fresh preview memory sample") &&
               preview.state().frame && preparations == 3,
           "stalled sample is readiness loss with retained pixels, not measured memory exhaustion");
    expect(session.setCurrentTime(timeAt(3)), "newest Quarter intent replaces stalled request");
    for (int index = 0; index < 20; ++index) {
        owner.requestSample();
        preview.notifyProcessingCapacityChanged();
    }
    QCoreApplication::processEvents();
    expect(owner.admittedSampleCount() == 3 && preparations == 3 && !bridge.isRunning() &&
               jobsPolls == pollsBeforeHold,
           "held sampler and stale defaults admit no backlog or retry spin with Jobs stopped");
    held->release.count_down();
    expect(spin([&] { return readyAt(timeAt(3), runtime::PreviewResolutionPolicy::Quarter); }) &&
               preparations == 4 && admittedWithinLiveBudget,
           "sample completion automatically recovers only the newest Quarter intent");
    owner.beginShutdown();
    preview.beginShutdown();
    service.beginShutdown();
    bridge.beginShutdown();
    expect(spin([&] { return scheduler.isQuiescent(); }), "pressure recovery tasks retire");
    expect(policy->snapshot().reservations.admittedPhases == 0,
           "pressure recovery leaves no processing phase claims");
}
void coldForegroundAndSettings() {
    auto& ledger = runtime::processMemoryBudgetLedger();
    ledger.beginProcessSampling();
    expect(ledger.state().effectiveBytes == 0 && config().isValid() &&
               runtime::GpuTaskAdmission{0, gib}.isValid(),
           "cold reusable zero does not invalidate foreground CPU/GPU processing admission");
    QTemporaryDir directory;
    QSettings settings(directory.filePath("budgets.ini"), QSettings::IniFormat);
    settings.setValue("playback/ram-preview-memory-bytes", QString::number(8 * gib));
    settings.setValue("playback/operation-cache-bytes", QString::number(16 * gib));
    static_cast<void>(ui::cacheMemoryBudgetsFromSettings(settings));
    runtime::TaskScheduler scheduler(config());
    ui::TaskUiBridge bridge(scheduler);
    bridge.stop();
    ui::ProjectHost host(scheduler);
    auto [document, stack] = host.liveDocumentAndStack();
    ui::CompositionSession session(*document, *stack, host.lowestCompositionId());
    const auto rate = document::FrameRate::create(25, 1);
    if (!rate.has_value()) {
        expect(false, "fixture frame rate is invalid");
        return;
    }
    const auto format =
        document::CompositionFormat::create(4, 3, core::PixelAspectRatio::square(), *rate);
    if (!format.has_value()) {
        expect(false, "fixture composition format is invalid");
        return;
    }
    commands::Transaction transaction("Seed cold preview", session.snapshot().revision());
    transaction.emplace<commands::AddComposition>("Cold preview", *format,
                                                  core::RationalTime::fromInteger(1));
    const auto seeded = session.executeTransaction(std::move(transaction));
    const auto id = seeded.outputId<document::CompositionId>(commands::kAddCompositionOutput);
    expect(id && session.setComposition(*id) && session.addSolidLayer("Solid", {0.2, 0.4, 0.8, 1}),
           "normal authoring seeds cold foreground preview");
    runtime::NodeDefinitionRegistry definitions;
    expect(runtime::registerBuiltInNodeDefinitions(definitions), "definitions register");
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer display;
    runtime::QualifiedDisplayProcessorProvider qualified;
    auto cache = std::make_shared<ui::PreviewFrameCache>(gib);
    ui::CompositionPreviewController preview(
        session, scheduler, bridge,
        ui::makeCompositionPreviewPipeline(compiler, evaluator, display, qualified), {}, cache);
    ui::WindowStatusBar first(session, &preview, nullptr, evaluator.operationCache().get());
    ui::WindowStatusBar second(session, &preview, nullptr, evaluator.operationCache().get());
    expect(spin([&] { return preview.state().activity == ui::PreviewActivity::Ready; }),
           "ordinary cold Visible production pipeline completes with Jobs stopped");
    expect(
        preview.state().frame && cache->size() == 0 && cache->byteBudget() == 0 &&
            evaluator.operationCache()->retainedBytes() == 0 && ledger.state().sampleSerial == 0,
        "foreground frame remains visible while reusable caches and status observers remain cold");
    ui::MemoryBudgetController owner(scheduler, bridge, ledger,
                                     [](const auto&) { return sample(); });
    expect(spin([&] { return ledger.state().sampleSerial == 1; }),
           "one owner delivers shared sample");
    const auto requested = ui::cacheMemoryBudgetsFromSettings(settings);
    cache->setByteBudget(requested.previewFrameCacheByteBudget);
    expect(owner.admittedSampleCount() == 1 &&
               settings.value("playback/operation-cache-bytes").toULongLong() == 16 * gib &&
               settings.value("playback/ram-preview-memory-bytes").toULongLong() == 8 * gib &&
               cache->byteBudget() < requested.previewFrameCacheByteBudget,
           "many status surfaces share one sampler; settings remain requested while cache clamps");
    settings.setValue("playback/ram-preview-memory-bytes", QString::number(20 * gib));
    settings.setValue("playback/operation-cache-bytes", QString::number(20 * gib));
    const auto oversized = ui::cacheMemoryBudgetsFromSettings(settings);
    first.pollMemoryPressureForTest({.availableBytes = 60 * gib},
                                    runtime::MemoryBudgetLedger::Clock::now());
    expect(settings.value("playback/operation-cache-bytes").toULongLong() == 20 * gib &&
               settings.value("playback/ram-preview-memory-bytes").toULongLong() == 20 * gib &&
               oversized == requested &&
               first.cacheToolTipForTest().contains(QStringLiteral("Requested cache total:")) &&
               first.cacheToolTipForTest().contains(QStringLiteral("Current cache budget:")) &&
               first.cacheToolTipForTest().contains(QStringLiteral("combined cache budgets")),
           "exact selected requests persist while reporting identifies the shared finite system "
           "limit");
    owner.beginShutdown();
    preview.beginShutdown();
    scheduler.beginShutdown();
    expect(spin([&] { return scheduler.isQuiescent(); }), "cold foreground scheduler retires");
}
} // namespace
// The pressure monitor samples on its own thread; a sample short of the headroom is applied on
// the UI thread, so a UI-owned cache evicts there within a few intervals, never on the monitor.
void pressureAppliesOnUiThread() {
    runtime::MemoryBudgetLedger ledger(60 * gib, 60 * gib);
    ledger.setRamHeadroom(2 * gib);
    std::atomic<std::size_t> held{4 * gib};
    const auto uiThread = std::this_thread::get_id();
    std::atomic<bool> appliedOnUi{true};
    ledger.registerCache(
        &held, 30 * gib, [&] { return held.load(); },
        [&](const std::size_t value) {
            if (std::this_thread::get_id() != uiThread)
                appliedOnUi = false;
            held = std::min(held.load(), value);
        });
    static_cast<void>(ledger.poll({.availableBytes = 20 * gib}));
    runtime::TaskScheduler scheduler(config());
    ui::TaskUiBridge bridge(scheduler);
    std::atomic<std::size_t> taken{0};
    runtime::MemoryPressureMonitor::Options pressure;
    pressure.interval = 20ms;
    pressure.psiTrigger = false;
    pressure.counterSampler = {};
    // 20 GiB free until another program takes 19 GiB; what the cache releases becomes free.
    pressure.sampler = [&] {
        return runtime::MachineMemorySample{.availableBytes =
                                                20 * gib - taken.load() + (4 * gib - held.load())};
    };
    ui::MemoryBudgetController owner(
        scheduler, bridge, ledger, [](const auto&) { return sample(); }, nullptr, pressure);
    const auto end = std::chrono::steady_clock::now() + 150ms;
    while (std::chrono::steady_clock::now() < end)
        QCoreApplication::processEvents();
    expect(owner.pressureApplications() == 0 && held == 4 * gib,
           "a machine above the headroom applies no pressure");
    taken = 19 * gib; // Another program takes memory: 1 GiB short of the 2 GiB headroom.
    expect(spin([&] { return owner.pressureApplications() > 0 && held < 4 * gib; }),
           "pressure shrinks a UI-owned cache without waiting for the next process sample");
    const auto settleEnd = std::chrono::steady_clock::now() + 150ms;
    while (std::chrono::steady_clock::now() < settleEnd)
        QCoreApplication::processEvents();
    expect(held == 4 * gib - (gib + gib / 4) && appliedOnUi,
           "the cache gives back the deficit plus the margin, on the UI thread, then holds");
    owner.beginShutdown();
    scheduler.beginShutdown();
    expect(spin([&] { return scheduler.isQuiescent(); }), "pressure scheduler retires");
    ledger.unregisterCache(&held);
}
// Evict-on-demand: a processing-phase reclaimer on a worker thread makes the UI-owned pools give
// back the shortfall on the UI thread and takes a fresh sample at once; after shutdown a kept
// reclaimer is a no-op.
void reclaimForWorkOnUiThread() {
    runtime::MemoryBudgetLedger ledger(60 * gib, 60 * gib);
    std::size_t held = 4 * gib;
    const auto uiThread = std::this_thread::get_id();
    bool appliedOnUi = true;
    ledger.registerCache(
        &held, 30 * gib, [&] { return held; },
        [&](const std::size_t value) {
            appliedOnUi = appliedOnUi && std::this_thread::get_id() == uiThread;
            held = std::min(held, value);
        });
    runtime::TaskScheduler scheduler(config());
    ui::TaskUiBridge bridge(scheduler);
    std::optional<runtime::MemoryPressureMonitor::Options> noMonitor;
    ui::MemoryBudgetController owner(scheduler, bridge, ledger, [](const auto&) { return sample(); },
                                     nullptr, noMonitor);
    expect(spin([&] { return owner.admittedSampleCount() >= 1 && !owner.sampleActive(); }),
           "the initial sample settles");
    const auto samplesBefore = owner.admittedSampleCount();
    auto reclaim = owner.workReclaimer();
    std::thread worker([&] {
        reclaim(gib);
        reclaim(gib / 2); // Coalesced with the first while it is queued.
    });
    worker.join();
    expect(spin([&] { return owner.reclaimApplications() == 1; }),
           "the reclaim is applied once on the UI thread");
    expect(held == 3 * gib && appliedOnUi,
           "the pools gave back the shortfall, on the UI thread");
    expect(owner.admittedSampleCount() > samplesBefore,
           "a fresh sample is taken at once so waiting work sees the released memory");
    owner.beginShutdown();
    reclaim(gib);
    const auto end = std::chrono::steady_clock::now() + 50ms;
    while (std::chrono::steady_clock::now() < end)
        QCoreApplication::processEvents();
    expect(held == 3 * gib && owner.reclaimApplications() == 1,
           "a reclaimer kept past shutdown does nothing");
    scheduler.beginShutdown();
    expect(spin([&] { return scheduler.isQuiescent(); }), "reclaim scheduler retires");
    ledger.unregisterCache(&held);
}
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    heldSampling();
    pressureAppliesOnUiThread();
    reclaimForWorkOnUiThread();
    teardown();
    liveSettingsAndDeliveredCapacity();
    sampledPressurePreviewRecovery();
    coldForegroundAndSettings();
    return failures == 0 ? 0 : 1;
}
