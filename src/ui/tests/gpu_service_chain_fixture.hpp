#pragma once

// Cohesive, test-only fixture for the GPU resident service-chain acceptance.
//
// It owns the real evaluator/compiler/qualified-provider and the ONE real GPU scene builder whose
// media context is derived from the evaluator (so a relative media path resolves against the live
// asset base directory). It exposes the UNMODIFIED production stage factories:
//   * ui::makeCompositionPreviewGpuSceneStage(compiler, builder, provider)
//   * ui::makeCompositionPreviewCpuStage(compiler, evaluator, provider)
//   * ui::makeCompositionPreviewCpuDisplayFallback(displayPreparer)
// so every request -- supported or not, solid or media -- is compiled by production code from the
// immutable document snapshot it is given. There are no test generation branches and no
// hand-lowered plan presented as a document.
//
// It is compiled only into the GPU vertical acceptance tests. No production TU includes it.

#include "gpu_vertical_proof_support.hpp"

#include "gpu_preview_display_service_private.hpp"

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/document/document.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_presentation_coordinator.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/gpu_resident_frame_lease.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/preview_cpu_stage.hpp>
#include <bloom/runtime/qualified_display_preparation.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_gpu_scene_stage.hpp>

#include <QGuiApplication>

#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::ui::verticalproof {

using namespace std::chrono_literals;
using bloom::core::Color4d;
using bloom::core::RationalTime;
using bloom::render::GpuBorrowedInstanceView;
using bloom::render::GpuBorrowedSurface;
using bloom::render::GpuPresentBackground;
using bloom::render::GpuPresentChannel;
using bloom::render::GpuPresentRect;
using bloom::render::GpuPresentSourceWindow;
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::CpuQualifiedDisplayPreparer;
using bloom::runtime::EvaluationColorIntent;
using bloom::runtime::EvaluationQuality;
using bloom::runtime::EvaluationRequest;
using bloom::runtime::GpuPresentationClient;
using bloom::runtime::GpuPresentationPortCode;
using bloom::runtime::GpuPresentationTargetState;
using bloom::runtime::GpuPresentationUpdate;
using bloom::runtime::GpuPreviewDisplayService;
using bloom::runtime::GpuPreviewDisplayServiceOptions;
using bloom::runtime::GpuPreviewDisplayServicePresentationMode;
using bloom::runtime::GpuPreviewDisplayServiceState;
using bloom::runtime::GpuResidentFrameLease;
using bloom::runtime::PreviewCpuDisplayFallback;
using bloom::runtime::PreviewCpuStage;
using bloom::runtime::PreviewCpuStageFunction;
using bloom::runtime::PreviewCpuStageOutcome;
using bloom::runtime::PreviewCpuStageOutcomeHandle;
using bloom::runtime::PreviewDisplayProvider;
using bloom::runtime::PreviewGpuSceneStage;
using bloom::runtime::PreviewGpuSceneStageFunction;
using bloom::runtime::PreviewGpuSceneStageOutcome;
using bloom::runtime::PreviewGpuSceneStageOutcomeHandle;
using bloom::runtime::PreviewGpuSceneStageStatus;
using bloom::runtime::PreviewPreparationResult;
using bloom::runtime::PreviewPreparationResultHandle;
using bloom::runtime::PreviewPreparationStatus;
using bloom::runtime::PreviewRequestIdentity;
using bloom::runtime::PreviewResolutionPolicy;
using bloom::runtime::TaskContext;
using bloom::runtime::TaskDiagnostic;
using bloom::runtime::TaskHandle;
using bloom::runtime::TaskOwner;
using bloom::runtime::TaskOwnerId;
using bloom::runtime::TaskOwnerKind;
using bloom::runtime::TaskRequest;
using bloom::runtime::TaskResult;
using bloom::runtime::TaskScheduler;
using bloom::runtime::TaskSchedulerConfig;
using bloom::runtime::TaskState;
using bloom::runtime::TaskSubmissionStatus;

constexpr std::size_t kBudget = std::size_t{1} << 28;

