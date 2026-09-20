// Private lifecycle harness for the GPU preview display service test target. It is compiled into
// the service test executable only (not the library) and uses the real
// device/pipeline/qualification path for the bounded native-dispatch-deadline scenario and the
// paired benchmark. It fabricates no eligibility report.

#include "gpu_preview_display_service_private.hpp"

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/project.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_neutral_display.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_preview_display_product.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/qualified_display_preparation.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::runtime::detail {
namespace {

using namespace std::chrono_literals;
using bloom::core::Color4d;
using bloom::core::RationalTime;

constexpr std::size_t kBudget = std::size_t{1} << 28;
constexpr auto kProjectId = document::ProjectId::fromRaw(1);
constexpr auto kCompositionId = document::CompositionId::fromRaw(2);
constexpr auto kSolid = document::NodeId::fromRaw(10);
constexpr auto kLayerNode = document::NodeId::fromRaw(11);
constexpr auto kStack = document::NodeId::fromRaw(12);
constexpr auto kOutput = document::NodeId::fromRaw(13);
constexpr auto kLayer = document::LayerId::fromRaw(20);
constexpr auto kSlot = document::LayerSlotId::fromRaw(30);
constexpr auto kColorP = document::ParameterId::fromRaw(40);
constexpr auto kPosP = document::ParameterId::fromRaw(41);
constexpr auto kOpacityP = document::ParameterId::fromRaw(42);
constexpr auto kAnchorP = document::ParameterId::fromRaw(43);
constexpr auto kScaleP = document::ParameterId::fromRaw(44);
constexpr auto kRotP = document::ParameterId::fromRaw(45);
constexpr auto kBlendP = document::ParameterId::fromRaw(46);

[[nodiscard]] TaskDiagnostic failDiag(const char* code) {
    return TaskDiagnostic{.code = code,
                          .severity = DiagnosticSeverity::Error,
                          .summary = "GPU preview display test harness failure.",
                          .detail = {},
                          .suggestedAction = {}};
}

[[nodiscard]] TaskSchedulerConfig harnessConfig(const std::size_t cpuWorkers = 2) {
    TaskSchedulerConfig c;
    c.cpuWorkerCount = cpuWorkers;
    c.rowBandWorkerCount = kSerialRowBandWorkers;
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

[[nodiscard]] document::Snapshot makeSnapshot() {
    document::Document doc(document::Project(kProjectId, "gpu-preview-display-lifecycle"));
    return doc.snapshot();
}

[[nodiscard]] document::CompositionFormat format(const std::uint32_t w, const std::uint32_t h) {
    const auto value = document::CompositionFormat::create(w, h);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan> makePlan(const std::uint32_t w,
                                                                      const std::uint32_t h) {
    const auto compositionFormat = format(w, h);
    std::vector<CompiledOperation> ops;
    ops.emplace_back(CompiledSolid{kSolid,
                                   {kColorP, Color4d{1.0, 0.25, 0.5, 1.0}},
                                   {document::ParameterId::fromRaw(kSolid.value() * 100 + 1000),
                                    static_cast<double>(compositionFormat.width())},
                                   {document::ParameterId::fromRaw(kSolid.value() * 100 + 1001),
                                    static_cast<double>(compositionFormat.height())}});
    ops.emplace_back(CompiledLayerOutput{
        kLayerNode, kLayer, OperationIndex::fromRaw(0),
        CompiledVec2Parameter{kPosP, document::Vec2d{2.0, 1.0}},
        CompiledVec2Parameter{kAnchorP, document::kDefaultAnchor},
        CompiledVec2Parameter{kScaleP, document::kDefaultScale},
        CompiledScalarParameter{kRotP, document::kDefaultRotationDegrees},
        CompiledScalarParameter{kOpacityP, 1.0}, kBlendP, core::kDefaultBlendMode});
    ops.emplace_back(CompiledMerge{kStack, {{kSlot, kLayer, OperationIndex::fromRaw(1)}}});
    ops.emplace_back(CompiledCompositionOutput{kOutput, OperationIndex::fromRaw(2)});
    return std::make_shared<const CompiledCompositionPlan>(CompiledCompositionPlanDefinition{
        document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
        std::move(ops), OperationIndex::fromRaw(3)});
}

[[nodiscard]] std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> makeProcessor() {
    auto resolution = color::resolveBloomNeutralV1BuiltIn(
        color::OcioConfigLocatorKind::BloomBuiltIn, color::kBloomNeutralV1ConfigUri,
        color::kBloomNeutralV1ConfigDigest);
    if (!resolution.ready()) {
        return nullptr;
    }
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return nullptr;
    }
    auto built = color::buildBloomNeutralCpuDisplayProcessor(*resolved);
    if (!built) {
        return nullptr;
    }
    auto handle = std::move(built).takeHandle();
    if (!handle.has_value()) {
        return nullptr;
    }
    return std::make_shared<const color::PreparedCpuDisplayProcessorHandle>(std::move(*handle));
}

[[nodiscard]] std::shared_ptr<const ProcessFrame>
evaluatePlan(const std::shared_ptr<CpuCompositionEvaluator>& evaluator,
             const std::shared_ptr<const CompiledCompositionPlan>& plan, const std::size_t budget,
             OperationCacheStatistics* statistics = nullptr,
             CpuRowBandExecutor* rowBands = nullptr) {
    auto result = evaluator->evaluate(plan,
                                      {.time = RationalTime::fromInteger(0),
                                       .output = plan->output(),
                                       .resolution = CompositionFormatResolution{},
                                       .quality = EvaluationQuality::Reference,
                                       .colorIntent = EvaluationColorIntent::LinearRec709Scene,
                                       .pixelStorageByteLimit = budget},
                                      CancellationToken{}, {}, rowBands, statistics);
    return result.status() == EvaluationStatus::Evaluated ? result.frame() : nullptr;
}

[[nodiscard]] PreviewRequestIdentity makeIdentity(const CompiledCompositionPlan& plan,
                                                  const std::uint64_t generation) {
    return PreviewRequestIdentity{.projectId = plan.projectId(),
                                  .compositionId = plan.compositionId(),
                                  .sourceRevision = plan.sourceRevision(),
                                  .requestGeneration = generation,
                                  .time = RationalTime::fromInteger(0),
                                  .output = PreviewOutput::Composition,
                                  .resolution = CompositionFormatResolution{},
                                  .quality = EvaluationQuality::Reference,
                                  .colorIntent = EvaluationColorIntent::LinearRec709Scene,
                                  .resolutionPolicy = PreviewResolutionPolicy::Auto,
                                  .viewAdjust = ViewAdjust{},
                                  .displayName = {},
                                  .viewName = {},
                                  .showLook = true};
}

[[nodiscard]] PreviewCpuDisplayFallback
qualifiedFallback(std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> processor) {
    return [processor = std::move(processor)](
               const PreviewCpuStage& stage,
               TaskContext& context) -> TaskResult<PreviewPreparationResultHandle> {
        using R = TaskResult<PreviewPreparationResultHandle>;
        if (context.isCancellationRequested()) {
            return R::cancelled();
        }
        CpuQualifiedDisplayPreparer preparer(*processor);
        QualifiedDisplayPreparationRequest request;
        request.aggregatePixelStorageByteLimit = stage.pixelStorageByteLimit();
        auto prepared = preparer.prepare(stage.processFrame(), request, context.cancellation());
        if (prepared.status() != QualifiedDisplayPreparationStatus::Prepared ||
            prepared.frame() == nullptr) {
            return R::failed(failDiag("harness.cpu-display-failed"));
        }
        auto frame = PreparedPreviewFrame::createQualified(
            stage.desiredIdentity().requestGeneration, prepared.frame());
        if (!frame.has_value()) {
            return R::failed(failDiag("harness.cpu-frame-failed"));
        }
        auto result = PreviewPreparationResult::prepared(
            std::make_shared<const PreparedPreviewFrame>(std::move(*frame)));
        if (!result.has_value()) {
            return R::failed(failDiag("harness.cpu-result-failed"));
        }
        return R::succeeded(std::make_shared<const PreviewPreparationResult>(std::move(*result)));
    };
}

// The stage function selects the warm plan by request generation and evaluates it for real.
[[nodiscard]] PreviewCpuStageFunction
benchmarkStage(std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> processor,
               std::shared_ptr<const CompiledCompositionPlan> small,
               std::shared_ptr<const CompiledCompositionPlan> large,
               std::shared_ptr<CpuCompositionEvaluator> evaluator,
               std::shared_ptr<std::atomic<std::size_t>> cacheHits) {
    return [processor = std::move(processor), small = std::move(small), large = std::move(large),
            evaluator = std::move(evaluator), cacheHits = std::move(cacheHits)](
               const document::Snapshot&, const PreviewRequestIdentity& identity,
               const std::size_t limit, const std::vector<SnapshotParameterOverride>&,
               TaskContext& context) -> TaskResult<PreviewCpuStageOutcomeHandle> {
        using R = TaskResult<PreviewCpuStageOutcomeHandle>;
        if (context.isCancellationRequested()) {
            return R::cancelled();
        }
        const auto& plan = identity.requestGeneration == 2 ? large : small;
        OperationCacheStatistics statistics;
        auto frame = evaluatePlan(evaluator, plan, limit, &statistics, context.rowBandExecutor());
        cacheHits->fetch_add(statistics.hits, std::memory_order_relaxed);
        if (frame == nullptr) {
            return R::cancelled();
        }
        auto stage = std::make_shared<const PreviewCpuStage>(identity, frame, processor, limit,
                                                             std::vector<TaskDiagnostic>{});
        return R::succeeded(std::make_shared<const PreviewCpuStageOutcome>(
            PreviewCpuStageOutcome{PreviewCpuStageStatus::Evaluated, std::move(stage), {}}));
    };
}

[[nodiscard]] GpuPreviewDisplayServiceOptions gpuOptions(const std::filesystem::path& loader) {
    GpuPreviewDisplayServiceOptions options;
    options.enabled = true;
    options.loaderPath = loader;
    options.previewByteAllowance = kBudget;
    return options;
}

template <typename Value>
[[nodiscard]] std::optional<TaskResult<Value>>
awaitResult(const TaskHandle<Value>& handle, const std::chrono::milliseconds timeout = 15s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = handle.tryTakeResult()) {
            return result;
        }
        std::this_thread::sleep_for(500us);
    }
    return std::nullopt;
}

[[nodiscard]] bool waitQuiescent(TaskScheduler& scheduler,
                                 const std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (scheduler.isQuiescent()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return scheduler.isQuiescent();
}

[[nodiscard]] double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    if (values.size() % 2 == 1) {
        return values[middle];
    }
    return 0.5 * (values[middle - 1] + values[middle]);
}

} // namespace

