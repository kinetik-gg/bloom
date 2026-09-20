// GPU-APP-1 focused application-pipeline test: the real runtime service driven through the preview
// submitter the controllers use, with the production CPU stage/display fallback, the real
// compiler/evaluator/qualified provider, and a real compiled snapshot. It asserts actual GPU
// provenance and CPU-oracle parity at 1080p, the missing-loader CPU fallback without duplicate
// evaluation, the live cached-frame re-request, and a warm-cache paired timing. It never fabricates
// a GPU success or an eligibility report.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_neutral_display.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_preview_display_product.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_plan_cache.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QString>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
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

// Matches the controller's default preview pixel-storage allowance (512 MiB) so the
// service's request-owned admission is not tighter than the request it is handed.
constexpr std::size_t kBudget = std::size_t{512} * 1024U * 1024U;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

struct Options final {
    std::filesystem::path loader;
    bool requireDevice = false;
    bool valid = true;
};

[[nodiscard]] Options parseOptions(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index] == nullptr ? std::string{} : argv[index];
        if (argument == "--loader" && index + 1 < argc && argv[index + 1] != nullptr) {
            options.loader = argv[++index];
        } else if (argument == "--require-device") {
            options.requireDevice = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            options.valid = false;
        }
    }
    return options;
}

template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 15'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (std::invoke(predicate)) {
            return true;
        }
        std::this_thread::yield();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return std::invoke(predicate);
}

[[nodiscard]] runtime::TaskSchedulerConfig serviceSchedulerConfig() {
    runtime::TaskSchedulerConfig config;
    config.cpuWorkerCount = 2;
    config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    config.blockingIoWorkerCount = 1;
    config.cpuQueueCapacity = 32;
    config.blockingIoQueueCapacity = 8;
    config.gpuPendingQueueCapacity = 8;
    config.gpuAdmittedStateCapacity = 16;
    config.gpuLiveContinuationCapacity = 8;
    config.gpuQueuedCommandByteCapacity = std::size_t{1} << 30U;
    config.gpuRequestOwnedByteCapacity = std::size_t{1} << 30U;
    config.terminalHistoryCapacity = 64;
    config.diagnosticsPerTask = 16;
    config.groupRegistryCapacity = 16;
    return config;
}

