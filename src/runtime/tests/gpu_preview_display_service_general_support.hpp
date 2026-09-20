#pragma once

// Shared harness for the general-display service acceptance tests. Included by the main service
// test and the forced Neutral-qualification gate test; it defines the plan/identity/stage builders,
// the resolver request, and the await helpers. It contains no main().

#include "gpu_preview_display_service_private.hpp"

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/project.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>
#include <bloom/runtime/gpu_ocio_display_arm.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/preview_cpu_stage.hpp>
#include <bloom/runtime/preview_gpu_scene_stage.hpp>
#include <bloom/runtime/qualified_display_preparation.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
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
using bloom::core::Color4d;
using bloom::core::RationalTime;
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::EvaluationColorIntent;
using bloom::runtime::EvaluationQuality;
using bloom::runtime::EvaluationRequest;
using bloom::runtime::GpuDisplayProgram;
using bloom::runtime::GpuDisplayProgramError;
using bloom::runtime::GpuDisplayProgramService;
using bloom::runtime::GpuOcioCompileOptions;
using bloom::runtime::GpuOcioContextRequest;
using bloom::runtime::GpuOcioContextResolver;
using bloom::runtime::GpuPreviewDisplayService;
using bloom::runtime::GpuPreviewDisplayServiceOptions;
using bloom::runtime::GpuPreviewDisplayServicePresentationMode;
using bloom::runtime::GpuPreviewDisplayServiceState;
using bloom::runtime::PreviewCpuDisplayFallback;
using bloom::runtime::PreviewCpuStage;
using bloom::runtime::PreviewCpuStageFunction;
using bloom::runtime::PreviewCpuStageOutcome;
using bloom::runtime::PreviewCpuStageOutcomeHandle;
using bloom::runtime::PreviewCpuStageStatus;
using bloom::runtime::PreviewGpuSceneStage;
using bloom::runtime::PreviewGpuSceneStageFunction;
using bloom::runtime::PreviewGpuSceneStageOutcome;
using bloom::runtime::PreviewGpuSceneStageOutcomeHandle;
using bloom::runtime::PreviewGpuSceneStageStatus;
using bloom::runtime::PreviewPreparationResult;
using bloom::runtime::PreviewPreparationResultHandle;
using bloom::runtime::PreviewRequestIdentity;
using bloom::runtime::PreviewResolutionPolicy;
using bloom::runtime::TaskContext;
using bloom::runtime::TaskDiagnostic;
using bloom::runtime::TaskHandle;
using bloom::runtime::TaskRequest;
using bloom::runtime::TaskResult;
using bloom::runtime::TaskScheduler;
using bloom::runtime::TaskSchedulerConfig;
using bloom::runtime::TaskState;
using bloom::runtime::TaskSubmissionStatus;
using bloom::runtime::ViewAdjust;

constexpr std::size_t kBudget = std::size_t{1} << 30;
constexpr int kSkipExit = 77;
constexpr auto kProjectId = bloom::document::ProjectId::fromRaw(1);
constexpr auto kCompositionId = bloom::document::CompositionId::fromRaw(2);
constexpr auto kSolid = bloom::document::NodeId::fromRaw(10);
constexpr auto kLayerNode = bloom::document::NodeId::fromRaw(11);
constexpr auto kStack = bloom::document::NodeId::fromRaw(12);
constexpr auto kOutput = bloom::document::NodeId::fromRaw(13);
constexpr auto kLayer = bloom::document::LayerId::fromRaw(20);
constexpr auto kSlot = bloom::document::LayerSlotId::fromRaw(30);
constexpr auto kColorP = bloom::document::ParameterId::fromRaw(40);
constexpr auto kPosP = bloom::document::ParameterId::fromRaw(41);
constexpr auto kOpacityP = bloom::document::ParameterId::fromRaw(42);
constexpr auto kAnchorP = bloom::document::ParameterId::fromRaw(43);
constexpr auto kScaleP = bloom::document::ParameterId::fromRaw(44);
constexpr auto kRotP = bloom::document::ParameterId::fromRaw(45);
constexpr auto kBlendP = bloom::document::ParameterId::fromRaw(46);

struct TestOptions final {
    std::filesystem::path loaderPath;
    bool requireDevice = false;
    bool valid = true;
};

[[nodiscard]] TestOptions parseOptions(const int argc, char** argv) {
    TestOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader" && index + 1 < argc) {
            options.loaderPath = argv[++index];
        } else if (argument == "--require-device") {
            options.requireDevice = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            options.valid = false;
            return options;
        }
    }
    return options;
}

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

