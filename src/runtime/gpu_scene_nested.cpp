#include "gpu_scene_nested.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace bloom::runtime::detail {

NestedCompositionClassification
NestedCompositionChainClassifier::classify(const CompiledCompositionSource& source,
                                           const CompiledCompositionPlan& plan,
                                           const CancellationToken& cancellation) {
    std::vector<document::CompositionId> ancestors{plan.compositionId()};
    return classifyImpl(source, plan, 0, ancestors, cancellation);
}

NestedCompositionClassification NestedCompositionChainClassifier::classifyImpl(
    const CompiledCompositionSource& source, const CompiledCompositionPlan& plan,
    const std::size_t depth, std::vector<document::CompositionId>& ancestors,
    const CancellationToken& cancellation) {
    if (cancellation.isCancellationRequested()) {
        return NestedCompositionClassification::Cancelled;
    }
    if (depth >= kMaxNestedCompositionDepth) {
        return NestedCompositionClassification::Unsupported;
    }
    if (remainingBudget_ == 0) {
        return NestedCompositionClassification::Unsupported;
    }
    --remainingBudget_;
    if (!nestedCompositionReferenceReady(source, plan)) {
        return NestedCompositionClassification::Unsupported;
    }
    const auto& nested = plan.nestedPlans()[source.nestedPlanIndex];
    // A composition identity already on this path is a conservative cycle guard. It is checked
    // before memoization so a shared plan encountered under a path that closes a cycle still fails.
    if (std::ranges::find(ancestors, nested->compositionId()) != ancestors.end()) {
        return NestedCompositionClassification::Unsupported;
    }
    // A child plan already walked by this classifier is not walked again: a wide graph stays linear
    // in its distinct child plans instead of exponential in the nesting depth.
    if (!visited_.insert(nested.get()).second) {
        return NestedCompositionClassification::Supported;
    }
    ancestors.push_back(nested->compositionId());
    for (const auto& operation : nested->operations()) {
        const auto* childSource = std::get_if<CompiledCompositionSource>(&operation);
        if (childSource == nullptr) {
            continue;
        }
        const auto outcome =
            classifyImpl(*childSource, *nested, depth + 1, ancestors, cancellation);
        if (outcome != NestedCompositionClassification::Supported) {
            ancestors.pop_back();
            return outcome;
        }
    }
    ancestors.pop_back();
    return NestedCompositionClassification::Supported;
}

} // namespace bloom::runtime::detail