[[nodiscard]] core::RationalTime time(const std::int64_t numerator,
                                      const std::int64_t denominator = 1) {
    const auto value = core::RationalTime::create(numerator, denominator);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

[[nodiscard]] document::CompositionFormat viewerFormat() {
    const auto rate = document::FrameRate::create(25, 1);
    if (!rate.has_value()) {
        std::abort();
    }
    const auto format =
        document::CompositionFormat::create(1920, 1080, core::PixelAspectRatio::square(), *rate);
    if (!format.has_value()) {
        std::abort();
    }
    return *format;
}

[[nodiscard]] document::NewProject makeViewerProject() {
    return document::makeNewProject("GPU app pipeline", "Main", time(4), viewerFormat());
}

// The real compile/evaluate/select stage and the real display fallback, exactly as the application
// constructs them, plus the counters the test needs.
struct AppFixture final {
    runtime::NodeDefinitionRegistry definitions;
    runtime::SnapshotCompiler compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer referencePreparer;
    runtime::QualifiedDisplayProcessorProvider provider;
    ui::CompiledPlanCacheHandle planCache = std::make_shared<ui::CompiledPlanCache>();
    std::atomic<int> stageEvaluations{0};

    AppFixture() : compiler(definitions) {
        if (!runtime::registerBuiltInNodeDefinitions(definitions)) {
            std::abort();
        }
        definitions.freeze();
        provider.publish(runtime::buildBloomNeutralQualifiedDisplayProcessor());
    }

    [[nodiscard]] runtime::PreviewCpuStageFunction stage() {
        auto stage = ui::makeCompositionPreviewCpuStage(compiler, evaluator, provider, planCache);
        return [this, stage = std::move(stage)](
                   const document::Snapshot& snapshot,
                   const runtime::PreviewRequestIdentity& identity, const std::size_t limit,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides,
                   runtime::TaskContext& context) {
            stageEvaluations.fetch_add(1);
            return stage(snapshot, identity, limit, overrides, context);
        };
    }

    [[nodiscard]] runtime::PreviewCpuDisplayFallback fallback() const {
        return ui::makeCompositionPreviewCpuDisplayFallback(referencePreparer);
    }
};

[[nodiscard]] runtime::GpuPreviewDisplayServiceOptions
gpuOptions(const std::filesystem::path& loader) {
    runtime::GpuPreviewDisplayServiceOptions options;
    options.enabled = true;
    options.loaderPath = loader;
    options.previewByteAllowance = kBudget;
    return options;
}

[[nodiscard]] ui::PreviewPreparationSubmitter
serviceSubmitter(runtime::GpuPreviewDisplayService& service,
                 const std::shared_ptr<std::atomic<int>>& calls) {
    return
        [&service, calls](runtime::TaskRequest request, const document::Snapshot& snapshot,
                          const runtime::PreviewRequestIdentity& identity, const std::size_t limit,
                          const std::vector<runtime::SnapshotParameterOverride>& overrides) {
            calls->fetch_add(1);
            return service.submit(std::move(request), snapshot, identity, limit, overrides);
        };
}

[[nodiscard]] bool isReady(const ui::CompositionPreviewController& controller) {
    return controller.state().activity == ui::PreviewActivity::Ready;
}

[[nodiscard]] bool pixelsMatchCpuOracle(const runtime::PreparedPreviewFrame& gpu,
                                        const runtime::PreparedPreviewFrame& cpu) {
    const auto gpuView = gpu.displayBufferView();
    const auto cpuView = cpu.displayBufferView();
    if (!gpuView.has_value() || !cpuView.has_value() ||
        gpuView->pixels.size() != cpuView->pixels.size()) {
        return false;
    }
    for (std::size_t index = 0; index < gpuView->pixels.size(); ++index) {
        const auto& a = gpuView->pixels[index];
        const auto& b = cpuView->pixels[index];
        if (a.alpha != b.alpha) {
            return false;
        }
        const auto close = [](const std::uint8_t lhs, const std::uint8_t rhs) {
            return lhs > rhs ? lhs - rhs <= 1 : rhs - lhs <= 1;
        };
        if (!close(a.red, b.red) || !close(a.green, b.green) || !close(a.blue, b.blue)) {
            return false;
        }
    }
    return true;
}

// Gate 2: real service through the foreground controller at 1080p: actual GPU provenance and
// CPU-oracle parity, then a cached re-request with zero extra preparation.
void testControllerGpuPathAndParity(Expectations& expectations, const Options& options) {
    auto newProject = makeViewerProject();
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.4, 0.8, 1.0}),
        "the fixture adds a full-frame solid layer");
    runtime::TaskScheduler scheduler(serviceSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    AppFixture fixture;
    auto serviceCalls = std::make_shared<std::atomic<int>>(0);
    runtime::GpuPreviewDisplayService service(scheduler, fixture.stage(), fixture.fallback(),
                                              gpuOptions(options.loader));
    const bool terminal = waitUntil([&] {
        return service.status().state != runtime::GpuPreviewDisplayServiceState::Initializing;
    });
    const bool ready =
        terminal && service.status().state == runtime::GpuPreviewDisplayServiceState::Ready;
    expectations.expect(ready || !options.requireDevice,
                        "the service reaches Ready when a device is required");

    auto frameCache = std::make_shared<ui::PreviewFrameCache>();
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        ui::makeCompositionPreviewPipeline(fixture.compiler, fixture.evaluator,
                                           fixture.referencePreparer, fixture.provider,
                                           fixture.planCache),
        {.colorIntent = session.colorIntent(),
         .displayName = {},
         .viewName = {},
         .showLook = true,
         .pixelStorageByteLimit = kBudget},
        frameCache, nullptr, serviceSubmitter(service, serviceCalls));
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the foreground frame is ready through the real service");
    if (ready) {
        // The controller may publish an initial CPU frame synchronously; wait for the routed GPU
        // result to replace it before asserting provenance.
        expectations.expect(
            waitUntil([&] {
                const auto current = controller.state().frame;
                return current != nullptr && current->provenance().provider ==
                                                 runtime::PreviewDisplayProvider::GpuNeutral;
            }),
            "the routed service result replaces the opening frame with actual GPU provenance");
    }

    const auto frame = controller.state().frame;
    expectations.expect(frame != nullptr, "the controller published a display frame");
    if (frame == nullptr) {
        controller.beginShutdown();
        service.beginShutdown();
        bridge.beginShutdown();
        return;
    }
    const auto view = frame->displayBufferView();
    expectations.expect(view.has_value() && view->pixels.size() == std::size_t{1920} * 1080,
                        "the 1080p display buffer has valid selection geometry");

    if (ready) {
        expectations.expect(frame->provenance().provider ==
                                runtime::PreviewDisplayProvider::GpuNeutral,
                            "the application frame carries actual GPU Neutral provenance");
        // CPU oracle from the same plan/time/identity, evaluated once and mapped by the qualified
        // CPU display preparer.
        const auto processIdentity = frame->processIdentity();
        auto oracleFrame =
            fixture.evaluator.evaluate(processIdentity.plan,
                                       {.time = processIdentity.time,
                                        .output = processIdentity.output,
                                        .resolution = processIdentity.resolution,
                                        .quality = processIdentity.quality,
                                        .colorIntent = processIdentity.colorIntent,
                                        .pixelStorageByteLimit = kBudget,
                                        .roi = processIdentity.roi,
                                        .bypassLookNodes = processIdentity.bypassLookNodes},
                                       runtime::CancellationToken{});
        expectations.expect(oracleFrame.status() == runtime::EvaluationStatus::Evaluated,
                            "the CPU oracle evaluates the same plan");
        if (oracleFrame.frame() != nullptr) {
            runtime::CpuQualifiedDisplayPreparer preparer(*fixture.provider.handle());
            runtime::QualifiedDisplayPreparationRequest request;
            request.aggregatePixelStorageByteLimit = kBudget;
            auto prepared =
                preparer.prepare(oracleFrame.frame(), request, runtime::CancellationToken{});
            auto cpuFrame = runtime::PreparedPreviewFrame::createQualified(
                frame->desiredIdentity().requestGeneration, prepared.frame());
            expectations.expect(cpuFrame.has_value(), "the CPU oracle display frame builds");
            if (cpuFrame.has_value()) {
                expectations.expect(
                    pixelsMatchCpuOracle(*frame, *cpuFrame),
                    "GPU display pixels match the CPU oracle within 1 RGB code, alpha exact");
                const auto gpuBounds = frame->evaluatedBounds();
                const auto cpuBounds = cpuFrame->evaluatedBounds();
                expectations.expect(
                    gpuBounds.size() == cpuBounds.size() &&
                        std::equal(gpuBounds.begin(), gpuBounds.end(), cpuBounds.begin()),
                    "GPU evaluated bounds match the CPU oracle (content/output "
                    "vectors and layer IDs, not just size)");
            }
        }
    } else {
        expectations.expect(frame->provenance().provider !=
                                runtime::PreviewDisplayProvider::GpuNeutral,
                            "without a device the frame is the honest CPU result");
    }

    // Re-request the live cached frame: a different time evaluates through the service, then the
    // original time is answered by the frame cache with zero extra preparation.
    const int callsBeforeNewTime = serviceCalls->load();
    expectations.expect(session.setCurrentTime(time(1, 25)) &&
                            waitUntil([&] { return isReady(controller); }),
                        "a different time evaluates through the service");
    const int callsAfterNewTime = serviceCalls->load();
    const int evaluationsAfterNewTime = fixture.stageEvaluations.load();
    expectations.expect(callsAfterNewTime > callsBeforeNewTime,
                        "the uncached time submitted a real request");
    expectations.expect(session.setCurrentTime(time(0)) &&
                            waitUntil([&] { return isReady(controller); }),
                        "returning to the cached time publishes the retained frame");
    expectations.expect(serviceCalls->load() == callsAfterNewTime &&
                            fixture.stageEvaluations.load() == evaluationsAfterNewTime,
                        "the live cached frame is re-served with zero extra preparation");
    expectations.expect(fixture.stageEvaluations.load() == serviceCalls->load(),
                        "one stage evaluation per submitted request (no duplicate evaluation)");

    if (ready) {
        // Layout-only node move plus a work-area shrink that retains the current time must leave
        // the retained native GPU frame valid with zero extra service or stage work.
        const auto nodeId = session.composition()->graph().nodes().front().id;
        const int callsBeforeLayout = serviceCalls->load();
        const int evaluationsBeforeLayout = fixture.stageEvaluations.load();
        const auto retainedFrame = controller.state().frame;

        commands::Transaction move("Move Nodes", session.snapshot().revision());
        move.emplace<commands::MoveNodes>(
            compositionId, std::map<document::NodeId, document::Vec2d>{{nodeId, {6.0, 7.0}}});
        expectations.expect(session.executeNodeTransaction(std::move(move)).changed(),
                            "the layout-only node move publishes");

        commands::Transaction range("Set Work Area", session.snapshot().revision());
        const auto current = session.currentTime();
        range.emplace<commands::SetWorkArea>(compositionId, current, time(1));
        expectations.expect(session.executeTransaction(std::move(range)).changed(),
                            "the work-area shrink publishes");
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

        expectations.expect(controller.state().frame != nullptr &&
                                controller.state().frame->displayBufferView().has_value() &&
                                controller.state().frame->provenance().provider ==
                                    runtime::PreviewDisplayProvider::GpuNeutral,
                            "the retained native frame stays valid with GPU provenance after a "
                            "layout-only move and work-area shrink");
        expectations.expect(serviceCalls->load() == callsBeforeLayout &&
                                fixture.stageEvaluations.load() == evaluationsBeforeLayout,
                            "the layout-only edits cause no extra service call or evaluation");
        expectations.expect(
            controller.state().frame == retainedFrame,
            "the retained native frame object is unchanged by the layout-only edits");
    }

    controller.beginShutdown();
    service.beginShutdown();
    bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                        "the application pipeline reaches scheduler quiescence");
}