NativeDeadlineScenarioResult runNativeDeadlineScenario(const std::filesystem::path& loader) {
    NativeDeadlineScenarioResult result;
    auto processor = makeProcessor();
    if (processor == nullptr) {
        result.message = "processor unavailable";
        return result;
    }
    render::GpuDeviceCreationOptions deviceOptions;
    deviceOptions.loader_path = loader;
    auto device = render::GpuDevice::create(deviceOptions);
    if (!device) {
        result.message = "device unavailable";
        return result;
    }
    auto pipeline = render::GpuNeutralDisplay::create(*device.device);
    if (!pipeline) {
        result.message = "pipeline unavailable";
        return result;
    }
    auto report = std::make_shared<const GpuNeutralDisplayQualificationReport>(
        qualifyGpuNeutralDisplay(*processor, *device.device, *pipeline.display));
    if (!report->eligible()) {
        result.message = "qualification not eligible";
        return result;
    }

    static constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 4> kSizes{
        {{1920, 1080}, {1280, 720}, {960, 540}, {256, 144}}};
    const auto eligibleInterval = report->eligibleInterval();
    if (!eligibleInterval.has_value()) {
        result.message = "qualification interval unavailable";
        return result;
    }
    std::optional<std::pair<std::uint32_t, std::uint32_t>> size;
    for (const auto& candidate : kSizes) {
        const auto pixels = static_cast<std::uint64_t>(candidate.first) * candidate.second;
        if (pixels >= eligibleInterval->min_pixels && pixels <= eligibleInterval->max_pixels) {
            size = candidate;
            break;
        }
    }
    if (!size.has_value()) {
        result.message = "no eligible size";
        return result;
    }
    const auto plan = makePlan(size->first, size->second);
    auto evaluator = std::make_shared<CpuCompositionEvaluator>();
    auto frame = evaluatePlan(evaluator, plan, kBudget);
    if (frame == nullptr) {
        result.message = "frame unavailable";
        return result;
    }

    TaskScheduler scheduler(harnessConfig(2));
    auto core = std::make_shared<PreviewDisplayServiceCore>();
    core->scheduler = &scheduler;
    core->options.previewByteAllowance = kBudget;
    // Mirror the production service constructor: the owner-resolved admission allowance is seeded
    // from the configured options before the native pump can read it. Without this the handcrafted
    // core reports a zero device allowance and the real pipeline refuses every dispatch as
    // OverBudget, so the deadline path would never be reached.
    core->effectivePreviewByteAllowance.store(kBudget, std::memory_order_relaxed);
    core->gpuAvailable = true;
    core->qualification = report;
    core->display = std::move(pipeline.display);
    core->fallback = [](const PreviewCpuStage&,
                        TaskContext&) -> TaskResult<PreviewPreparationResultHandle> {
        using R = TaskResult<PreviewPreparationResultHandle>;
        return R::succeeded(std::make_shared<const PreviewPreparationResult>(
            PreviewPreparationResult::unsupported()));
    };

    auto stage = std::make_shared<PreviewDisplayStageRecord>();
    stage->stage = std::make_shared<const PreviewCpuStage>(makeIdentity(*plan, 1), frame, processor,
                                                           kBudget, std::vector<TaskDiagnostic>{});
    stage->pixelStorageByteLimit = kBudget;
    // Mirror startGpuPreviewStage(): the accepted request carries both the host pixel-storage
    // ceiling and the device-stage admission allowance resolved from capacity.
    stage->requestOwnedBytes = kBudget;
    stage->gpuByteAllowance = kBudget;
    stage->owner = TaskOwner{.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(1)};
    stage->priority = TaskPriority::Visible;
    stage->phase = PreviewDisplayStageRecord::Phase::AwaitingNative;

    auto fakeNow = std::make_shared<std::chrono::steady_clock::time_point>();
    core->nativeClockOverride = [fakeNow] { return *fakeNow; };
    core->nativePollOverride = [](render::GpuNeutralDisplay&) {
        return render::GpuNeutralDisplayPollResult::Pending;
    };
    core->nativeReady.push_back(stage);

    processNativeDisplay(core);
    result.ran = true;
    result.dispatched = core->nativeInFlight;

    *fakeNow += core->options.nativeDispatchDeadline + std::chrono::milliseconds(1);
    processNativeDisplay(core);
    result.retired = core->display == nullptr && !core->gpuAvailable;
    result.timeoutDiagnostic =
        core->diagnostic.code == GpuPreviewDisplayServiceDiagnosticCode::NativeTimeout;
    result.fallbackDispatched = stage->fallbackChild.isValid();
    result.message = result.dispatched ? "ok" : "native job was not dispatched";

    scheduler.beginShutdown();
    static_cast<void>(waitQuiescent(scheduler));
    return result;
}

