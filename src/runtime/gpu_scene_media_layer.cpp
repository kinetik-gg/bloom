#include "gpu_scene_media_layer.hpp"

#include "cpu_composition_evaluator_support.hpp"
#include "cpu_composition_resolution.hpp"
#include "gpu_scene_preparation_builders.hpp"

#include <bloom/core/blend_mode.hpp>
#include <bloom/render/image.hpp>

#include <cstddef>

namespace bloom::runtime::detail {
namespace {

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
        return GpuSceneLeafFailure{outcome.failureCode,
                                   outcome.failure.empty() ? "Media source could not be prepared"
                                                           : outcome.failure};
    }
    const auto descriptor = outcome.uploadDescriptor.value_or(*outcome.image->descriptor());
    // The layer transform samples the PROXY image the media source publishes, exactly as the CPU
    // evaluator does: under a fractional proxy the leaf's output window is the point-resample
    // output, not the full-resolution raw upload. At unit scale the two are identical.
    const auto leafWindow = outcome.resample.has_value() ? outcome.resample->output.dataWindow()
                                                         : descriptor.dataWindow();
    result.bounds.local = outcome.frameBounds.value_or(boundsForWindow(leafWindow, hScale, vScale));
    result.bounds.output = result.bounds.local;
    result.outputWindow = leafWindow;
    if (const auto error =
            chargeBytes(outcome.image->pixels().data(), outcome.image->pixelStorageByteCost())) {
        return error;
    }
    result.image = outcome.image;
    result.descriptor = descriptor;
    result.semanticKey = outcome.semanticKey;
    result.uploadSemanticKey =
        outcome.uploadSemanticKey.empty() ? outcome.semanticKey : outcome.uploadSemanticKey;
    result.program = outcome.program;
    result.resample = outcome.resample;
    return std::nullopt;
}

} // namespace

std::optional<GpuSceneLeafFailure> screenUnsupportedLayers(const CompiledCompositionPlan& plan,
                                                           const EvaluationRequest& request,
                                                           const ResolvedEvaluation& resolved,
                                                           const CancellationToken& cancellation) {
    const std::size_t operationCount = plan.operations().size();
    // One classifier per plan so a child plan several sources reference is classified once.
    NestedCompositionChainClassifier nestedClassifier;
    for (std::size_t index = 0; index < operationCount; ++index) {
        if (cancellation.isCancellationRequested()) {
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Preparation was cancelled");
        }
        if (!isGpuSceneSubsetOperation(plan.operations()[index])) {
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "A reachable operation is outside the prepared subset");
        }
        // A composition source is classified into the subset only when its REAL nested chain is
        // present, acyclic, within the depth ceiling, and inside the bounded cancellation-aware
        // scan. The child's own operations are screened when that child is built, so an unsupported
        // child still fails closed.
        if (const auto* source =
                std::get_if<CompiledCompositionSource>(&plan.operations()[index])) {
            switch (nestedClassifier.classify(*source, plan, cancellation)) {
            case NestedCompositionClassification::Supported:
                break;
            case NestedCompositionClassification::Cancelled:
                return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Preparation was cancelled");
            case NestedCompositionClassification::Unsupported:
                return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                            "A composition source without a supported nested plan is outside the "
                            "prepared subset");
            }
        }
        const auto* layer = std::get_if<CompiledLayerOutput>(&plan.operations()[index]);
        if (layer == nullptr) {
            continue;
        }
        // ADR 0024: visibility on the placement clock, operands on the layer's own clock.
        const auto operationIndex = OperationIndex::fromRaw(index);
        const auto placementTime =
            timeForClock(resolved, request.time, plan.placementClock(operationIndex));
        if (!placementTime || *placementTime < layer->inPoint ||
            (layer->outPoint.has_value() && *placementTime >= *layer->outPoint)) {
            continue;
        }
        if (!timeForClock(resolved, request.time, plan.operationClock(operationIndex)))
            continue;
        const auto& layerResolved = resolvedForClock(resolved, plan.operationClock(operationIndex));
        // Resolve only to fail closed on a genuinely unevaluable layer before any media decode.
        // Full affine, parent composition, non-Normal blends and generic (merge/layer) inputs are
        // covered by the builder, so they are no longer screened here.
        const auto position = resolveParameter(layer->position, plan, layerResolved);
        const auto anchor = resolveParameter(layer->anchor, plan, layerResolved);
        const auto scale = resolveParameter(layer->scale, plan, layerResolved);
        const auto rotation = resolveParameter(layer->rotation, plan, layerResolved);
        const auto opacity = resolveParameter(layer->opacity, plan, layerResolved);
        const auto blend = resolveParameter(*layer, layerResolved);
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

std::optional<GpuSceneLeafFailure>
buildImageColorLeaf(const CompiledImageSource& source, const EvaluationRequest& request,
                    const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                    const GpuSceneMediaContext& context, const GpuSceneOcioContext& ocioContext,
                    const std::uint64_t pixelBudget, const double hScale, const double vScale,
                    const GpuSceneMediaChargeBytes& chargeBytes,
                    const CancellationToken& cancellation, GpuSceneMediaStatistics& statistics,
                    GpuSceneUploadLeafResult& result) {
    const auto outcome = prepareImageColorLeaf(
        source, request, plan, resolved, context, ocioContext, pixelBudget,
        request.bypassOperationCache, plan.bypassOperationCache(), cancellation, statistics);
    return finishUploadLeaf(outcome, result, hScale, vScale, chargeBytes);
}

std::optional<GpuSceneLeafFailure> buildChannelShuffleColorLeaf(
    const CompiledChannelShuffle& shuffle, const EvaluationRequest& request,
    const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
    const GpuSceneMediaContext& context, const GpuSceneOcioContext& ocioContext,
    const std::uint64_t pixelBudget, const double hScale, const double vScale,
    const GpuSceneMediaChargeBytes& chargeBytes, const CancellationToken& cancellation,
    GpuSceneMediaStatistics& statistics, GpuSceneUploadLeafResult& result) {
    const auto outcome = prepareChannelShuffleColorLeaf(
        shuffle, request, plan, resolved, context, ocioContext, pixelBudget,
        request.bypassOperationCache, plan.bypassOperationCache(), cancellation, statistics);
    return finishUploadLeaf(outcome, result, hScale, vScale, chargeBytes);
}

std::optional<GpuSceneLeafFailure>
buildVideoColorLeaf(const CompiledVideoSource& source, const EvaluationRequest& request,
                    const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                    const GpuSceneMediaContext& context, const GpuSceneOcioContext& ocioContext,
                    const std::uint64_t pixelBudget, const double hScale, const double vScale,
                    const GpuSceneMediaChargeBytes& chargeBytes,
                    const CancellationToken& cancellation, GpuSceneMediaStatistics& statistics,
                    GpuSceneUploadLeafResult& result) {
    const auto outcome = prepareVideoColorLeaf(
        source, request, plan, resolved, context, ocioContext, pixelBudget,
        request.bypassOperationCache, plan.bypassOperationCache(), cancellation, statistics);
    return finishUploadLeaf(outcome, result, hScale, vScale, chargeBytes);
}

} // namespace bloom::runtime::detail