[[nodiscard]] inline TaskSchedulerConfig schedulerConfig() {
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

[[nodiscard]] inline bloom::document::CompositionFormat format1920x1080NonSquare() {
    const auto aspect = bloom::core::PixelAspectRatio::create(4, 3);
    if (!aspect.has_value()) {
        std::abort();
    }
    const auto format = bloom::document::CompositionFormat::create(1920U, 1080U, *aspect);
    if (!format.has_value()) {
        std::abort();
    }
    return *format;
}

[[nodiscard]] inline std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle>
makeProcessor() {
    auto resolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    if (!resolution.ready()) {
        return nullptr;
    }
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return nullptr;
    }
    auto built = bloom::color::buildBloomNeutralCpuDisplayProcessor(*resolved);
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

[[nodiscard]] inline PreviewRequestIdentity
makeIdentity(const std::uint64_t generation, const bloom::document::Snapshot& snapshot,
             const bloom::document::CompositionId compositionId) {
    return PreviewRequestIdentity{.projectId = snapshot.project().id(),
                                  .compositionId = compositionId,
                                  .sourceRevision = snapshot.revision(),
                                  .requestGeneration = generation,
                                  .time = RationalTime::fromInteger(0),
                                  .output = bloom::runtime::PreviewOutput::Composition,
                                  .resolution = bloom::runtime::CompositionFormatResolution{},
                                  .quality = EvaluationQuality::Reference,
                                  .colorIntent = EvaluationColorIntent::LinearRec709Scene,
                                  .resolutionPolicy = PreviewResolutionPolicy::Auto,
                                  .viewAdjust = bloom::runtime::ViewAdjust{},
                                  .displayName = {},
                                  .viewName = {},
                                  .showLook = true};
}

[[nodiscard]] inline GpuPreviewDisplayServiceOptions
serviceOptions(const std::filesystem::path& loader) {
    GpuPreviewDisplayServiceOptions options;
    options.enabled = true;
    options.loaderPath = loader;
    options.presentation = GpuPreviewDisplayServicePresentationMode::Wayland;
    options.previewByteAllowance = kBudget;
    options.presentationCoordinator.maxTargets = 3U;
    options.presentationCoordinator.maxRetainedTargets = 64U;
    options.presentationCoordinator.shutdownDrainPumps = 600U;
    return options;
}

[[nodiscard]] inline GpuPresentationUpdate makeUpdate(const GpuResidentFrameLease& lease,
                                                      const std::uint32_t targetWidth,
                                                      const std::uint32_t targetHeight) {
    GpuPresentationUpdate update;
    update.lease = lease;
    update.params.targetWidth = targetWidth;
    update.params.targetHeight = targetHeight;
    update.params.destination = GpuPresentRect{0.0F, 0.0F, static_cast<float>(targetWidth),
                                               static_cast<float>(targetHeight)};
    const auto window = lease.displayWindow();
    update.params.source =
        window.has_value() ? GpuPresentSourceWindow{static_cast<double>(window->originX()),
                                                    static_cast<double>(window->originY()),
                                                    static_cast<double>(window->extent().width()),
                                                    static_cast<double>(window->extent().height())}
                           : GpuPresentSourceWindow{0.0, 0.0, static_cast<double>(lease.width()),
                                                    static_cast<double>(lease.height())};
    update.params.channel = GpuPresentChannel::Rgba;
    update.params.background = GpuPresentBackground::Solid;
    return update;
}

// The shared, real, device-free fixture: compiler + evaluator + qualified provider + the one media
// builder whose context follows the evaluator's live asset base directory.
struct Fixture final {
    bloom::runtime::NodeDefinitionRegistry definitions;
    bloom::runtime::SnapshotCompiler compiler;
    CpuCompositionEvaluator evaluator;
    bloom::runtime::QualifiedDisplayProcessorProvider provider;
    std::shared_ptr<CpuGpuSceneBuilder> sceneBuilder;
    std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle> processor;
    std::filesystem::path mediaDirectory;
    bloom::document::AssetRecord mediaAsset;

    explicit Fixture(const std::filesystem::path& directory)
        : compiler(definitions), mediaDirectory(directory) {
        if (!bloom::runtime::registerBuiltInNodeDefinitions(definitions)) {
            std::abort();
        }
        definitions.freeze();
        processor = makeProcessor();
        if (processor != nullptr) {
            provider.publish(bloom::runtime::buildBloomNeutralQualifiedDisplayProcessor());
        }
        evaluator.setAssetBaseDirectory(mediaDirectory);
        auto mediaContext = bloom::runtime::GpuSceneMediaContext::fromEvaluator(evaluator);
        mediaContext.assetBaseDirectory = mediaDirectory;
        sceneBuilder = std::make_shared<CpuGpuSceneBuilder>(nullptr, std::move(mediaContext));
        const auto exr = mediaDirectory / "vertical-proof-still.exr";
        writeExrRgba(exr, 64, 48, stillPixels());
        mediaAsset = imageAsset(exr, "vertical-proof-still", 900);
    }

    // The unmodified production GPU-scene stage factory over the ONE media-aware builder. It
    // compiles whatever immutable snapshot it is handed, for every request generation.
    [[nodiscard]] PreviewGpuSceneStageFunction productionGpuStage() const {
        return makeCompositionPreviewGpuSceneStage(compiler, *sceneBuilder, provider);
    }

    [[nodiscard]] PreviewCpuStageFunction productionCpuStage() const {
        return makeCompositionPreviewCpuStage(compiler, evaluator, provider);
    }

    [[nodiscard]] PreviewCpuDisplayFallback
    productionCpuFallback(const bloom::runtime::CpuReferenceDisplayPreparer& preparer) const {
        return makeCompositionPreviewCpuDisplayFallback(preparer);
    }
};

[[nodiscard]] inline std::shared_ptr<const runtime::PreparedPreviewFrame>
residentFrameOf(const TaskResult<PreviewPreparationResultHandle>& result) {
    if (!result.value().has_value()) {
        return nullptr;
    }
    const auto& prepared = result.value().value();
    if (prepared->status() != PreviewPreparationStatus::Prepared || prepared->frame() == nullptr) {
        return nullptr;
    }
    return prepared->frame();
}

[[nodiscard]] inline bool
isResident(const std::shared_ptr<const runtime::PreparedPreviewFrame>& frame) {
    return frame != nullptr && frame->provenance().provider == PreviewDisplayProvider::GpuResident;
}

[[nodiscard]] inline bool
hasCpuPixels(const std::shared_ptr<const runtime::PreparedPreviewFrame>& frame) {
    return frame != nullptr && frame->displayBufferView().has_value();
}

// Evaluate one immutable plan with the fixture evaluator and convert it with the fixture qualified
// display processor. This is the shared CPU oracle body.
[[nodiscard]] inline std::shared_ptr<const runtime::PreparedPreviewFrame>
planCpuReference(Fixture& fixture,
                 const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
                 const PreviewRequestIdentity& identity) {
    if (plan == nullptr || fixture.processor == nullptr) {
        return nullptr;
    }
    const auto evaluated =
        fixture.evaluator.evaluate(plan,
                                   {.time = identity.time,
                                    .output = plan->output(),
                                    .resolution = runtime::CompositionFormatResolution{},
                                    .quality = EvaluationQuality::Reference,
                                    .colorIntent = EvaluationColorIntent::LinearRec709Scene,
                                    .pixelStorageByteLimit = kBudget},
                                   {});
    if (evaluated.status() != runtime::EvaluationStatus::Evaluated ||
        evaluated.frame() == nullptr) {
        return nullptr;
    }
    CpuQualifiedDisplayPreparer preparer(*fixture.processor);
    runtime::QualifiedDisplayPreparationRequest request;
    request.aggregatePixelStorageByteLimit = kBudget;
    const auto prepared = preparer.prepare(evaluated.frame(), request, {});
    if (prepared.status() != runtime::QualifiedDisplayPreparationStatus::Prepared ||
        prepared.frame() == nullptr) {
        return nullptr;
    }
    auto frame = runtime::PreparedPreviewFrame::createQualified(identity.requestGeneration,
                                                                prepared.frame());
    if (!frame.has_value()) {
        return nullptr;
    }
    return std::make_shared<const runtime::PreparedPreviewFrame>(std::move(*frame));
}

// The genuine document CPU reference: compile the live snapshot and evaluate it with the fixture
// evaluator. This is the oracle the resident pixels are compared against.
[[nodiscard]] inline std::shared_ptr<const runtime::PreparedPreviewFrame>
snapshotCpuReference(Fixture& fixture, const bloom::document::Snapshot& snapshot,
                     const PreviewRequestIdentity& identity) {
    const auto compile = fixture.compiler.compile(
        {.snapshot = snapshot, .compositionId = identity.compositionId, .parameterOverrides = {}},
        {});
    if (compile.status != runtime::SnapshotCompileStatus::Compiled || compile.plan == nullptr) {
        return nullptr;
    }
    return planCpuReference(fixture, compile.plan, identity);
}

[[nodiscard]] inline bool
samePixels(const std::shared_ptr<const runtime::PreparedPreviewFrame>& a,
           const std::shared_ptr<const runtime::PreparedPreviewFrame>& b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    const auto va = a->displayBufferView();
    const auto vb = b->displayBufferView();
    if (!va.has_value() || !vb.has_value() || va->pixels.size() != vb->pixels.size()) {
        return false;
    }
    return std::memcmp(va->pixels.data(), vb->pixels.data(), va->pixels.size_bytes()) == 0;
}

struct UiSurface final {
    QWindow* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    [[nodiscard]] std::uint64_t bits() const noexcept {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(surface));
    }
};

} // namespace bloom::ui::verticalproof