// Gate 2b: a missing loader is an explicit CPU fallback, evaluated once, without a crash.
void testMissingLoaderCpuFallback(Expectations& expectations) {
    auto newProject = makeViewerProject();
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.4, 0.8, 1.0}),
        "the fallback fixture adds a solid layer");
    runtime::TaskScheduler scheduler(serviceSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    AppFixture fixture;
    auto serviceCalls = std::make_shared<std::atomic<int>>(0);
    runtime::GpuPreviewDisplayService service(scheduler, fixture.stage(), fixture.fallback(),
                                              gpuOptions("/nonexistent/bloom-loader.so"));
    expectations.expect(
        waitUntil([&] {
            return service.status().state != runtime::GpuPreviewDisplayServiceState::Initializing;
        }) &&
            service.status().state == runtime::GpuPreviewDisplayServiceState::Unavailable,
        "a missing loader reports Unavailable");
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        ui::makeCompositionPreviewPipeline(fixture.compiler, fixture.evaluator,
                                           fixture.referencePreparer, fixture.provider,
                                           fixture.planCache),
        {.colorIntent = session.colorIntent(),
         .displayName = {},
         .viewName = {},
         .showLook = true,
         .pixelStorageByteLimit = kBudget},
        std::make_shared<ui::PreviewFrameCache>(), nullptr,
        serviceSubmitter(service, serviceCalls));
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the missing-loader path still publishes a frame");
    expectations.expect(controller.state().frame != nullptr &&
                            controller.state().frame->displayBufferView().has_value(),
                        "the missing-loader fallback published a real CPU display frame");
    expectations.expect(
        serviceCalls->load() >= 1 && fixture.stageEvaluations.load() == serviceCalls->load(),
        "the fallback evaluated each request exactly once (no duplicate evaluation)");
    controller.beginShutdown();
    service.beginShutdown();
    bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                        "the missing-loader path quiesces");
}

