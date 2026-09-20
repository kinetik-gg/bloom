#pragma once

// Private cohesive helper for the GPU process-frame evaluator's CPU scene preparation. It runs on
// the CALLING CPU worker (never the GPU owner thread): media decode, OCIO configuration, and shader
// compilation all happen here, and the owner receives only the immutable PreparedGpuScene.

#include <bloom/runtime/gpu_process_frame.hpp>

#include <memory>
#include <string>

namespace bloom::runtime::detail {

struct ExportScenePreparation final {
    std::shared_ptr<const PreparedGpuScene> scene;
    GpuProcessFrameStatus status = GpuProcessFrameStatus::Evaluated;
    GpuProcessFrameDiagnosticCode diagnosticCode = GpuProcessFrameDiagnosticCode::None;
    std::string diagnosticMessage;
};

[[nodiscard]] ExportScenePreparation
prepareExportScene(const GpuProcessFrameEvaluatorOptions& options,
                   const std::shared_ptr<const CompiledCompositionPlan>& plan,
                   const EvaluationRequest& request, const CancellationToken& cancellation);

} // namespace bloom::runtime::detail
