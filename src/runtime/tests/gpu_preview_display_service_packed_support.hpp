#pragma once
#include "gpu_display_transfer_fault.hpp"
#include "gpu_preview_display_service_general_support.hpp"
#include <algorithm>
#include <atomic>
#include <bloom/render/gpu_display_transfer.hpp>
#include <bloom/runtime/gpu_prepared_packed_preview_product.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/runtime/packed_host_retirement.hpp>
#include <mutex>
#include <stdexcept>
namespace {
#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE
// Headless fixture thread: close/join only after its service, task and product aliases die.
struct PackedRetirementScope final {
    std::shared_ptr<bloom::runtime::PackedHostRetirementOwner> owner =
        bloom::runtime::PackedHostRetirementOwner::create();
    PackedRetirementScope() {
        if (!owner->prepare())
            throw std::runtime_error("packed fixture retirement worker startup failed");
    }
    ~PackedRetirementScope() {
        owner->beginShutdown();
        if (!waitUntil([&] { return owner->exitAcknowledged(); }, 30s) || !owner->collect()) {
            std::cerr << "packed fixture retirement did not actually exit/collect\n";
            std::terminate();
        }
    }
};

// Capture the exact program-owned buffers before the final transfer allocates its staging.
// The built-in Neutral OCIO command has no LUT images; its status/optional uniform and packed
// intermediate remain charged in the owning native program cache after takeDisplayImage().
struct PackedProgramStorageProbe final {
    struct Sample final {
        bloom::render::GpuProcessingPhaseSnapshot root;
        bloom::runtime::GpuOcioExecutorCounters programs;
        std::size_t expectedBuffers = 0;
        std::size_t sampledTextures = 0;
    };
    std::mutex mutex;
    Sample sample;
    [[nodiscard]] Sample snapshot() {
        const std::lock_guard lock(mutex);
        return sample;
    }
};

void observePackedProgramStorage(
    const std::shared_ptr<bloom::runtime::detail::PreviewDisplayServiceCore>& core,
    const std::shared_ptr<bloom::runtime::GpuProcessingPhasePolicy>& policy,
    const std::shared_ptr<PackedProgramStorageProbe>& probe) {
    const std::weak_ptr<bloom::runtime::detail::PreviewDisplayServiceCore> weakCore = core;
    core->residentGeneralDisplayPollOverride = [weakCore, policy,
                                                probe](bloom::runtime::GpuOcioDisplayArm& arm) {
        const auto result = arm.poll();
        if (result == bloom::runtime::GpuOcioExecutorPollResult::Ready) {
            if (const auto owner = weakCore.lock();
                owner && owner->residentNativeActive && owner->residentNativeActive->gpuStage &&
                owner->residentNativeActive->gpuStage->displayProgram()) {
                const auto& program =
                    owner->residentNativeActive->gpuStage->displayProgram()->command->program();
                const std::lock_guard lock(probe->mutex);
                probe->sample = {.root = policy->snapshot().reservations,
                                 .programs = arm.counters(),
                                 .expectedBuffers = 2U + (program.uniformBufferSize != 0 ? 1U : 0U),
                                 .sampledTextures = program.textures.size()};
            }
        }
        return result;
    };
}

void checkPackedProgramStorage(Expectations& checks,
                               const PackedProgramStorageProbe::Sample& sample,
                               const std::uint64_t logicalPackedBytes) {
    checks.expect(sample.sampledTextures == 0 && sample.programs.cacheEntries == 1 &&
                      sample.programs.programCreations == 1 &&
                      sample.root.ownedNativeBuffers == sample.expectedBuffers &&
                      sample.root.ownedNativeBufferBytes >= sample.programs.cacheBytes &&
                      sample.root.ownedNativeBufferBytes - sample.programs.cacheBytes >=
                          logicalPackedBytes,
                  "one exact native OCIO program owns its status/uniform and packed intermediate");
}

[[nodiscard]] bloom::runtime::PreviewDisplayBufferView
packedView(const bloom::runtime::PreparedPreviewFrame& frame) {
    const auto view = frame.displayBufferView();
    if (!view)
        throw std::runtime_error("GPU packed fixture expected a valid display buffer");
    return *view;
}

[[nodiscard]] PreviewGpuSceneStageFunction packedStageFunction(
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
                                  .resolution = identity.resolution,
                                  .quality = identity.quality,
                                  .colorIntent = identity.colorIntent,
                                  .pixelStorageByteLimit = limit,
                                  .roi = identity.roi,
                                  .bypassLookNodes = !identity.showLook};
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
        auto stage = std::make_shared<const PreviewGpuSceneStage>(
            identity, built.scene, processor, std::move(program), limit,
            std::vector<TaskDiagnostic>{}, std::move(built.phaseLease));
        return R::succeeded(
            std::make_shared<const PreviewGpuSceneStageOutcome>(PreviewGpuSceneStageOutcome{
                PreviewGpuSceneStageStatus::Prepared, std::move(stage), {}}));
    };
}

