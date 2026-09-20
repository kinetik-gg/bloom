#include "gpu_scene_nested.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace bloom::runtime::detail {
namespace {

[[nodiscard]] bool chainIsSupported(const CompiledCompositionSource& source,
                                    const CompiledCompositionPlan& plan, const std::size_t depth,
                                    std::vector<document::CompositionId>& ancestors) {
    if (depth >= kMaxNestedCompositionDepth)
        return false;
    if (!nestedCompositionReferenceReady(source, plan))
        return false;
    const auto& nested = plan.nestedPlans()[source.nestedPlanIndex];
    // A composition identity already on the ancestor chain is a dependency cycle.
    if (std::ranges::find(ancestors, nested->compositionId()) != ancestors.end())
        return false;
    ancestors.push_back(nested->compositionId());
    bool supported = true;
    for (const auto& operation : nested->operations()) {
        const auto* childSource = std::get_if<CompiledCompositionSource>(&operation);
        if (childSource == nullptr)
            continue;
        if (!chainIsSupported(*childSource, *nested, depth + 1, ancestors)) {
            supported = false;
            break;
        }
    }
    ancestors.pop_back();
    return supported;
}

} // namespace

bool nestedCompositionChainIsSupported(const CompiledCompositionSource& source,
                                       const CompiledCompositionPlan& plan) {
    std::vector<document::CompositionId> ancestors{plan.compositionId()};
    return chainIsSupported(source, plan, 0, ancestors);
}

} // namespace bloom::runtime::detail
