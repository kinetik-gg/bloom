#ifndef BLOOM_RUNTIME_GPU_SCENE_PREPARATION_BUILDERS_HPP
#define BLOOM_RUNTIME_GPU_SCENE_PREPARATION_BUILDERS_HPP

// Private to src/runtime. Small, coherent orchestration helpers shared by the scene builder
// translation unit: the prepared-subset classification and the reachable-operation pre-pass. Both
// live here so cpu_gpu_scene_preparation.cpp stays within the source-size budget without moving any
// per-leaf emission semantics out of their owning helpers.

#include "gpu_scene_nested.hpp"
#include "gpu_scene_preparation_common.hpp"

#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <cstddef>
#include <optional>
#include <vector>

namespace bloom::runtime::detail {

// The reachable-operation subset the builder prepares. A new CompiledOperation alternative is not
// admitted until its preparation exists, so an unsupported graph fails closed instead of being
// silently approximated.
[[nodiscard]] inline bool isGpuSceneSubsetOperation(const CompiledOperation& operation) noexcept {
    return std::holds_alternative<CompiledSolid>(operation) ||
           std::holds_alternative<CompiledText>(operation) ||
           std::holds_alternative<CompiledShape>(operation) ||
           std::holds_alternative<CompiledImageSource>(operation) ||
           std::holds_alternative<CompiledVideoSource>(operation) ||
           std::holds_alternative<CompiledImageEffect>(operation) ||
           std::holds_alternative<CompiledLayerOutput>(operation) ||
           std::holds_alternative<CompiledMerge>(operation) ||
           std::holds_alternative<CompiledCompositionOutput>(operation) ||
           std::holds_alternative<CompiledCompositionSource>(operation);
}

// Computes the reachable operation mask from the terminal output, following pixel inputs and layer
// parents, and validates every reachable operation against the prepared subset. A composition
// source is admitted only when its real nested chain is present, acyclic, within the depth ceiling,
// and inside the bounded scan; the check is cancellation-aware. A failure carries the diagnostic
// the builder should publish.
[[nodiscard]] inline std::optional<GpuSceneLeafFailure> computeReachablePreparedOperations(
    const CompiledCompositionPlan& plan, const std::size_t outputIndex,
    const CancellationToken& cancellation, std::vector<bool>& reachable) {
    const std::size_t operationCount = plan.operations().size();
    reachable.assign(operationCount, false);
    std::vector<std::size_t> pending{outputIndex};
    while (!pending.empty()) {
        if (cancellation.isCancellationRequested()) {
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Preparation was cancelled");
        }
        const auto index = pending.back();
        pending.pop_back();
        if (index >= operationCount) {
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Plan references an invalid operation");
        }
        if (reachable[index]) {
            continue;
        }
        reachable[index] = true;
        forEachInput(plan.operations()[index],
                     [&pending](const OperationIndex input) { pending.push_back(input.value()); });
        // A parent is not a pixel input, but the child's composed matrix needs the parent's matrix,
        // so the parent subtree is part of what this build must resolve. The preflight still
        // rejects a parent that is not an earlier Layer Output.
        if (const auto* layer = std::get_if<CompiledLayerOutput>(&plan.operations()[index]);
            layer && layer->parent && layer->parent->value() < operationCount) {
            pending.push_back(layer->parent->value());
        }
    }

    // One classifier is shared across every source in the plan, so a child plan several sources
    // reference is walked once.
    NestedCompositionChainClassifier nestedClassifier;
    for (std::size_t index = 0; index < operationCount; ++index) {
        if (!reachable[index]) {
            continue;
        }
        const auto& operation = plan.operations()[index];
        if (!isGpuSceneSubsetOperation(operation)) {
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "A reachable operation is outside the prepared subset");
        }
        // A composition source is inside the prepared subset only when it names a present,
        // compatible child chain. A source that does not is refused exactly as it was before nested
        // compositions were prepared at all, before any resolution or child build.
        if (const auto* source = std::get_if<CompiledCompositionSource>(&operation)) {
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
    }
    return std::nullopt;
}

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_SCENE_PREPARATION_BUILDERS_HPP
