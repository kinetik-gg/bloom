// GPU preview display service acceptance tests and paired-benchmark preparation.
// Non-device cases run the real compiler/evaluator/qualified-processor fixture through the
// service's CPU path. Device-gated cases drive full submit -> native -> product and are SKIPPED
// (never fabricated) when no device is available. The benchmark records nothing as truth.
#include "gpu_preview_display_service_private.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_preview_display_product.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/qualified_display_preparation.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <utility>
#include <vector>
namespace {
using namespace std::chrono_literals;
using namespace bloom;
using runtime::PreviewCpuStage;
using runtime::PreviewCpuStageFunction;
using runtime::PreviewCpuStageOutcomeHandle;
using runtime::PreviewPreparationResult;
using runtime::PreviewPreparationResultHandle;
using runtime::TaskContext;
using runtime::TaskDiagnostic;
using runtime::TaskHandle;
using runtime::TaskResult;
using runtime::TaskScheduler;
using runtime::TaskSchedulerConfig;
using runtime::TaskState;
constexpr std::size_t kBudget = std::size_t{1} << 28;
constexpr auto kProjectId = document::ProjectId::fromRaw(1);
constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 4> kSizes{
    {{1920, 1080}, {1280, 720}, {960, 540}, {256, 144}}};
class Expectations final {
  public:
    void expect(const bool ok, const std::string& message,
                const std::source_location loc = std::source_location::current()) {
        if (!ok) {
            ++failures_;
            std::cerr << loc.file_name() << ':' << loc.line() << ": " << message << '\n';
        }
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};
[[nodiscard]] TaskSchedulerConfig config(const std::size_t cpuWorkers = 1) {
    TaskSchedulerConfig c;
    c.cpuWorkerCount = cpuWorkers;
    c.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    c.blockingIoWorkerCount = 1;
    c.cpuQueueCapacity = 64;
    c.blockingIoQueueCapacity = 8;
    c.gpuPendingQueueCapacity = 8;
    c.gpuAdmittedStateCapacity = 8;
    c.gpuLiveContinuationCapacity = 4;
    c.gpuQueuedCommandByteCapacity = std::size_t{1} << 30U;
    c.gpuRequestOwnedByteCapacity = std::size_t{1} << 30U;
    c.terminalHistoryCapacity = 64;
    c.diagnosticsPerTask = 16;
    c.groupRegistryCapacity = 16;
    return c;
}
[[nodiscard]] runtime::TaskOwner owner(const std::uint64_t value) {
    return {.kind = runtime::TaskOwnerKind::Composition,
            .id = runtime::TaskOwnerId::fromRaw(value)};
}
[[nodiscard]] document::Snapshot makeSnapshot() {
    document::Document doc(document::Project(kProjectId, "svc-test"));
    return doc.snapshot();
}
template <typename Value>
[[nodiscard]] std::optional<TaskResult<Value>>
awaitResult(const TaskHandle<Value>& handle, const std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = handle.tryTakeResult()) {
            return result;
        }
        std::this_thread::sleep_for(500us);
    }
    return std::nullopt;
}
template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(500us);
    }
    return predicate();
}
[[nodiscard]] runtime::GpuPreviewDisplayServiceOptions gpuOptions(const std::string& loader) {
    runtime::GpuPreviewDisplayServiceOptions options;
    options.enabled = true;
    options.loaderPath = loader;
    options.previewByteAllowance = kBudget;
    return options;
}
[[nodiscard]] bool ensureReady(Expectations& expectations,
                               runtime::GpuPreviewDisplayService& service,
                               const bool requireDevice) {
    const bool terminal = waitUntil(
        [&] {
            return service.status().state != runtime::GpuPreviewDisplayServiceState::Initializing;
        },
        30s);
    const bool ready =
        terminal && service.status().state == runtime::GpuPreviewDisplayServiceState::Ready;
    if (!ready) {
        expectations.expect(!requireDevice, "a device is required but unavailable");
    }
    return ready;
}
[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>>
selectableSize(const runtime::GpuNeutralDisplayQualificationReport& report) {
    for (const auto& candidate : kSizes) {
        if (runtime::detail::gpuPreviewDisplaySelectsNative(
                report, static_cast<std::uint64_t>(candidate.first) * candidate.second,
                runtime::detail::gpuPreviewDisplayHandoffOverheadMicros())) {
            return candidate;
        }
    }
    return std::nullopt;
}
// Non-device: disabled and missing-loader both take the ordinary CPU task, one evaluation each.
void testCpuPaths(Expectations& expectations) {
    runtime::detail::PreviewDisplayServiceTestFixture fixture(32, 18);
    expectations.expect(fixture.processor != nullptr, "the neutral processor builds");
    if (fixture.processor == nullptr) {
        return;
    }
    const auto snapshot = makeSnapshot();
    {
        TaskScheduler scheduler(config());
        runtime::GpuPreviewDisplayServiceOptions options;
        options.enabled = false;
        runtime::GpuPreviewDisplayService service(scheduler, fixture.stageFn(), fixture.fallback(),
                                                  options);
        auto submission = service.submit(runtime::TaskRequest("preview", owner(1)), snapshot,
                                         runtime::detail::testIdentity(*fixture.plan), kBudget, {});
        const auto result = awaitResult(submission.handle);
        expectations.expect(result.has_value() && result->state() == TaskState::Succeeded &&
                                fixture.evaluations->load() == 1 && fixture.fallbacks->load() == 1,
                            "the disabled path evaluates once and falls back once");
        expectations.expect(result.has_value() && result->value().has_value() &&
                                result->value().value()->status() ==
                                    runtime::PreviewPreparationStatus::Prepared &&
                                result->value().value()->frame() != nullptr,
                            "the disabled path publishes a prepared frame");
        expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                            "the disabled path leaves the scheduler quiescent");
    }
    fixture.evaluations->store(0);
    fixture.fallbacks->store(0);
    {
        TaskScheduler scheduler(config(2));
        runtime::GpuPreviewDisplayService service(scheduler, fixture.stageFn(), fixture.fallback(),
                                                  gpuOptions("/nonexistent/bloom-loader.so"));
        expectations.expect(waitUntil([&] {
                                return service.status().state !=
                                       runtime::GpuPreviewDisplayServiceState::Initializing;
                            }) &&
                                service.status().state ==
                                    runtime::GpuPreviewDisplayServiceState::Unavailable,
                            "a missing loader reports Unavailable");
        auto submission = service.submit(runtime::TaskRequest("preview", owner(2)), snapshot,
                                         runtime::detail::testIdentity(*fixture.plan), kBudget, {});
        const auto result = awaitResult(submission.handle);
        expectations.expect(result.has_value() && result->state() == TaskState::Succeeded &&
                                fixture.evaluations->load() == 1 && fixture.fallbacks->load() == 1,
                            "the unavailable path uses the ordinary CPU task");
        expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                            "the unavailable path leaves the scheduler quiescent");
    }
}
// Device: full submit -> native -> product provenance and CPU pixel parity.
void testGpuProvenanceAndParity(Expectations& expectations, const std::string& loader,
                                const bool requireDevice) {
    runtime::detail::PreviewDisplayServiceTestFixture probe(1920, 1080);
    if (probe.processor == nullptr) {
        expectations.expect(!requireDevice, "the neutral processor builds");
        return;
    }
    const auto snapshot = makeSnapshot();
    TaskScheduler scheduler(config());
    std::shared_ptr<const runtime::GpuNeutralDisplayQualificationReport> report;
    {
        runtime::GpuPreviewDisplayService probeService(scheduler, {}, {}, gpuOptions(loader));
        if (!ensureReady(expectations, probeService, requireDevice)) {
            return;
        }
        report = probeService.status().qualification;
    }
    const auto size = selectableSize(*report);
    if (!size.has_value()) {
        std::cout << "SKIP: no device-eligible size beats the two service handoffs\n";
        return;
    }
    runtime::detail::PreviewDisplayServiceTestFixture fixture(size->first, size->second);
    runtime::GpuPreviewDisplayService service(scheduler, fixture.stageFn(), fixture.fallback(),
                                              gpuOptions(loader));
    if (!ensureReady(expectations, service, requireDevice)) {
        return;
    }
    const auto identity = runtime::detail::testIdentity(*fixture.plan);
    auto submission =
        service.submit(runtime::TaskRequest("preview", owner(4)), snapshot, identity, kBudget, {});
    const auto result = awaitResult(submission.handle);
    expectations.expect(result.has_value() && result->state() == TaskState::Succeeded &&
                            fixture.evaluations->load() == 1,
                        "the qualified path succeeds after one evaluation");
    if (!result.has_value() || !result->value().has_value() ||
        result->value().value()->frame() == nullptr) {
        return;
    }
    const auto displayOnly = result->value().value()->frame()->displayOnlyFrame();
    expectations.expect(displayOnly != nullptr &&
                            displayOnly->provenance().provider ==
                                runtime::PreviewDisplayProvider::GpuNeutral &&
                            displayOnly->isOcioQualified(),
                        "the result is an OCIO-qualified GPU Neutral display-only frame");
    if (displayOnly == nullptr) {
        return;
    }
    runtime::CpuQualifiedDisplayPreparer preparer(*fixture.processor);
    runtime::QualifiedDisplayPreparationRequest oracleRequest;
    oracleRequest.aggregatePixelStorageByteLimit = kBudget;
    auto oracle = preparer.prepare(runtime::detail::testEvaluateFrame(fixture.plan, kBudget),
                                   oracleRequest, runtime::CancellationToken{});
    const auto gpuView = displayOnly->displayBufferView();
    expectations.expect(oracle.status() == runtime::QualifiedDisplayPreparationStatus::Prepared &&
                            gpuView.has_value() &&
                            gpuView->pixels.size() == oracle.frame()->buffer().pixels().size() &&
                            std::equal(gpuView->pixels.begin(), gpuView->pixels.end(),
                                       oracle.frame()->buffer().pixels().begin()),
                        "GPU packed pixels match the CPU qualified render");
    expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                        "the qualified path leaves the scheduler quiescent");
}
// Device: a late eligibility failure (tiny frame) reuses the single evaluated stage.
void testLateFallbackSameFrame(Expectations& expectations, const std::string& loader,
                               const bool requireDevice) {
    runtime::detail::PreviewDisplayServiceTestFixture fixture(8, 8);
    if (fixture.processor == nullptr) {
        return;
    }
    const auto snapshot = makeSnapshot();
    TaskScheduler scheduler(config());
    runtime::GpuPreviewDisplayService service(scheduler, fixture.stageFn(), fixture.fallback(),
                                              gpuOptions(loader));
    if (!ensureReady(expectations, service, requireDevice)) {
        return;
    }
    auto submission = service.submit(runtime::TaskRequest("preview", owner(5)), snapshot,
                                     runtime::detail::testIdentity(*fixture.plan), kBudget, {});
    const auto result = awaitResult(submission.handle);
    expectations.expect(result.has_value() && result->state() == TaskState::Succeeded &&
                            fixture.evaluations->load() == 1 && fixture.fallbacks->load() == 1,
                        "the late fallback reuses the one evaluated stage");
    expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                        "the late-fallback path leaves the scheduler quiescent");
}
// Device: one CPU worker is free while the final GPU parent is pending.
void testWorkerFreedWhilePending(Expectations& expectations, const std::string& loader,
                                 const bool requireDevice) {
    runtime::detail::PreviewDisplayServiceTestFixture fixture(3840, 2160);
    if (fixture.processor == nullptr) {
        return;
    }
    const auto snapshot = makeSnapshot();
    TaskScheduler scheduler(config(1));
    runtime::GpuPreviewDisplayService service(scheduler, fixture.stageFn(), fixture.fallback(),
                                              gpuOptions(loader));
    if (!ensureReady(expectations, service, requireDevice)) {
        return;
    }
    auto submission = service.submit(runtime::TaskRequest("preview", owner(6)), snapshot,
                                     runtime::detail::testIdentity(*fixture.plan), kBudget, {});
    const auto core = runtime::detail::GpuPreviewDisplayServiceTestAccess::coreOf(service);
    const bool pending = waitUntil(
        [&] {
            return core != nullptr &&
                   runtime::detail::GpuPreviewDisplayServiceTestAccess::isNativeInFlight(*core);
        },
        5s);
    if (pending) {
        auto unrelated = scheduler.submit<int>(
            runtime::TaskRequest("Unrelated CPU work", owner(7), runtime::TaskPriority::Background),
            [](TaskContext&) { return TaskResult<int>::succeeded(7); });
        expectations.expect(awaitResult(unrelated.handle).has_value(),
                            "a CPU worker is free while the GPU parent is pending");
    } else {
        std::cout << "SKIP: the native job retired before the CPU-free window was observed\n";
    }
    expectations.expect(awaitResult(submission.handle).has_value(), "the GPU parent resolves");
}
// Device: a live GPU parent retains completion until its cancelled child is actually terminal.
void testHeldStageCancellation(Expectations& expectations, const std::string& loader,
                               const bool requireDevice) {
    runtime::detail::PreviewDisplayServiceTestFixture fixture(3840, 2160);
    if (fixture.processor == nullptr) {
        return;
    }
    auto started = std::make_shared<std::atomic_bool>(false);
    auto release = std::make_shared<std::atomic_bool>(false);
    PreviewCpuStageFunction blocking =
        [plan = fixture.plan, processor = fixture.processor, started,
         release](const document::Snapshot&, const runtime::PreviewRequestIdentity& identity,
                  const std::size_t limit, const std::vector<runtime::SnapshotParameterOverride>&,
                  TaskContext&) -> TaskResult<PreviewCpuStageOutcomeHandle> {
        using R = TaskResult<PreviewCpuStageOutcomeHandle>;
        auto frame = runtime::detail::testEvaluateFrame(plan, limit);
        started->store(true, std::memory_order_release);
        // Deliberately ignores cancellation until release: proves the service retains ownership
        // rather than force-releasing the parent at any deadline.
        while (!release->load()) {
            std::this_thread::sleep_for(1ms);
        }
        if (frame == nullptr) {
            return R::cancelled();
        }
        auto stage = std::make_shared<const PreviewCpuStage>(identity, frame, processor, limit,
                                                             std::vector<TaskDiagnostic>{});
        return R::succeeded(
            std::make_shared<const runtime::PreviewCpuStageOutcome>(runtime::PreviewCpuStageOutcome{
                runtime::PreviewCpuStageStatus::Evaluated, std::move(stage), {}}));
    };
    const auto snapshot = makeSnapshot();
    TaskScheduler scheduler(config(1));
    runtime::GpuPreviewDisplayService service(scheduler, std::move(blocking), {},
                                              gpuOptions(loader));
    if (!ensureReady(expectations, service, requireDevice)) {
        return;
    }
    auto submission = service.submit(runtime::TaskRequest("preview", owner(8)), snapshot,
                                     runtime::detail::testIdentity(*fixture.plan), kBudget, {});
    expectations.expect(submission.accepted() && waitUntil([&] { return started->load(); }),
                        "the blocking stage child is live");
    service.beginShutdown();
    // Held well beyond the former unconditional 5s deadline: ownership must not be released.
    std::this_thread::sleep_for(6s);
    expectations.expect(!submission.handle.tryTakeResult().has_value(),
                        "a live child keeps the parent pending beyond the former deadline");
    release->store(true);
    const auto result = awaitResult(submission.handle);
    expectations.expect(result.has_value() && result->state() == TaskState::Cancelled,
                        "the parent completes as Cancelled only after the child is terminal");
    scheduler.beginShutdown();
    expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                        "held cancellation/shutdown reaches quiescence");
}
// Non-device: a submission racing shutdown is rejected without evaluation.
void testSubmitAfterShutdownRejects(Expectations& expectations) {
    runtime::detail::PreviewDisplayServiceTestFixture fixture(16, 9);
    if (fixture.processor == nullptr) {
        return;
    }
    const auto snapshot = makeSnapshot();
    TaskScheduler scheduler(config(2));
    runtime::GpuPreviewDisplayServiceOptions options;
    options.enabled = false;
    runtime::GpuPreviewDisplayService service(scheduler, fixture.stageFn(), fixture.fallback(),
                                              options);
    expectations.expect(service
                            .submit(runtime::TaskRequest("preview", owner(20)), snapshot,
                                    runtime::detail::testIdentity(*fixture.plan), kBudget, {})
                            .accepted(),
                        "pre-shutdown submit admitted");
    service.beginShutdown();
    const int before = fixture.evaluations->load();
    expectations.expect(!service
                             .submit(runtime::TaskRequest("preview", owner(21)), snapshot,
                                     runtime::detail::testIdentity(*fixture.plan), kBudget, {})
                             .accepted(),
                        "submit after shutdown rejected");
    expectations.expect(fixture.evaluations->load() == before,
                        "rejected submit performs no evaluation");
    scheduler.beginShutdown();
    expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }), "quiesces");
}
// Non-device: beginShutdown immediately after construction must not hang or abandon startup.
void testShutdownBeforeStartupDispatch(Expectations& expectations) {
    TaskScheduler scheduler(config());
    runtime::GpuPreviewDisplayService service(scheduler, {}, {},
                                              gpuOptions("/nonexistent/bloom-loader.so"));
    service.beginShutdown();
    expectations.expect(waitUntil([&] {
                            return service.status().state ==
                                   runtime::GpuPreviewDisplayServiceState::Stopped;
                        }),
                        "shutdown reaches Stopped");
    scheduler.beginShutdown();
    expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                        "startup root not abandoned");
}

