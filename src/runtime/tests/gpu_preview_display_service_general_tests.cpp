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
               const std::size_t limit, const std::vector<bloom::runtime::SnapshotParameterOverride>&,
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
                PreviewGpuSceneStageOutcome{PreviewGpuSceneStageStatus::UnsupportedGpuSubset, nullptr,
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
            auto prepared = programService->prepare(binding, width, height, identity.viewAdjust,
                                                    cancel);
            if (prepared.error == bloom::runtime::GpuDisplayProgramError::Cancelled) {
                return R::cancelled();
            }
            if (prepared.hasValue() &&
                bloom::runtime::gpuDisplayProgramMatchesRequest(prepared.program, binding)) {
                program = std::make_shared<const GpuDisplayProgram>(std::move(prepared.program));
            }
        }
        auto stage = std::make_shared<const PreviewGpuSceneStage>(
            identity, built.scene, processor, std::move(program), limit,
            std::vector<TaskDiagnostic>{});
        return R::succeeded(std::make_shared<const PreviewGpuSceneStageOutcome>(
            PreviewGpuSceneStageOutcome{PreviewGpuSceneStageStatus::Prepared, std::move(stage), {}}));
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

[[nodiscard]] PreviewCpuDisplayFallback generalFallback(
    std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle> processor) {
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
[[nodiscard]] bool isResidentPrepared(const std::optional<TaskResult<PreviewPreparationResultHandle>>& r,
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
    if (result.frame()->provenance().provider != bloom::runtime::PreviewDisplayProvider::GpuResident) {
        return false;
    }
    out = result.frame();
    return true;
}

} // namespace

int main(const int argc, char** argv) {
    const TestOptions options = parseOptions(argc, argv);
    if (!options.valid) {
        return 2;
    }
#ifndef BLOOM_GPUSHADER_TOOLS_DIR
    std::cout << "SKIP: BLOOM_GPUSHADER_TOOLS_DIR is not set\n";
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

    // The production service shape: a lazy provider that resolves the packaged tools on first use.
    auto programService = std::make_shared<const GpuDisplayProgramService>(
        []() -> GpuOcioCompileOptions {
            GpuOcioCompileOptions options;
            options.glslangValidatorPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
            options.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
            return options;
        });
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