std::vector<PreviewDisplayBenchmarkSample>
runGpuPreviewDisplayBenchmark(const std::filesystem::path& loader) {
    std::vector<PreviewDisplayBenchmarkSample> samples;
    auto processor = makeProcessor();
    if (processor == nullptr) {
        return samples;
    }
    auto small = makePlan(640, 360);
    auto large = makePlan(1920, 1080);
    const auto snapshot = makeSnapshot();
    auto evaluator = std::make_shared<CpuCompositionEvaluator>();
    auto cacheHits = std::make_shared<std::atomic<std::size_t>>(0);
    const auto stage = benchmarkStage(processor, small, large, evaluator, cacheHits);
    const auto fallback = qualifiedFallback(processor);

    TaskScheduler scheduler(harnessConfig(4));
    GpuPreviewDisplayService gpuService(scheduler, stage, fallback, gpuOptions(loader));
    const bool terminal = [&] {
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (gpuService.status().state != GpuPreviewDisplayServiceState::Initializing) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }();
    if (!terminal || gpuService.status().state != GpuPreviewDisplayServiceState::Ready) {
        return samples;
    }
    GpuPreviewDisplayServiceOptions cpuOptions;
    cpuOptions.enabled = false;
    GpuPreviewDisplayService cpuService(scheduler, stage, fallback, cpuOptions);

    const auto sample = [&](GpuPreviewDisplayService& service,
                            const PreviewRequestIdentity& identity) -> std::optional<double> {
        const auto start = std::chrono::steady_clock::now();
        auto submission = service.submit(TaskRequest("benchmark",
                                                     TaskOwner{.kind = TaskOwnerKind::Composition,
                                                               .id = TaskOwnerId::fromRaw(1)},
                                                     TaskPriority::Visible),
                                         snapshot, identity, kBudget, {});
        auto result = awaitResult(submission.handle);
        if (!result.has_value() || result->state() != TaskState::Succeeded ||
            !result->value().has_value()) {
            return std::nullopt;
        }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    };
    const auto usesGpu = [&](const PreviewRequestIdentity& identity) -> bool {
        auto submission =
            gpuService.submit(TaskRequest("benchmark-probe",
                                          TaskOwner{.kind = TaskOwnerKind::Composition,
                                                    .id = TaskOwnerId::fromRaw(1)},
                                          TaskPriority::Visible),
                              snapshot, identity, kBudget, {});
        auto result = awaitResult(submission.handle);
        if (!result.has_value() || !result->value().has_value() ||
            result->value().value()->frame() == nullptr) {
            return false;
        }
        const auto displayOnly = result->value().value()->frame()->displayOnlyFrame();
        return displayOnly != nullptr &&
               displayOnly->provenance().provider == PreviewDisplayProvider::GpuNeutral;
    };

    for (const auto& size :
         std::array<std::pair<std::uint32_t, std::uint32_t>, 2>{{{640, 360}, {1920, 1080}}}) {
        PreviewDisplayBenchmarkSample sampleResult;
        sampleResult.width = size.first;
        sampleResult.height = size.second;
        const auto plan = size.second == 1080 ? large : small;
        const auto identity = makeIdentity(*plan, size.second == 1080 ? 2 : 1);
        sampleResult.usedGpu = usesGpu(identity);
        bool succeeded = true;
        for (int warmup = 0; warmup < 5; ++warmup) {
            succeeded = sample(gpuService, identity).has_value() && succeeded;
            succeeded = sample(cpuService, identity).has_value() && succeeded;
        }
        const std::size_t hitsBefore = cacheHits->load(std::memory_order_relaxed);
        std::vector<double> serviceSamples;
        std::vector<double> cpuSamples;
        // Alternating pairs: even pairs run service then CPU, odd pairs CPU then service.
        for (int pair = 0; pair < 10; ++pair) {
            if (pair % 2 == 0) {
                if (auto value = sample(gpuService, identity)) {
                    serviceSamples.push_back(*value);
                } else {
                    succeeded = false;
                }
                if (auto value = sample(cpuService, identity)) {
                    cpuSamples.push_back(*value);
                } else {
                    succeeded = false;
                }
            } else {
                if (auto value = sample(cpuService, identity)) {
                    cpuSamples.push_back(*value);
                } else {
                    succeeded = false;
                }
                if (auto value = sample(gpuService, identity)) {
                    serviceSamples.push_back(*value);
                } else {
                    succeeded = false;
                }
            }
        }
        const bool fullPairs = serviceSamples.size() == 10 && cpuSamples.size() == 10;
        sampleResult.serviceMedianMs = median(serviceSamples);
        sampleResult.cpuMedianMs = median(cpuSamples);
        sampleResult.cacheHits = cacheHits->load(std::memory_order_relaxed) - hitsBefore;
        sampleResult.succeeded = succeeded && fullPairs;
        sampleResult.ran = true;
        samples.push_back(sampleResult);
    }
    scheduler.beginShutdown();
    return samples;
}

