#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/layer_bounds_query.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <exception>
#include <new>
#include <variant>

namespace bloom::runtime {
namespace {

[[nodiscard]] LayerBoundsQueryResult refused(const LayerBoundsQueryStatus status,
                                             std::string diagnostic) {
    LayerBoundsQueryResult result;
    result.status = status;
    result.diagnostic = std::move(diagnostic);
    return result;
}

[[nodiscard]] std::string firstSummary(const auto& diagnostics, std::string fallback) {
    for (const auto& diagnostic : diagnostics)
        if (!diagnostic.summary.empty())
            return diagnostic.summary;
    return fallback;
}

} // namespace

std::vector<EvaluatedOperationBounds>
layerOutputBounds(const CompiledCompositionPlan& plan,
                  const std::span<const EvaluatedOperationBounds> bounds) {
    std::vector<EvaluatedOperationBounds> result;
    const auto operations = plan.operations();
    const auto count = std::min(operations.size(), bounds.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto& entry = bounds[index];
        if (!std::holds_alternative<CompiledLayerOutput>(operations[index]) ||
            !entry.layerId.isValid() || entry.output.empty())
            continue;
        result.push_back(entry);
    }
    return result;
}

LayerBoundsQueryResult queryLayerBounds(const SnapshotCompiler& compiler,
                                        const CpuCompositionEvaluator& evaluator,
                                        const LayerBoundsQueryRequest& request,
                                        const CancellationToken& cancellation) {
    try {
        if (request.snapshot.project().findComposition(request.composition) == nullptr)
            return refused(LayerBoundsQueryStatus::Failed, "The composition does not exist");
        const auto compiled = compiler.compile(
            {.snapshot = request.snapshot, .compositionId = request.composition}, cancellation);
        switch (compiled.status) {
        case SnapshotCompileStatus::Compiled:
            break;
        case SnapshotCompileStatus::Unsupported:
            return refused(
                LayerBoundsQueryStatus::Unsupported,
                firstSummary(compiled.diagnostics, "The composition cannot be compiled"));
        case SnapshotCompileStatus::Cancelled:
            return refused(LayerBoundsQueryStatus::Cancelled, "The bounds query was cancelled");
        case SnapshotCompileStatus::Failed:
            return refused(LayerBoundsQueryStatus::Failed,
                           firstSummary(compiled.diagnostics, "The composition failed to compile"));
        }
        if (compiled.plan == nullptr)
            return refused(LayerBoundsQueryStatus::Failed, "Compilation returned no plan");
        const auto evaluated = evaluator.evaluate(
            compiled.plan,
            EvaluationRequest{.time = request.time,
                              .output = compiled.plan->output(),
                              .resolution = CompositionFormatResolution{},
                              .quality = EvaluationQuality::Reference,
                              .colorIntent = request.colorIntent,
                              .pixelStorageByteLimit = request.pixelStorageByteLimit},
            cancellation);
        switch (evaluated.status()) {
        case EvaluationStatus::Evaluated:
            break;
        case EvaluationStatus::Cancelled:
            return refused(LayerBoundsQueryStatus::Cancelled, "The bounds query was cancelled");
        case EvaluationStatus::Failed:
            return refused(
                LayerBoundsQueryStatus::Failed,
                firstSummary(evaluated.diagnostics(), "The composition failed to evaluate"));
        }
        if (evaluated.frame() == nullptr)
            return refused(LayerBoundsQueryStatus::Failed, "Evaluation returned no frame");
        LayerBoundsQueryResult result;
        result.status = LayerBoundsQueryStatus::Evaluated;
        for (auto& entry : layerOutputBounds(*compiled.plan, evaluated.frame()->evaluatedBounds()))
            if (request.layers.empty() ||
                std::ranges::find(request.layers, entry.layerId) != request.layers.end())
                result.bounds.push_back(entry);
        return result;
    } catch (const std::bad_alloc&) {
        return refused(LayerBoundsQueryStatus::Failed, "The bounds query ran out of memory");
    }
}

} // namespace bloom::runtime
