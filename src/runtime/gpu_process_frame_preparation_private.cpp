#include "gpu_process_frame_preparation_private.hpp"

#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace bloom::runtime::detail {
namespace {

[[nodiscard]] GpuProcessFrameDiagnosticCode
mapSceneDiagnostic(const PreparedGpuSceneDiagnosticCode code) {
    switch (code) {
    case PreparedGpuSceneDiagnosticCode::UnsupportedOperation:
    case PreparedGpuSceneDiagnosticCode::UnsupportedTransform:
    case PreparedGpuSceneDiagnosticCode::UnsupportedBlend:
    case PreparedGpuSceneDiagnosticCode::UnsupportedRequest:
    case PreparedGpuSceneDiagnosticCode::MediaUnavailable:
        return GpuProcessFrameDiagnosticCode::SceneUnsupported;
    case PreparedGpuSceneDiagnosticCode::AwaitingCapacity:
        return GpuProcessFrameDiagnosticCode::AwaitingCapacity;
    case PreparedGpuSceneDiagnosticCode::BudgetRefused:
        return GpuProcessFrameDiagnosticCode::BudgetRefused;
    case PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded:
        return GpuProcessFrameDiagnosticCode::OverBudget;
    case PreparedGpuSceneDiagnosticCode::AllocationFailure:
        return GpuProcessFrameDiagnosticCode::BadAllocation;
    case PreparedGpuSceneDiagnosticCode::Cancelled:
        return GpuProcessFrameDiagnosticCode::Cancelled;
    case PreparedGpuSceneDiagnosticCode::InvalidRequest:
    case PreparedGpuSceneDiagnosticCode::InvalidPlan:
        return GpuProcessFrameDiagnosticCode::InvalidRequest;
    default:
        return GpuProcessFrameDiagnosticCode::InternalInvariant;
    }
}

} // namespace

ExportScenePreparation
prepareExportScene(const GpuProcessFrameEvaluatorOptions& options,
                   const std::shared_ptr<const CompiledCompositionPlan>& plan,
                   const EvaluationRequest& request, const CancellationToken& cancellation,
                   const bool encodedOutput) {
    ExportScenePreparation result;
    if (cancellation.isCancellationRequested()) {
        result.status = GpuProcessFrameStatus::Cancelled;
        result.diagnosticCode = GpuProcessFrameDiagnosticCode::Cancelled;
        result.diagnosticMessage = "Final preparation was cancelled before admission";
        return result;
    }
    GpuScenePreparationPhase phase;
    result.readbackByteAllowance = options.readbackByteBudget;
    if (options.processingPhasePolicy && plan) {
        std::uint64_t pixels =
            static_cast<std::uint64_t>(plan->format().width()) * plan->format().height();
        if (const auto* proxy = std::get_if<ProxyResolution>(&request.resolution))
            pixels = static_cast<std::uint64_t>(proxy->extent.width()) * proxy->extent.height();
        if (request.roi)
            pixels = static_cast<std::uint64_t>(request.roi->extent().width()) *
                     request.roi->extent().height();
        // Reserve actual selected output overlap, with one MiB for native transfer rounding. The
        // OCIO arm's existing per-program ceiling is separate from transfer/adoption storage.
        constexpr std::uint64_t padding = 1024ULL * 1024;
        const std::uint64_t nativeProgram = encodedOutput ? 512ULL * 1024 * 1024 : 0;
        const std::uint64_t perPixel = encodedOutput ? 64 : 32;
        if (pixels >
            (std::numeric_limits<std::uint64_t>::max() - padding - nativeProgram) / perPixel) {
            result.status = GpuProcessFrameStatus::Failed;
            result.diagnosticCode = GpuProcessFrameDiagnosticCode::BudgetRefused;
            result.diagnosticMessage = "The final phase output envelope overflows";
            return result;
        }
        if (options.readbackByteBudget < nativeProgram + pixels * perPixel) {
            result.status = GpuProcessFrameStatus::Failed;
            result.diagnosticCode = GpuProcessFrameDiagnosticCode::BudgetRefused;
            result.diagnosticMessage = "The final output minimum exceeds the readback ceiling";
            return result;
        }
        result.readbackByteAllowance =
            std::min(options.readbackByteBudget, nativeProgram + pixels * perPixel + padding);
        phase.policy = options.processingPhasePolicy;
        phase.envelope = {.output = GpuProcessingPhaseOutput::FloatFinal,
                          .encodedBytes = encodedOutput ? pixels * 16 : 0,
                          .metadataBytes = 16ULL * 1024 * 1024,
                          .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                          .requestedExecutorBytes = options.requestByteBudget,
                          .minimumExecutorBytes = pixels * 16,
                          .deviceWorkingBytes = result.readbackByteAllowance};
    }
    GpuSceneMediaContext mediaContext;
    try {
        mediaContext =
            options.mediaContextProvider ? options.mediaContextProvider() : options.mediaContext;
    } catch (const std::invalid_argument& error) {
        result.status = GpuProcessFrameStatus::Failed;
        result.diagnosticCode = GpuProcessFrameDiagnosticCode::InternalInvariant;
        result.diagnosticMessage =
            "GPU media context configuration refused: " + std::string{error.what()};
        return result;
    }
    const CpuGpuSceneBuilder builder(nullptr, mediaContext,
                                     options.ocioContext != nullptr ? *options.ocioContext
                                                                    : GpuSceneOcioContext{},
                                     std::move(phase));
    auto built = builder.build(plan, request, cancellation);
    if (!built.hasValue()) {
        result.diagnosticCode = mapSceneDiagnostic(built.diagnostic.code);
        result.status = result.diagnosticCode == GpuProcessFrameDiagnosticCode::SceneUnsupported
                            ? GpuProcessFrameStatus::UnsupportedGpuSubset
                        : result.diagnosticCode == GpuProcessFrameDiagnosticCode::Cancelled
                            ? GpuProcessFrameStatus::Cancelled
                            : GpuProcessFrameStatus::Failed;
        result.diagnosticMessage = std::move(built.diagnostic.message);
        return result;
    }
    result.phaseLease = std::move(built.phaseLease);
    result.scene = std::move(built.scene);
    return result;
}

} // namespace bloom::runtime::detail