[[nodiscard]] TaskSchedulerConfig schedulerConfig() {
    TaskSchedulerConfig config;
    config.cpuWorkerCount = 2;
    config.rowBandWorkerCount = bloom::runtime::kSerialRowBandWorkers;
    config.blockingIoWorkerCount = 1;
    config.cpuQueueCapacity = 64;
    config.blockingIoQueueCapacity = 8;
    config.gpuPendingQueueCapacity = 8;
    config.gpuAdmittedStateCapacity = 8;
    config.gpuLiveContinuationCapacity = 4;
    config.gpuQueuedCommandByteCapacity = std::size_t{1} << 30U;
    config.gpuRequestOwnedByteCapacity = std::size_t{1} << 30U;
    config.terminalHistoryCapacity = 64;
    config.diagnosticsPerTask = 16;
    config.groupRegistryCapacity = 16;
    return config;
}

[[nodiscard]] bloom::document::CompositionFormat format(const std::uint32_t w,
                                                        const std::uint32_t h) {
    const auto value = bloom::document::CompositionFormat::create(w, h);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

[[nodiscard]] std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>
makePlan(const std::uint32_t w, const std::uint32_t h) {
    const auto compositionFormat = format(w, h);
    std::vector<bloom::runtime::CompiledOperation> ops;
    ops.emplace_back(bloom::runtime::CompiledSolid{
        kSolid,
        {kColorP, Color4d{1.0, 0.25, 0.5, 1.0}},
        {bloom::document::ParameterId::fromRaw(kSolid.value() * 100 + 1000),
         static_cast<double>(compositionFormat.width())},
        {bloom::document::ParameterId::fromRaw(kSolid.value() * 100 + 1001),
         static_cast<double>(compositionFormat.height())}});
    ops.emplace_back(bloom::runtime::CompiledLayerOutput{
        kLayerNode, kLayer, bloom::runtime::OperationIndex::fromRaw(0),
        bloom::runtime::CompiledVec2Parameter{kPosP, bloom::document::Vec2d{2.0, 1.0}},
        bloom::runtime::CompiledVec2Parameter{kAnchorP, bloom::document::kDefaultAnchor},
        bloom::runtime::CompiledVec2Parameter{kScaleP, bloom::document::kDefaultScale},
        bloom::runtime::CompiledScalarParameter{kRotP, bloom::document::kDefaultRotationDegrees},
        bloom::runtime::CompiledScalarParameter{kOpacityP, 1.0}, kBlendP,
        bloom::core::kDefaultBlendMode});
    ops.emplace_back(bloom::runtime::CompiledMerge{
        kStack, {{kSlot, kLayer, bloom::runtime::OperationIndex::fromRaw(1)}}});
    ops.emplace_back(bloom::runtime::CompiledCompositionOutput{
        kOutput, bloom::runtime::OperationIndex::fromRaw(2)});
    return std::make_shared<const bloom::runtime::CompiledCompositionPlan>(
        bloom::runtime::CompiledCompositionPlanDefinition{
            bloom::document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
            std::move(ops), bloom::runtime::OperationIndex::fromRaw(3)});
}

[[nodiscard]] PreviewRequestIdentity
makeIdentity(const bloom::runtime::CompiledCompositionPlan& plan, const std::uint64_t generation,
             std::string display, std::string view, const ViewAdjust adjust,
             const EvaluationColorIntent intent = EvaluationColorIntent::LinearRec709Scene) {
    return PreviewRequestIdentity{.projectId = plan.projectId(),
                                  .compositionId = plan.compositionId(),
                                  .sourceRevision = plan.sourceRevision(),
                                  .requestGeneration = generation,
                                  .time = RationalTime::fromInteger(0),
                                  .output = bloom::runtime::PreviewOutput::Composition,
                                  .resolution = bloom::runtime::CompositionFormatResolution{},
                                  .quality = EvaluationQuality::Reference,
                                  .colorIntent = intent,
                                  .resolutionPolicy = PreviewResolutionPolicy::Auto,
                                  .viewAdjust = adjust,
                                  .displayName = std::move(display),
                                  .viewName = std::move(view),
                                  .showLook = true};
}

[[nodiscard]] std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle>
makeNeutralProcessor(const bloom::color::ResolvedBloomNeutralConfig& config) {
    auto built = bloom::color::buildBloomNeutralCpuDisplayProcessor(config);
    if (!built) {
        return nullptr;
    }
    auto handle = std::move(built).takeHandle();
    if (!handle.has_value()) {
        return nullptr;
    }
    return std::make_shared<const bloom::color::PreparedCpuDisplayProcessorHandle>(
        std::move(*handle));
}

[[nodiscard]] TaskDiagnostic failDiag(const char* code) {
    return TaskDiagnostic{.code = code,
                          .severity = bloom::runtime::DiagnosticSeverity::Error,
                          .summary = "general display service acceptance harness failure.",
                          .detail = {},
                          .suggestedAction = {}};
}

// The injected GPU-scene stage function: a real CpuGpuSceneBuilder scene plus an off-UI-prepared
// general display program for the request's own project color binding (config URI, revision,
// working space, display/view). It performs no manual preparer switch: the binding is derived from
// the request identity exactly as the production stage does.
[[nodiscard]] PreviewGpuSceneStageFunction generalStageFunction(
    std::shared_ptr<const bloom::runtime::CompiledCompositionPlan> plan,
    std::shared_ptr<CpuGpuSceneBuilder> builder,
    std::shared_ptr<const GpuDisplayProgramService> programService,
    std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle> processor) {
    return [plan = std::move(plan), builder = std::move(builder),
            programService = std::move(programService), processor = std::move(processor)](
               const bloom::document::Snapshot&, const PreviewRequestIdentity& identity,
               const std::size_t limit,
               const std::vector<bloom::runtime::SnapshotParameterOverride>&,
               TaskContext& context) -> TaskResult<PreviewGpuSceneStageOutcomeHandle> {
        using R = TaskResult<PreviewGpuSceneStageOutcomeHandle>;
        if (context.isCancellationRequested()) {
            return R::cancelled();
        }
        EvaluationRequest request{.time = identity.time,
                                  .output = plan->output(),
                                  .resolution = bloom::runtime::CompositionFormatResolution{},
                                  .quality = EvaluationQuality::Reference,
                                  .colorIntent = EvaluationColorIntent::LinearRec709Scene,
                                  .pixelStorageByteLimit = limit};
        auto built = builder->build(plan, request, context.cancellation());
        if (!built.hasValue() || built.scene == nullptr) {
            return R::succeeded(std::make_shared<const PreviewGpuSceneStageOutcome>(
                PreviewGpuSceneStageOutcome{PreviewGpuSceneStageStatus::UnsupportedGpuSubset,
                                            nullptr,
                                            {failDiag("harness.scene-build-refused")}}));
        }
        const auto& descriptor = built.scene->outputDescriptor();
        const auto width = descriptor.dataWindow().extent().width();
        const auto height = descriptor.dataWindow().extent().height();
        std::shared_ptr<const GpuDisplayProgram> program;
        if (programService != nullptr) {
            const auto binding = bloom::runtime::gpuDisplayColorBindingForIntent(
                identity.colorIntent, identity.displayName, identity.viewName);
            bloom::runtime::GpuOcioCancellation cancel = [&context] {
                return context.isCancellationRequested();
            };
            auto prepared =
                programService->prepare(binding, width, height, identity.viewAdjust, cancel);
            if (prepared.error == bloom::runtime::GpuDisplayProgramError::Cancelled) {
                return R::cancelled();
            }
            if (prepared.hasValue() &&
                bloom::runtime::gpuDisplayProgramMatchesRequest(prepared.program, binding)) {
                program = std::make_shared<const GpuDisplayProgram>(std::move(prepared.program));
            }
        }
        auto stage = std::make_shared<const PreviewGpuSceneStage>(identity, built.scene, processor,
                                                                  std::move(program), limit,
                                                                  std::vector<TaskDiagnostic>{});
        return R::succeeded(
            std::make_shared<const PreviewGpuSceneStageOutcome>(PreviewGpuSceneStageOutcome{
                PreviewGpuSceneStageStatus::Prepared, std::move(stage), {}}));
    };
}

// The full original CPU path used by the resident fallback.
[[nodiscard]] PreviewCpuStageFunction generalCpuStageFunction(
    std::shared_ptr<const bloom::runtime::CompiledCompositionPlan> plan,
    std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle> processor,
    std::shared_ptr<CpuCompositionEvaluator> evaluator) {
    return [plan = std::move(plan), processor = std::move(processor),
            evaluator = std::move(evaluator)](
               const bloom::document::Snapshot&, const PreviewRequestIdentity& identity,
               const std::size_t limit,
               const std::vector<bloom::runtime::SnapshotParameterOverride>&,
               TaskContext& context) -> TaskResult<PreviewCpuStageOutcomeHandle> {
        using R = TaskResult<PreviewCpuStageOutcomeHandle>;
        if (context.isCancellationRequested()) {
            return R::cancelled();
        }
        auto result =
            evaluator->evaluate(plan,
                                {.time = identity.time,
                                 .output = plan->output(),
                                 .resolution = bloom::runtime::CompositionFormatResolution{},
                                 .quality = EvaluationQuality::Reference,
                                 .colorIntent = EvaluationColorIntent::LinearRec709Scene,
                                 .pixelStorageByteLimit = limit},
                                context.cancellation());
        if (result.status() != bloom::runtime::EvaluationStatus::Evaluated ||
            result.frame() == nullptr) {
            return R::cancelled();
        }
        auto stage = std::make_shared<const PreviewCpuStage>(identity, result.frame(), processor,
                                                             limit, std::vector<TaskDiagnostic>{});
        return R::succeeded(std::make_shared<const PreviewCpuStageOutcome>(
            PreviewCpuStageOutcome{PreviewCpuStageStatus::Evaluated, std::move(stage), {}}));
    };
}

[[nodiscard]] PreviewCpuDisplayFallback
generalFallback(std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle> processor) {
    return [processor = std::move(processor)](
               const PreviewCpuStage& stage,
               TaskContext& context) -> TaskResult<PreviewPreparationResultHandle> {
        using R = TaskResult<PreviewPreparationResultHandle>;
        if (context.isCancellationRequested()) {
            return R::cancelled();
        }
        bloom::runtime::CpuQualifiedDisplayPreparer preparer(*processor);
        bloom::runtime::QualifiedDisplayPreparationRequest request;
        request.aggregatePixelStorageByteLimit = stage.pixelStorageByteLimit();
        auto prepared = preparer.prepare(stage.processFrame(), request, context.cancellation());
        if (prepared.status() != bloom::runtime::QualifiedDisplayPreparationStatus::Prepared ||
            prepared.frame() == nullptr) {
            return R::failed(std::vector<TaskDiagnostic>{failDiag("harness.cpu-display-failed")});
        }
        auto frame = bloom::runtime::PreparedPreviewFrame::createQualified(
            stage.desiredIdentity().requestGeneration, prepared.frame());
        if (!frame.has_value()) {
            return R::failed(std::vector<TaskDiagnostic>{failDiag("harness.cpu-frame-failed")});
        }
        auto result = PreviewPreparationResult::prepared(
            std::make_shared<const bloom::runtime::PreparedPreviewFrame>(std::move(*frame)));
        if (!result.has_value()) {
            return R::failed(std::vector<TaskDiagnostic>{failDiag("harness.cpu-result-failed")});
        }
        return R::succeeded(std::make_shared<const PreviewPreparationResult>(std::move(*result)));
    };
}

[[nodiscard]] GpuPreviewDisplayServiceOptions serviceOptions(const std::filesystem::path& loader) {
    GpuPreviewDisplayServiceOptions options;
    options.enabled = true;
    options.loaderPath = loader;
    options.presentation = GpuPreviewDisplayServicePresentationMode::Wayland;
    options.previewByteAllowance = kBudget;
    options.presentationCoordinator.maxTargets = 3U;
    options.presentationCoordinator.maxRetainedTargets = 64U;
    options.presentationCoordinator.shutdownDrainPumps = 600U;
    options.residentLeaseBudgets.maxBytes = std::size_t{1} << 30U;
    options.residentExecutorBudgets.maxImageBytes = std::size_t{1} << 28U;
    return options;
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(500us);
    }
    return predicate();
}

template <typename Value>
[[nodiscard]] std::optional<TaskResult<Value>>
awaitResult(const TaskHandle<Value>& handle, const std::chrono::milliseconds timeout = 60s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = handle.tryTakeResult()) {
            return result;
        }
        std::this_thread::sleep_for(500us);
    }
    return std::nullopt;
}

// True when the result carries a genuine resident frame on the requested generation.
[[nodiscard]] bool
isResidentPrepared(const std::optional<TaskResult<PreviewPreparationResultHandle>>& r,
                   const std::uint64_t generation,
                   std::shared_ptr<const bloom::runtime::PreparedPreviewFrame>& out) {
    if (!r.has_value() || r->state() != TaskState::Succeeded || !r->value().has_value()) {
        return false;
    }
    const auto& result = **r->value();
    if (result.status() != bloom::runtime::PreviewPreparationStatus::Prepared ||
        result.frame() == nullptr ||
        result.frame()->desiredIdentity().requestGeneration != generation) {
        return false;
    }
    if (result.frame()->provenance().provider !=
        bloom::runtime::PreviewDisplayProvider::GpuResident) {
        return false;
    }
    out = result.frame();
    return true;
}

} // namespace
