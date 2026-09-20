#include "gpu_process_frame_preparation_private.hpp"

#include <bloom/runtime/prepared_gpu_scene.hpp>

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
                   const EvaluationRequest& request, const CancellationToken& cancellation) {
    ExportScenePreparation result;
    const GpuSceneMediaContext mediaContext =
        options.mediaContextProvider ? options.mediaContextProvider() : options.mediaContext;
    const CpuGpuSceneBuilder builder(nullptr, mediaContext,
                                     options.ocioContext != nullptr ? *options.ocioContext
                                                                    : GpuSceneOcioContext{});
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
    result.scene = std::move(built.scene);
    return result;
}

} // namespace bloom::runtime::detail