// Gate 3: warm-cache paired timing through the service seam, including handoff until result.
void testPipelineTiming(Expectations& expectations, const Options& options) {
    auto newProject = makeViewerProject();
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.4, 0.8, 1.0}),
        "the timing fixture adds a solid layer");
    runtime::TaskScheduler scheduler(serviceSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    AppFixture fixture;
    const auto snapshot = session.evaluationSnapshotForTime(time(0));

    runtime::GpuPreviewDisplayService gpuService(scheduler, fixture.stage(), fixture.fallback(),
                                                 gpuOptions(options.loader));
    const bool ready = waitUntil([&] {
                           return gpuService.status().state !=
                                  runtime::GpuPreviewDisplayServiceState::Initializing;
                       }) &&
                       gpuService.status().state == runtime::GpuPreviewDisplayServiceState::Ready;
    if (!ready) {
        expectations.expect(!options.requireDevice, "the timing gate requires a qualified device");
        return;
    }
    runtime::GpuPreviewDisplayServiceOptions cpuOptions;
    cpuOptions.enabled = false;
    runtime::GpuPreviewDisplayService cpuService(scheduler, fixture.stage(), fixture.fallback(),
                                                 cpuOptions);

    const auto runOnce = [&](runtime::GpuPreviewDisplayService& service,
                             const std::int64_t frameIndex)
        -> std::optional<std::pair<double, runtime::PreviewDisplayProvider>> {
        runtime::PreviewRequestIdentity identity{
            .projectId = snapshot.project().id(),
            .compositionId = compositionId,
            .sourceRevision = snapshot.revision(),
            .requestGeneration = static_cast<std::uint64_t>(frameIndex + 1),
            .time = time(frameIndex, 25),
            .output = runtime::PreviewOutput::Composition,
            .resolution = runtime::CompositionFormatResolution{},
            .quality = runtime::EvaluationQuality::Reference,
            .colorIntent = session.colorIntent(),
            .resolutionPolicy = runtime::PreviewResolutionPolicy::Auto,
            .viewAdjust = runtime::ViewAdjust{},
            .displayName = {},
            .viewName = {},
            .showLook = true};
        const auto start = std::chrono::steady_clock::now();
        auto submission =
            service.submit(runtime::TaskRequest("app-timing",
                                                {.kind = runtime::TaskOwnerKind::Composition,
                                                 .id = runtime::TaskOwnerId::fromRaw(1)},
                                                runtime::TaskPriority::Visible),
                           snapshot, identity, kBudget, {});
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto result = submission.handle.tryTakeResult()) {
                if (result->state() != runtime::TaskState::Succeeded ||
                    !result->value().has_value() || result->value().value()->frame() == nullptr) {
                    return std::nullopt;
                }
                return std::make_pair(std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - start)
                                          .count(),
                                      result->value().value()->frame()->provenance().provider);
            }
            std::this_thread::sleep_for(200us);
        }
        return std::nullopt;
    };

    for (std::int64_t frame = 0; frame < 5; ++frame) {
        static_cast<void>(runOnce(gpuService, frame));
        static_cast<void>(runOnce(cpuService, frame));
    }
    std::vector<double> gpuSamples;
    std::vector<double> cpuSamples;
    const auto record =
        [&](const std::optional<std::pair<double, runtime::PreviewDisplayProvider>> value,
            const bool expectGpu, std::vector<double>& samples) {
            if (!value.has_value()) {
                expectations.expect(false, "a timed application sample succeeded");
                return;
            }
            if (expectGpu) {
                expectations.expect(value->second == runtime::PreviewDisplayProvider::GpuNeutral,
                                    "every timed GPU sample carries actual GpuNeutral provenance");
            } else {
                expectations.expect(value->second != runtime::PreviewDisplayProvider::GpuNeutral,
                                    "every timed CPU sample carries CPU provenance, not GPU");
            }
            samples.push_back(value->first);
        };
    for (int pair = 0; pair < 10; ++pair) {
        const std::int64_t frame = 10 + pair;
        if (pair % 2 == 0) {
            record(runOnce(gpuService, frame), true, gpuSamples);
            record(runOnce(cpuService, frame), false, cpuSamples);
        } else {
            record(runOnce(cpuService, frame), false, cpuSamples);
            record(runOnce(gpuService, frame), true, gpuSamples);
        }
    }
    cpuService.beginShutdown();
    gpuService.beginShutdown();
    bridge.beginShutdown();
    const auto median = [](std::vector<double> values) {
        if (values.empty()) {
            return 0.0;
        }
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    };
    const double gpuMedian = median(gpuSamples);
    const double cpuMedian = median(cpuSamples);
    expectations.expect(gpuSamples.size() == 10 && cpuSamples.size() == 10,
                        "every timed application request succeeded");
    std::cout << "app-timing 1920x1080: actual app CPU stage + service submit-to-result "
                 "(excludes Qt controller delivery/presentation), warm cache: service median="
              << gpuMedian << "ms CPU-forced median=" << cpuMedian
              << "ms (no whole-app FPS claim)\n";
    expectations.expect(gpuMedian > 0.0 && gpuMedian < cpuMedian,
                        "the 1080p GPU service path is faster than the CPU-forced service "
                        "(submit-to-result, no presentation claim)");
    expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                        "the timing path quiesces");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    const Options options = parseOptions(argc, argv);
    if (!options.valid) {
        return 2;
    }
    Expectations expectations;
    try {
        testControllerGpuPathAndParity(expectations, options);
        testMissingLoaderCpuFallback(expectations);
        testPipelineTiming(expectations, options);
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " GPU app expectation(s) failed\n";
        return 1;
    }
    std::cout << "gpu_preview_app_tests: all expectations passed\n";
    return 0;
}