// Non-device: more than 64 accepted ordinary CPU roots all retain terminal ownership.
void testManyOrdinaryRootsRetainOwnership(Expectations& expectations) {
    runtime::detail::PreviewDisplayServiceTestFixture fixture(16, 9);
    if (fixture.processor == nullptr) {
        return;
    }
    const auto snapshot = makeSnapshot();
    TaskSchedulerConfig schedulerConfig = config(1);
    schedulerConfig.cpuQueueCapacity = 128;
    TaskScheduler scheduler(schedulerConfig);
    runtime::GpuPreviewDisplayServiceOptions options;
    options.enabled = false;
    runtime::GpuPreviewDisplayService service(scheduler, fixture.stageFn(), fixture.fallback(),
                                              options);
    std::vector<TaskHandle<PreviewPreparationResultHandle>> handles;
    for (std::uint64_t index = 0; index < 70; ++index) {
        auto submission =
            service.submit(runtime::TaskRequest("preview", owner(100 + index)), snapshot,
                           runtime::detail::testIdentity(*fixture.plan), kBudget, {});
        expectations.expect(submission.accepted(), "ordinary CPU root admitted");
        if (submission.accepted()) {
            handles.push_back(submission.handle);
        }
    }
    service.beginShutdown();
    int resolved = 0;
    for (const auto& handle : handles) {
        if (awaitResult(handle, 5s).has_value()) {
            ++resolved;
        }
    }
    expectations.expect(resolved == static_cast<int>(handles.size()),
                        "all roots above 64 reach terminal (no silent drop)");
    scheduler.beginShutdown();
    expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }), "quiesces");
}