std::shared_ptr<const ProcessFrame>
testEvaluateFrame(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                  const std::size_t budget) {
    auto evaluator = std::make_shared<CpuCompositionEvaluator>();
    return evaluatePlan(evaluator, plan, budget);
}

PreviewRequestIdentity testIdentity(const CompiledCompositionPlan& plan) {
    return makeIdentity(plan, 1);
}

PreviewDisplayServiceTestFixture::PreviewDisplayServiceTestFixture(const std::uint32_t width,
                                                                   const std::uint32_t height)
    : plan(makePlan(width, height)), evaluator(std::make_shared<CpuCompositionEvaluator>()),
      processor(makeProcessor()) {}

PreviewCpuStageFunction PreviewDisplayServiceTestFixture::stageFn() const {
    auto planPtr = plan;
    auto processorPtr = processor;
    auto evalCount = evaluations;
    auto evaluatorPtr = evaluator;
    return [planPtr, processorPtr, evalCount,
            evaluatorPtr](const document::Snapshot&, const PreviewRequestIdentity& identity,
                          const std::size_t limit, const std::vector<SnapshotParameterOverride>&,
                          TaskContext& context) -> TaskResult<PreviewCpuStageOutcomeHandle> {
        using R = TaskResult<PreviewCpuStageOutcomeHandle>;
        ++(*evalCount);
        if (context.isCancellationRequested()) {
            return R::cancelled();
        }
        auto frame = evaluatePlan(evaluatorPtr, planPtr, limit);
        if (frame == nullptr) {
            return R::cancelled();
        }
        auto stage = std::make_shared<const PreviewCpuStage>(identity, frame, processorPtr, limit,
                                                             std::vector<TaskDiagnostic>{});
        return R::succeeded(std::make_shared<const PreviewCpuStageOutcome>(
            PreviewCpuStageOutcome{PreviewCpuStageStatus::Evaluated, std::move(stage), {}}));
    };
}

PreviewCpuDisplayFallback PreviewDisplayServiceTestFixture::fallback() const {
    auto inner = qualifiedFallback(processor);
    auto fallbackCount = fallbacks;
    return [inner = std::move(inner),
            fallbackCount](const PreviewCpuStage& stage,
                           TaskContext& context) -> TaskResult<PreviewPreparationResultHandle> {
        ++(*fallbackCount);
        return inner(stage, context);
    };
}

} // namespace bloom::runtime::detail
