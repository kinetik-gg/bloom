#include "gpu_scene_media_layer.hpp"

#include "cpu_composition_evaluator_support.hpp"
#include "cpu_composition_resolution.hpp"

#include <bloom/core/blend_mode.hpp>
#include <bloom/render/image.hpp>

#include <cstddef>

namespace bloom::runtime::detail {
namespace {

[[nodiscard]] bool isSubsetOperation(const CompiledOperation& operation) noexcept {
    return std::holds_alternative<CompiledSolid>(operation) ||
           std::holds_alternative<CompiledText>(operation) ||
           std::holds_alternative<CompiledShape>(operation) ||
           std::holds_alternative<CompiledImageSource>(operation) ||
           std::holds_alternative<CompiledVideoSource>(operation) ||
           std::holds_alternative<CompiledLayerOutput>(operation) ||
           std::holds_alternative<CompiledMerge>(operation) ||
           std::holds_alternative<CompiledCompositionOutput>(operation);
}

// Common tail shared by both upload leaves: derive bounds + window from the frozen image, charge
// the resident bytes, and fill the result. Returns a failure if the upload produced no descriptor
// or the allowance would be exceeded.
[[nodiscard]] std::optional<GpuSceneLeafFailure>
finishUploadLeaf(const MediaUploadOutcome& outcome, GpuSceneUploadLeafResult& result,
                 const double hScale, const double vScale,
                 const GpuSceneMediaChargeBytes& chargeBytes) {
    if (outcome.cancelled) {
        return GpuSceneLeafFailure{PreparedGpuSceneDiagnosticCode::Cancelled,
                                   "Preparation was cancelled"};
    }
    if (!outcome.failure.empty() || outcome.image == nullptr ||
        outcome.image->descriptor() == nullptr) {
        return GpuSceneLeafFailure{PreparedGpuSceneDiagnosticCode::MediaUnavailable,
                                   outcome.failure.empty() ? "Media source could not be prepared"
                                                           : outcome.failure};
    }
    const auto descriptor = *outcome.image->descriptor();
    const auto window = descriptor.dataWindow();
    result.bounds.local = boundsForWindow(window, hScale, vScale);
    result.bounds.output = result.bounds.local;
    result.outputWindow = window;
    if (const auto error = chargeBytes(window.extent().width(), window.extent().height(),
                                       sizeof(render::Rgba32f))) {
        return error;
    }
    result.image = outcome.image;
    result.descriptor = descriptor;
    result.semanticKey = outcome.semanticKey;
    return std::nullopt;
}

} // namespace

std::optional<GpuSceneLeafFailure> screenUnsupportedLayers(const CompiledCompositionPlan& plan,
                                                           const EvaluationRequest& request,
                                                           const ResolvedEvaluation& resolved,
                                                           const CancellationToken& cancellation) {
    const std::size_t operationCount = plan.operations().size();
    for (std::size_t index = 0; index < operationCount; ++index) {
        if (cancellation.isCancellationRequested()) {
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Preparation was cancelled");
        }
        if (!isSubsetOperation(plan.operations()[index])) {
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "A reachable operation is outside the prepared subset");
        }
        const auto* layer = std::get_if<CompiledLayerOutput>(&plan.operations()[index]);
        if (layer == nullptr) {
            continue;
        }
        if (request.time < layer->inPoint ||
            (layer->outPoint.has_value() && request.time >= *layer->outPoint)) {
            continue;
        }
        // Resolve only to fail closed on a genuinely unevaluable layer before any media decode.
        // Full affine, parent composition, non-Normal blends and generic (merge/layer) inputs are
        // covered by the builder, so they are no longer screened here.
        const auto position = resolveParameter(layer->position, plan, resolved);
        const auto anchor = resolveParameter(layer->anchor, plan, resolved);
        const auto scale = resolveParameter(layer->scale, plan, resolved);
        const auto rotation = resolveParameter(layer->rotation, plan, resolved);
        const auto opacity = resolveParameter(layer->opacity, plan, resolved);
        const auto blend = resolveParameter(*layer, resolved);
        if (!position || !anchor || !scale || !rotation || !opacity || !blend) {
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Layer parameters are not evaluable");
        }
    }
    return std::nullopt;
}

std::optional<GpuSceneLeafFailure>
buildImageUploadLeaf(const CompiledImageSource& source, const EvaluationRequest& request,
                     const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                     const GpuSceneMediaContext& context, const std::uint64_t pixelBudget,
                     const double hScale, const double vScale,
                     const GpuSceneMediaChargeBytes& chargeBytes,
                     const CancellationToken& cancellation, GpuSceneMediaStatistics& statistics,
                     GpuSceneUploadLeafResult& result) {
    const auto outcome = prepareImageUpload(source, request, plan, resolved, context, pixelBudget,
                                            request.bypassOperationCache,
                                            plan.bypassOperationCache(), cancellation, statistics);
    return finishUploadLeaf(outcome, result, hScale, vScale, chargeBytes);
}

std::optional<GpuSceneLeafFailure>
buildVideoUploadLeaf(const CompiledVideoSource& source, const EvaluationRequest& request,
                     const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                     const GpuSceneMediaContext& context, const std::uint64_t pixelBudget,
                     const double hScale, const double vScale,
                     const GpuSceneMediaChargeBytes& chargeBytes,
                     const CancellationToken& cancellation, GpuSceneMediaStatistics& statistics,
                     GpuSceneUploadLeafResult& result) {
    const auto outcome = prepareVideoUpload(source, request, plan, resolved, context, pixelBudget,
                                            request.bypassOperationCache,
                                            plan.bypassOperationCache(), cancellation, statistics);
    return finishUploadLeaf(outcome, result, hScale, vScale, chargeBytes);
}

} // namespace bloom::runtime::detail