// Non-device: unrelated scheduler tasks are preserved across service shutdown.
void testUnrelatedTasksPreserved(Expectations& expectations) {
    TaskScheduler scheduler(config(2));
    runtime::GpuPreviewDisplayServiceOptions options;
    options.enabled = false;
    runtime::GpuPreviewDisplayService service(scheduler, {}, {}, options);
    auto unrelated = scheduler.submit<int>(
        runtime::TaskRequest("unrelated", owner(200), runtime::TaskPriority::Background),
        [](TaskContext&) { return TaskResult<int>::succeeded(5); });
    service.beginShutdown();
    expectations.expect(awaitResult(unrelated.handle).has_value(),
                        "unrelated scheduler task preserved");
    scheduler.beginShutdown();
    expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }), "quiesces");
}

// Device: a stalled native dispatch is retired by the bounded deadline and falls back same-stage.
void testNativeDeadlineRetirement(Expectations& expectations, const std::string& loader,
                                  const bool requireDevice) {
    const auto scenario = runtime::detail::runNativeDeadlineScenario(loader);
    if (!scenario.ran) {
        expectations.expect(!requireDevice,
                            scenario.message.empty() ? "device required" : scenario.message);
        return;
    }
    expectations.expect(scenario.dispatched, "the native job is dispatched before the deadline");
    expectations.expect(scenario.retired, "deadline expiry retires the native pipeline");
    expectations.expect(scenario.timeoutDiagnostic, "deadline expiry publishes NativeTimeout");
    expectations.expect(scenario.fallbackDispatched,
                        "deadline expiry takes the same-stage CPU fallback");
}