std::shared_ptr<const bloom::runtime::PreparedPreviewFrame>
packedResult(const std::optional<TaskResult<PreviewPreparationResultHandle>>& result) {
    if (!result || result->state() != TaskState::Succeeded || !result->value() ||
        (**result->value()).frame() == nullptr)
        return {};
    return (**result->value()).frame();
}

void compareCpu(Expectations& checks, const bloom::runtime::PreparedPreviewFrame& frame,
                const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan,
                const bloom::color::PreparedCpuDisplayProcessorHandle& processor) {
    const auto& identity = frame.desiredIdentity();
    CpuCompositionEvaluator evaluator;
    auto evaluated = evaluator.evaluate(plan,
                                        {.time = identity.time,
                                         .output = plan->output(),
                                         .resolution = identity.resolution,
                                         .quality = identity.quality,
                                         .colorIntent = identity.colorIntent,
                                         .pixelStorageByteLimit = kBudget,
                                         .roi = identity.roi,
                                         .bypassLookNodes = !identity.showLook},
                                        {});
    checks.expect(evaluated.frame() != nullptr, "CPU oracle scene evaluated");
    if (!evaluated.frame())
        return;
    bloom::runtime::CpuQualifiedDisplayPreparer preparer(processor);
    bloom::runtime::QualifiedDisplayPreparationRequest request;
    request.aggregatePixelStorageByteLimit = kBudget;
    request.viewAdjust = identity.viewAdjust;
    auto reference = preparer.prepare(evaluated.frame(), request, {});
    checks.expect(reference.frame() != nullptr, "CPU oracle display prepared");
    if (!reference.frame())
        return;
    auto oracleFrame = bloom::runtime::PreparedPreviewFrame::createQualified(
        identity.requestGeneration, reference.frame());
    checks.expect(oracleFrame.has_value(), "CPU oracle envelope created");
    if (!oracleFrame)
        return;
    const auto oracle = oracleFrame->displayBufferView();
    const auto packed = frame.displayBufferView();
    checks.expect(packed && oracle && packed->displayWindow == oracle->displayWindow,
                  "packed display mapping matches actual CPU reference");
    if (!packed || !oracle)
        return;
    const auto data = packed->dataWindow.value_or(packed->displayWindow);
    const auto width = data.extent().width();
    const auto fullWidth = oracle->displayWindow.extent().width();
    bool equal = true;
    for (std::uint32_t y = 0; y < data.extent().height(); ++y) {
        const auto offset =
            static_cast<std::size_t>(data.originY() + y - oracle->displayWindow.originY()) *
                fullWidth +
            static_cast<std::size_t>(data.originX() - oracle->displayWindow.originX());
        equal = equal && std::ranges::equal(
                             packed->pixels.subspan(static_cast<std::size_t>(y) * width, width),
                             oracle->pixels.subspan(offset, width));
    }
    checks.expect(equal, "every transferred RGBA8 data-window pixel exactly matches CPU oracle");
}
#endif
} // namespace
