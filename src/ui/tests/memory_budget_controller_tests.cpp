#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
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

#include <atomic>
#include <chrono>
#include <iostream>
#include <latch>
#include <memory>
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
                                     6 * gib}}}};
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
    expect(budget > 0 && budget <= gib / 2 && appliedOnUi && polls == 0 && !owner.sampleActive(),
           "cache callback applies only on UI within paired process cap");
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
void coldForegroundAndSettings() {
    auto& ledger = runtime::processMemoryBudgetLedger();
    ledger.beginProcessSampling();
    expect(ledger.state().effectiveBytes == 0 && config().isValid() &&
               runtime::GpuTaskAdmission{0, gib}.isValid(),
           "cold reusable zero does not invalidate foreground CPU/GPU processing admission");
    QTemporaryDir directory;
    QSettings settings(directory.filePath("budgets.ini"), QSettings::IniFormat);
    ui::setRamPreviewByteBudgetInSettings(settings, 8 * gib);
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
    ui::setRamPreviewByteBudgetInSettings(settings, 20 * gib);
    settings.setValue("playback/operation-cache-bytes", QString::number(20 * gib));
    const auto oversized = ui::cacheMemoryBudgetsFromSettings(settings);
    first.pollMemoryPressureForTest({.availableBytes = 60 * gib},
                                    runtime::MemoryBudgetLedger::Clock::now());
    expect(settings.value("playback/operation-cache-bytes").toULongLong() == 20 * gib &&
               settings.value("playback/ram-preview-memory-bytes").toULongLong() == 20 * gib &&
               oversized.operationCacheByteBudget + oversized.previewFrameCacheByteBudget ==
                   36 * gib &&
               ledger.state().configuredBytes == 36 * gib &&
               first.cacheToolTipForTest().contains(QStringLiteral("Configured cache ceiling:")) &&
               !first.cacheToolTipForTest().contains(QStringLiteral("Requested total:")),
           "oversized raw requests persist; reporting distinguishes resolved configured ceiling");
    owner.beginShutdown();
    preview.beginShutdown();
    scheduler.beginShutdown();
    expect(spin([&] { return scheduler.isQuiescent(); }), "cold foreground scheduler retires");
}
} // namespace
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    heldSampling();
    teardown();
    coldForegroundAndSettings();
    return failures == 0 ? 0 : 1;
}