// Paired benchmark preparation: medians over 5 warmups + 10 alternating pairs, real service
// handoff.
void runBenchmark(Expectations& expectations, const std::string& loader, const bool requireDevice) {
    const auto samples = runtime::detail::runGpuPreviewDisplayBenchmark(loader);
    if (samples.empty()) {
        expectations.expect(!requireDevice, "benchmark requires a qualified device");
        return;
    }
    for (const auto& sample : samples) {
        expectations.expect(sample.ran && sample.succeeded,
                            "benchmark sample ran and every measured submission succeeded");
        expectations.expect(sample.cacheHits > 0,
                            "warm operation cache produced hits over the measured pairs");
        std::cout << "benchmark " << sample.width << 'x' << sample.height
                  << ": service median=" << sample.serviceMedianMs
                  << "ms ordinary-cpu median=" << sample.cpuMedianMs
                  << "ms gpu=" << (sample.usedGpu ? "yes" : "no")
                  << " cache_hits=" << sample.cacheHits << " (measured)\n";
    }
    for (const auto& sample : samples) {
        if (sample.height == 1080) {
            expectations.expect(sample.usedGpu && sample.serviceMedianMs < sample.cpuMedianMs,
                                "1080p takes the GPU path and beats the CPU path");
        }
    }
}

} // namespace
int main(const int argc, char** argv) {
    try {
        std::string loader;
        bool requireDevice = false;
        bool benchmark = false;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index] == nullptr ? std::string{} : argv[index];
            if (argument == "--loader" && index + 1 < argc && argv[index + 1] != nullptr) {
                loader = argv[++index];
            } else if (argument == "--require-device") {
                requireDevice = true;
            } else if (argument == "--benchmark") {
                benchmark = true;
            }
        }
        Expectations expectations;
        testCpuPaths(expectations);
        testSubmitAfterShutdownRejects(expectations);
        testShutdownBeforeStartupDispatch(expectations);
        testManyOrdinaryRootsRetainOwnership(expectations);
        testUnrelatedTasksPreserved(expectations);
        testGpuProvenanceAndParity(expectations, loader, requireDevice);
        testLateFallbackSameFrame(expectations, loader, requireDevice);
        testWorkerFreedWhilePending(expectations, loader, requireDevice);
        testHeldStageCancellation(expectations, loader, requireDevice);
        testNativeDeadlineRetirement(expectations, loader, requireDevice);
        if (benchmark) {
            runBenchmark(expectations, loader, requireDevice);
        }
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures()
                      << " gpu_preview_display_service expectation(s) failed\n";
            return 1;
        }
        std::cout << "gpu_preview_display_service_tests: all expectations passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
